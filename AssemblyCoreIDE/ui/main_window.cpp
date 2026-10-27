#include "main_window.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QVBoxLayout>

#include "artifacts.h"
#include "asmcore/blocks.h"
#include "block_editor.h"
#include "document_tab.h"
#include "block_link.h"
#include "hardware_designer.h"
#include "hardware_link.h"
#include "i18n.h"
#include "templates.h"
#include "text_util.h"

namespace acide {

namespace {
// '/'-separated absolute path. A path that is already absolute is left alone (on Windows absoluteFilePath() would prepend a drive letter
// to "/w/x", which breaks virtual file systems and tests).
std::string norm(const QString& p) {
    QString n = QDir::fromNativeSeparators(p);
    return toStd(QDir::isAbsolutePath(n) ? QDir::cleanPath(n) : QDir::fromNativeSeparators(QFileInfo(p).absoluteFilePath()));
}
}  // namespace

MainWindow::MainWindow(AppContext& ctx, PanelRegistry& panels, QWidget* parent) : QMainWindow(parent), ctx_(ctx) {
    registerBuiltinTabs();
    setObjectName("MainWindow");
    resize(1280, 800);
    auto* central = new QWidget;
    auto* lay = new QVBoxLayout(central);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    tabs_ = new QTabWidget;
    tabs_->setObjectName("editorTabs");
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    find_ = new FindBar;
    lay->addWidget(tabs_, 1);
    lay->addWidget(find_);
    setCentralWidget(central);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i) { closeEditor(i); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) { find_->setEditor(currentEditor()); updateTitle(); if (detachBlocksAct_) detachBlocksAct_->setEnabled(qobject_cast<BlockEditor*>(tabs_->currentWidget()) != nullptr); });

    buildLabel_ = new QLabel(tr("No build yet"));
    buildLabel_->setObjectName("buildStatus");
    caretLabel_ = new QLabel;
    debugLabel_ = new QLabel(tr("Debug: off"));
    debugLabel_->setObjectName("debugStatus");
    statusBar()->addPermanentWidget(debugLabel_);
    statusBar()->addPermanentWidget(buildLabel_);
    statusBar()->addPermanentWidget(caretLabel_);

    buildTimer_.setSingleShot(true);
    buildTimer_.setInterval(ctx.settings.getInt("build.debounceMs", 300));
    connect(&buildTimer_, &QTimer::timeout, this, [this] { ctx_.build.request(ctx_.workspace.snapshot()); buildLabel_->setText(tr("Building...")); });

    createActions();
    createPanels(panels);

    conns_.add(ctx.goTo, [this](const Location& l) { if (CodeEditor* e = openFile(l.file)) e->goTo(l.line, l.col, l.endCol); });
    conns_.add(ctx.status, [this](const QString& m) { statusBar()->showMessage(m, 5000); });
    conns_.add(ctx.requestBuild, [this] { buildNow(false); });
    conns_.add(ctx.workspace.documentChanged, [this](const std::string&) { scheduleBuild(); updateTitle(); });
    conns_.add(ctx.workspace.projectChanged, [this] { scheduleBuild(); updateTitle(); syncBuildMenu(); refreshGeneratedMarks(); });
    conns_.add(ctx.build.finished, [this](SnapshotPtr s) { onSnapshot(s); });
    conns_.add(ctx.debugger.updated, [this](const SimUpdate&) { updateDebugActions(); });
    conns_.add(ctx.debugger.stopped, [this](const SimUpdate& u, const DebugLocation& w) { onStopped(u, w); });
    conns_.add(ctx.debugger.session().loaded, [this](bool ok, const std::vector<std::string>& errs) {
        if (!ok) {
            QString m;
            for (auto& e : errs) m += fromStd(e) + "\n";
            ctx_.log.fire(tr("Cannot start the simulator:\n%1").arg(m));
            warn(tr("Debug"), tr("Cannot start the simulator:\n%1").arg(m));
            runAfterLoad_ = false;
        } else {
            ctx_.log.fire(tr("Debug session started"));
            for (const char* id : {"callstack", "breakpoints", "watch", "state"}) if (docks_.contains(id)) { docks_[id]->show(); docks_[id]->raise(); }
            if (runAfterLoad_) { runAfterLoad_ = false; ctx_.debugger.run(); }
        }
        updateDebugActions();
    });
    conns_.add(ctx.workspace.documentChanged, [this](const std::string&) { updateDebugActions(); });
    conns_.add(ctx.cursorMoved, [this](const std::string&, int line) {
        if (CodeEditor* e = currentEditor()) caretLabel_->setText(tr("Ln %1, Col %2").arg(line).arg(e->textCursor().positionInBlock() + 1));
    });
    updateTitle();
    updateRecentMenu();
    restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(ctx.settings.get("window.geometry"))));
    restoreState(QByteArray::fromBase64(QByteArray::fromStdString(ctx.settings.get("window.state"))));
}

MainWindow::~MainWindow() {}

void MainWindow::createActions() {
    auto act = [this](QMenu* m, const QString& text, const QKeySequence& key, auto fn) {
        QAction* a = m->addAction(text);
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, fn);
        return a;
    };
    QMenu* file = menuBar()->addMenu(tr("&File"));
    act(file, tr("&New Project..."), QKeySequence("Ctrl+Shift+N"), [this] {
        QString f = QFileDialog::getSaveFileName(this, tr("New project"), QString(), tr("AssemblyCore project (*.acproj)"));
        if (f.isEmpty()) return;
        if (!f.endsWith(".acproj")) f += ".acproj";
        newProject(f);
    });
    act(file, tr("&Open Project..."), QKeySequence("Ctrl+Shift+O"), [this] {
        QString f = QFileDialog::getOpenFileName(this, tr("Open project"), QString(), tr("AssemblyCore project (*.acproj)"));
        if (!f.isEmpty()) openProject(f);
    });
    recentMenu_ = file->addMenu(tr("Recent Projects"));
    closeProjectAct_ = act(file, tr("Close &Project"), QKeySequence("Ctrl+Shift+W"), [this] { endProject(); });
    closeProjectAct_->setEnabled(ctx_.workspace.hasProject());
    QMenu* blk = file->addMenu(tr("&Block Program"));
    blockFromAsmAct_ = act(blk, tr("Show Current File as &Blocks"), QKeySequence("Ctrl+Alt+B"), [this] { openAsBlocks(); });
    detachBlocksAct_ = act(blk, tr("Convert to &Text (Detach)"), QKeySequence(), [this] { detachBlocks(); });
    act(blk, tr("&Open Block Program..."), QKeySequence(), [this] {
        QString f = QFileDialog::getOpenFileName(this, tr("Open block program"), fromStd(ctx_.workspace.hasProject() ? ctx_.workspace.project().baseDir() : std::string()), tr("Block programs (*.acblk)"));
        if (!f.isEmpty()) openBlocks(norm(f));
    });
    act(blk, tr("&New Empty Block Program..."), QKeySequence(), [this] {
        QString f = QFileDialog::getSaveFileName(this, tr("New block program"), fromStd(ctx_.workspace.hasProject() ? ctx_.workspace.project().baseDir() : std::string()), tr("Block programs (*.acblk)"));
        if (f.isEmpty()) return;
        if (!f.endsWith(".acblk")) f += ".acblk";
        newBlocks(f);
    });
    {
        QToolBar* bt = addToolBar(tr("Blocks"));                          // always visible: this is how a program is shown as blocks
        bt->setObjectName("blocksToolbar");
        bt->addAction(blockFromAsmAct_);
    }
    QMenu* hw = file->addMenu(tr("&Hardware"));
    act(hw, tr("&New Hardware Description..."), QKeySequence(), [this] {
        if (!ctx_.workspace.hasProject()) { warn(tr("Hardware"), tr("Open or create a project first.")); return; }
        QString f = QFileDialog::getSaveFileName(this, tr("New hardware description"), fromStd(ctx_.workspace.project().baseDir()), tr("Hardware description (*.achw)"));
        if (f.isEmpty()) return;
        if (!f.endsWith(".achw")) f += ".achw";
        newHardware(f);
    });
    openHardwareAct_ = act(hw, tr("&Open Hardware Designer"), QKeySequence(), [this] {
        if (ctx_.workspace.hasProject() && !ctx_.workspace.project().hardware.empty()) openHardware(ctx_.workspace.project().absolute(ctx_.workspace.project().hardware));
    });
    openHardwareAct_->setEnabled(ctx_.workspace.hasProject() && !ctx_.workspace.project().hardware.empty());
    act(hw, tr("&Adopt Existing Definitions..."), QKeySequence(), [this] {
        if (!ctx_.workspace.hasProject()) { warn(tr("Hardware"), tr("Open or create a project first.")); return; }
        const QString dir = fromStd(ctx_.workspace.project().baseDir());
        QString inc = QFileDialog::getOpenFileName(this, tr("Assembler definitions to adopt (.inc)"), dir, tr("Assembler include (*.inc *.asm);;All files (*)"));
        if (inc.isEmpty()) return;
        QString env = QFileDialog::getOpenFileName(this, tr("Simulator environment to adopt (Cancel for none)"), dir, tr("Environment (*.cfg *.env *.ini);;All files (*)"));
        QString achw = QFileDialog::getSaveFileName(this, tr("Save the hardware description as"), dir + "/" + QFileInfo(inc).completeBaseName() + ".achw", tr("Hardware description (*.achw)"));
        if (achw.isEmpty()) return;
        if (!achw.endsWith(".achw")) achw += ".achw";
        adoptHardware(inc, env, achw);
    });
    file->addSeparator();
    act(file, tr("&Open File..."), QKeySequence::Open, [this] {
        QString f = QFileDialog::getOpenFileName(this, tr("Open file"), QString(), tr("Assembly (*.asm *.inc *.s);;Block programs (*.acblk);;All files (*)"));
        if (!f.isEmpty()) openFile(norm(f));
    });
    act(file, tr("New &File..."), QKeySequence::New, [this] {
        QString f = QFileDialog::getSaveFileName(this, tr("New file"), QString::fromStdString(ctx_.workspace.project().baseDir()), tr("Assembly (*.asm *.inc)"));
        if (f.isEmpty()) return;
        std::string p = norm(f);
        ctx_.workspace.setText(p, "");
        std::string err;
        ctx_.workspace.save(p, err);
        if (ctx_.workspace.hasProject()) {
            Project& pr = ctx_.workspace.project();
            std::string rel = relativeTo(pr.baseDir(), p);
            if (std::find(pr.sources.begin(), pr.sources.end(), rel) == pr.sources.end() && QFileInfo(f).suffix() == "asm") { pr.sources.push_back(rel); ctx_.workspace.projectEdited(); ctx_.workspace.saveProject(err); }
        }
        openFile(p);
    });
    file->addSeparator();
    act(file, tr("&Save"), QKeySequence::Save, [this] {
        CodeEditor* e = currentEditor();
        std::string err;
        if (e && !ctx_.workspace.save(e->path(), err)) warn(tr("Save"), fromStd(err));
        updateTitle();
    });
    act(file, tr("Save &All"), QKeySequence("Ctrl+Shift+S"), [this] {
        std::string err;
        if (!ctx_.workspace.saveAll(err)) warn(tr("Save"), fromStd(err));
        updateTitle();
    });
    act(file, tr("&Close Tab"), QKeySequence("Ctrl+W"), [this] { if (tabs_->currentIndex() >= 0) closeEditor(tabs_->currentIndex()); });
    file->addSeparator();
    act(file, tr("E&xit"), QKeySequence("Ctrl+Q"), [this] { close(); });

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    auto ed = [this](auto fn) { return [this, fn] { if (CodeEditor* e = currentEditor()) fn(e); }; };
    // undo / redo go to the tab in front: the block editor has its own history
    act(edit, tr("&Undo"), QKeySequence::Undo, [this] {
        if (auto* b = qobject_cast<BlockEditor*>(tabs_->currentWidget())) b->undo();
        else if (CodeEditor* e = currentEditor()) e->undo();
    });
    act(edit, tr("&Redo"), QKeySequence::Redo, [this] {
        if (auto* b = qobject_cast<BlockEditor*>(tabs_->currentWidget())) b->redo();
        else if (CodeEditor* e = currentEditor()) e->redo();
    });
    edit->addSeparator();
    auto blocksOrText = [this](void (BlockEditor::*onBlocks)(), void (CodeEditor::*onText)()) {
        return [this, onBlocks, onText] {
            if (auto* b = qobject_cast<BlockEditor*>(tabs_->currentWidget())) (b->*onBlocks)();
            else if (CodeEditor* e = currentEditor()) (e->*onText)();
        };
    };
    act(edit, tr("Cu&t"), QKeySequence::Cut, blocksOrText(&BlockEditor::cut, &CodeEditor::cut));
    act(edit, tr("&Copy"), QKeySequence::Copy, blocksOrText(&BlockEditor::copy, &CodeEditor::copy));
    act(edit, tr("&Paste"), QKeySequence::Paste, blocksOrText(&BlockEditor::paste, &CodeEditor::paste));
    edit->addSeparator();
    act(edit, tr("&Find / Replace"), QKeySequence::Find, [this] {
        CodeEditor* e = currentEditor();
        find_->setEditor(e);
        find_->open(e ? e->textCursor().selectedText() : QString());
    });
    act(edit, tr("Find &Next"), QKeySequence::FindNext, [this] { find_->setEditor(currentEditor()); find_->findNext(false); });
    act(edit, tr("Find in Pro&ject..."), QKeySequence("Ctrl+Shift+F"), [this] { findInProject(); });
    act(edit, tr("&Go to Line..."), QKeySequence("Ctrl+G"), ed([](CodeEditor* e) { e->gotoLineDialog(); }));
    edit->addSeparator();
    act(edit, tr("Toggle &Comment"), QKeySequence("Ctrl+/"), blocksOrText(&BlockEditor::toggleDisabled, &CodeEditor::toggleComment));              // (a block is "commented out" by disabling it)
    act(edit, tr("&Duplicate Line"), QKeySequence("Ctrl+D"), blocksOrText(&BlockEditor::duplicateSelection, &CodeEditor::duplicateLine));
    act(edit, tr("Move Line &Up"), QKeySequence("Alt+Up"), ed([](CodeEditor* e) { e->moveLines(-1); }));
    act(edit, tr("Move Line Dow&n"), QKeySequence("Alt+Down"), ed([](CodeEditor* e) { e->moveLines(1); }));
    edit->addSeparator();
    act(edit, tr("Go to &Definition"), QKeySequence("F12"), ed([](CodeEditor* e) { e->goToDefinition(); }));
    act(edit, tr("Find All &References"), QKeySequence("Shift+F12"), ed([](CodeEditor* e) { e->findReferences(); }));
    act(edit, tr("&Rename Symbol..."), QKeySequence("F2"), ed([](CodeEditor* e) { e->renameSymbol(); }));

    QMenu* build = menuBar()->addMenu(tr("&Build"));
    act(build, tr("&Build"), QKeySequence("F7"), [this] { buildNow(true); });
    build->addSeparator();
    auto* cm = build->addMenu(tr("Call model"));
    auto* grpC = new QActionGroup(this);
    grpC->setObjectName("callModelGroup");
    for (auto kv : {std::pair<const char*, asmcore::CallModel>{"inline", asmcore::CallModel::Inline}, {"softret", asmcore::CallModel::SoftRet}}) {
        QAction* a = cm->addAction(kv.first);
        a->setCheckable(true);
        a->setActionGroup(grpC);
        a->setChecked(ctx_.workspace.project().callModel == kv.second);
        connect(a, &QAction::triggered, this, [this, kv] { ctx_.workspace.project().callModel = kv.second; ctx_.workspace.projectEdited(); });
    }
    auto* dm = build->addMenu(tr("Data-flow check"));
    auto* grpD = new QActionGroup(this);
    grpD->setObjectName("dataflowGroup");
    for (auto kv : {std::pair<const char*, asmcore::DataflowMode>{"off", asmcore::DataflowMode::Off}, {"warn", asmcore::DataflowMode::Warn}, {"error", asmcore::DataflowMode::Error}}) {
        QAction* a = dm->addAction(kv.first);
        a->setCheckable(true);
        a->setActionGroup(grpD);
        a->setChecked(ctx_.workspace.project().dataflow == kv.second);
        connect(a, &QAction::triggered, this, [this, kv] { ctx_.workspace.project().dataflow = kv.second; ctx_.workspace.projectEdited(); });
    }

    createDebugActions();
    menuBar()->addMenu(tr("&View"))->setObjectName("viewMenu");
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    act(help, tr("&About"), QKeySequence(), [this] {
        QMessageBox::about(this, tr("About AssemblyCoreIDE"), tr("AssemblyCoreIDE\nAn IDE for the Sensor Micro-Sequencer ISA, built on the AssemblyCore assembler."));
    });
}

// The radio items of the Build menu follow the project (opening another project changes them).
void MainWindow::syncBuildMenu() {
    const Project& p = ctx_.workspace.project();
    if (auto* g = findChild<QActionGroup*>("callModelGroup"))
        for (QAction* a : g->actions()) a->setChecked(a->text() == (p.callModel == asmcore::CallModel::SoftRet ? "softret" : "inline"));
    if (auto* g = findChild<QActionGroup*>("dataflowGroup"))
        for (QAction* a : g->actions()) a->setChecked(a->text() == (p.dataflow == asmcore::DataflowMode::Off ? "off" : p.dataflow == asmcore::DataflowMode::Error ? "error" : "warn"));
}

// ---- language ------------------------------------------------------------------------------------------------------------------------
void MainWindow::createLanguageMenu(QMenu* view) {
    view->addSeparator();
    QMenu* lang = view->addMenu(tr("&Language"));
    lang->setObjectName("languageMenu");
    auto* grp = new QActionGroup(this);
    grp->setObjectName("languageGroup");
    auto add = [&](const QString& text, const std::string& code) {
        QAction* a = lang->addAction(text);
        a->setCheckable(true);
        a->setActionGroup(grp);
        a->setData(QString::fromStdString(code));
        connect(a, &QAction::triggered, this, [this, code] { ctx_.languageRequested.fire(code); });
    };
    add(tr("Automatic (system language)"), "auto");
    for (const LanguageInfo& l : availableLanguages()) add(l.nativeName, l.code);
    refreshLanguageMenu();
}

void MainWindow::refreshLanguageMenu() {
    auto* g = findChild<QActionGroup*>("languageGroup");
    if (!g) return;
    const QString cur = QString::fromStdString(ctx_.settings.get("ui.language", "auto"));
    for (QAction* a : g->actions()) a->setChecked(a->data().toString() == cur);
}

void MainWindow::saveWindowState() {
    ctx_.settings.set("window.geometry", saveGeometry().toBase64().toStdString());
    ctx_.settings.set("window.state", saveState().toBase64().toStdString());
}

bool MainWindow::prepareForRebuild() {
    saveSession();
    return closeProject();
}

void MainWindow::createPanels(PanelRegistry& panels) {
    QMenu* view = menuBar()->findChild<QMenu*>("viewMenu");
    for (const PanelSpec& spec : panels.specs()) {
        QWidget* w = spec.create(ctx_);
        auto* d = new QDockWidget(QCoreApplication::translate("QObject", spec.title), this);
        d->setObjectName("dock_" + spec.id);
        d->setWidget(w);
        addDockWidget(spec.area, d);
        d->setVisible(spec.visible);
        docks_[spec.id] = d;
        if (view) view->addAction(d->toggleViewAction());
    }
    if (view) createLanguageMenu(view);
    // tab the bottom docks together
    QDockWidget* prev = nullptr;
    for (const PanelSpec& spec : panels.specs()) {
        if (spec.area != Qt::BottomDockWidgetArea) continue;
        QDockWidget* d = docks_[spec.id];
        if (prev) tabifyDockWidget(prev, d);
        prev = d;
    }
    // the debugging panels on the right share one tab area under the listing
    QDockWidget* prevRight = nullptr;
    for (const PanelSpec& spec : panels.specs()) {
        if (spec.area != Qt::RightDockWidgetArea || spec.id == "listing") continue;
        QDockWidget* d = docks_[spec.id];
        if (prevRight) tabifyDockWidget(prevRight, d);
        prevRight = d;
    }
    if (docks_.contains("problems")) docks_["problems"]->raise();
    // sensible first-start widths (a saved window state overrides them)
    QList<QDockWidget*> left, right;
    for (const PanelSpec& spec : panels.specs()) {
        if (spec.area == Qt::LeftDockWidgetArea) left << docks_[spec.id];
        if (spec.area == Qt::RightDockWidgetArea) right << docks_[spec.id];
    }
    if (!left.isEmpty()) resizeDocks(left, QList<int>(left.size(), 240), Qt::Horizontal);
    if (!right.isEmpty()) resizeDocks(right, QList<int>(right.size(), 420), Qt::Horizontal);
    if (docks_.contains("listing") && docks_.contains("state")) resizeDocks({docks_["listing"], docks_["state"]}, {260, 340}, Qt::Vertical);
}

// ---- files and projects ------------------------------------------------------------------------------------------------------------
CodeEditor* MainWindow::currentEditor() const { return qobject_cast<CodeEditor*>(tabs_->currentWidget()); }

HardwareDesigner* MainWindow::currentDesigner() const { return qobject_cast<HardwareDesigner*>(tabs_->currentWidget()); }

QWidget* MainWindow::openTab(const std::string& path) {
    auto it = docTabs_.find(path);
    if (it != docTabs_.end()) { tabs_->setCurrentWidget(it->second); return it->second; }
    const TabKind* kind = TabRegistry::instance().find(path);
    if (!kind) return nullptr;
    if (!ctx_.workspace.open(path)) {
        warn(tr("Open"), tr("Cannot read %1").arg(fromStd(path)));
        return nullptr;
    }
    QWidget* w = kind->create(ctx_, path);
    auto* tab = dynamic_cast<DocumentTab*>(w);
    docTabs_[path] = w;
    int idx = tabs_->addTab(w, tab ? tab->tabTitle() : QFileInfo(fromStd(path)).fileName());
    tabs_->setTabToolTip(idx, fromStd(path));
    tabs_->setCurrentIndex(idx);
    return w;
}

HardwareDesigner* MainWindow::openHardware(const std::string& path) { return qobject_cast<HardwareDesigner*>(openTab(path)); }

BlockEditor* MainWindow::openBlocks(const std::string& path) { return qobject_cast<BlockEditor*>(openTab(path)); }

// Turns the assembler text of an editor into a block program (importAsm never fails: lines it does not know become text blocks) saved next to it
// as NAME.acblk, and shows it. If NAME.acblk already exists it is opened as it is -- the blocks are never overwritten by the text.
BlockEditor* MainWindow::openAsBlocks() {
    CodeEditor* e = currentEditor();
    if (!e) {
        warn(tr("Blocks"), tr("Open an assembler file first (or use File > Block Program > Open Block Program)."));
        return nullptr;
    }
    const std::string asmPath = e->path();
    const size_t dot = asmPath.find_last_of('.');
    const size_t slash = asmPath.find_last_of('/');
    const std::string base = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? asmPath.substr(0, dot) : asmPath;
    const std::string blkPath = base + ".acblk";
    if (!ctx_.workspace.fileSystem()->exists(blkPath) && !ctx_.workspace.find(blkPath)) {
        asmcore::blocks::ImportStats st;
        asmcore::blocks::Document d = asmcore::blocks::importAsm(e->toPlainText().toStdString(), QFileInfo(fromStd(base)).fileName().toStdString(), &st);
        d.output = QFileInfo(fromStd(asmPath)).fileName().toStdString();      // the file the blocks generate is the one they came from: the project and every .include stay as they are
        const std::string backup = asmPath + ".bak";                           // (the original text is kept once, the first time blocks replace it)
        if (!ctx_.workspace.fileSystem()->exists(backup)) ctx_.workspace.fileSystem()->write(backup, e->toPlainText().toStdString());
        ctx_.workspace.setText(blkPath, asmcore::blocks::serializeDocument(d));
        std::string err;
        if (!ctx_.workspace.save(blkPath, err)) { warn(tr("Blocks"), fromStd(err)); return nullptr; }
        ctx_.log.fire(tr("Created %1 from %2: %3 blocks, %4 lines kept as text").arg(fromStd(blkPath)).arg(QFileInfo(fromStd(asmPath)).fileName()).arg(st.blocks).arg(st.raw));
    }
    return openBlocks(blkPath);
}

// Releases the block program of the current tab: the generated file becomes an ordinary assembler file again (without the "generated" header), the
// program stays on disk but leaves the project. The text editor of the file opens.
bool MainWindow::detachBlocks() {
    auto* be = qobject_cast<BlockEditor*>(tabs_->currentWidget());
    if (!be) { warn(tr("Blocks"), tr("Open a block program first.")); return false; }
    const std::string blk = be->tabPath(), gen = be->generatedPath();
    if (ask(tr("Convert to text"), tr("%1 becomes an ordinary assembler file again and %2 leaves the project (the file stays on disk). Continue?").arg(QFileInfo(fromStd(gen)).fileName()).arg(QFileInfo(fromStd(blk)).fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes) return false;
    std::string text = be->session().generated().text;
    const std::string head = "; " + generatedHeader(blk) + "\n";
    if (text.compare(0, head.size(), head) == 0) { text.erase(0, head.size()); if (!text.empty() && text[0] == '\n') text.erase(0, 1); }
    std::string err;
    be->prepareToClose();
    if (!ctx_.workspace.save(blk, err)) { warn(tr("Blocks"), fromStd(err)); return false; }
    ctx_.workspace.setText(gen, text);
    if (!ctx_.workspace.save(gen, err)) { warn(tr("Blocks"), fromStd(err)); return false; }
    if (!acide::detachBlocks(ctx_.workspace, blk, err)) { warn(tr("Blocks"), fromStd(err)); return false; }
    closeDocTab(tabs_->indexOf(be));
    ctx_.log.fire(tr("%1 is plain assembler text again; %2 is no longer part of the project").arg(fromStd(gen)).arg(fromStd(blk)));
    openFile(gen);
    refreshGeneratedMarks();
    return true;
}

bool MainWindow::newBlocks(const QString& file) {
    const std::string path = norm(file);
    asmcore::blocks::Document d = asmcore::blocks::importAsm(".main\n", QFileInfo(file).completeBaseName().toStdString());
    ctx_.workspace.setText(path, asmcore::blocks::serializeDocument(d));
    std::string err;
    if (!ctx_.workspace.save(path, err)) { warn(tr("Blocks"), fromStd(err)); return false; }
    updateTitle();
    return openBlocks(path) != nullptr;
}

bool MainWindow::newHardware(const QString& file) {
    std::string err;
    const std::string path = norm(file);
    if (!createHardware(ctx_.workspace, path, err)) { warn(tr("Hardware"), fromStd(err)); return false; }
    ctx_.log.fire(tr("Created hardware description %1").arg(fromStd(path)));
    updateTitle();
    openHardware(path);
    return true;
}

bool MainWindow::adoptHardware(const QString& incFile, const QString& envFile, const QString& achwFile) {
    std::string err;
    std::vector<std::string> msgs;
    const std::string path = norm(achwFile);
    if (!acide::adoptHardware(ctx_.workspace, path, norm(incFile), envFile.isEmpty() ? std::string() : norm(envFile), msgs, err)) { warn(tr("Hardware"), fromStd(err)); return false; }
    for (auto& m : msgs) ctx_.log.fire(tr("Adopt: %1").arg(fromStd(m)));
    ctx_.log.fire(tr("Adopted %1: the definitions and the environment are now generated from the description; save to replace the originals.").arg(QFileInfo(incFile).fileName()));
    updateTitle();
    openHardware(path);
    return true;
}

CodeEditor* MainWindow::openFile(const std::string& path) {
    if (TabRegistry::instance().find(path)) { openTab(path); return nullptr; }
    auto it = editors_.find(path);
    if (it != editors_.end()) { tabs_->setCurrentWidget(it->second); return it->second; }
    if (!ctx_.workspace.open(path)) {
        warn(tr("Open"), tr("Cannot read %1").arg(fromStd(path)));
        return nullptr;
    }
    auto* e = new CodeEditor(ctx_, path);
    editors_[path] = e;
    connect(e, &CodeEditor::openBlocksRequested, this, [this](const QString& acblk) { openBlocks(toStd(acblk)); });
    {
        const auto gen = generatedFiles(ctx_.workspace);
        auto g = gen.find(path);
        if (g != gen.end()) e->setGeneratedFrom(g->second);
    }
    int idx = tabs_->addTab(e, QFileInfo(fromStd(path)).fileName());
    tabs_->setTabToolTip(idx, fromStd(path));
    tabs_->setCurrentIndex(idx);
    e->setSnapshot(ctx_.snapshot);
    connect(e, &CodeEditor::caretLineChanged, this, [this, e](int line) {
        if (e == currentEditor()) caretLabel_->setText(tr("Ln %1, Col %2").arg(line).arg(e->textCursor().positionInBlock() + 1));
    });
    find_->setEditor(e);
    e->setFocus();
    return e;
}

// Text editors of files generated from blocks are read-only (and say where the file comes from); the others are ordinary again.
void MainWindow::refreshGeneratedMarks() {
    const auto gen = generatedFiles(ctx_.workspace);
    for (auto& kv : editors_) {
        auto g = gen.find(kv.first);
        kv.second->setGeneratedFrom(g == gen.end() ? std::string() : g->second);
    }
}

bool MainWindow::closeDocTab(int index) {
    QWidget* w = tabs_->widget(index);
    auto* tab = dynamic_cast<DocumentTab*>(w);
    if (!tab) return true;
    tab->prepareToClose();
    const std::string path = tab->tabPath();
    const std::vector<std::string> paths = tab->workspacePaths();
    bool dirty = false;
    for (const std::string& p : paths) if (Document* doc = ctx_.workspace.find(p)) dirty |= doc->dirty;
    if (dirty) {
        auto r = ask(tr("Unsaved changes"), tr("Save changes to %1?").arg(QFileInfo(fromStd(path)).fileName()),
                     QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Discard);
        if (r == QMessageBox::Cancel) return false;
        std::string err;
        if (r == QMessageBox::Save) {
            for (const std::string& p : paths) if (ctx_.workspace.find(p) && !ctx_.workspace.save(p, err)) { warn(tr("Save"), fromStd(err)); return false; }
        } else {
            for (const std::string& p : paths) {                  // throw the edits away: the files on disk are the truth again
                if (!ctx_.workspace.find(p)) continue;
                if (ctx_.workspace.fileSystem()->exists(p)) ctx_.workspace.reload(p); else ctx_.workspace.close(p);
            }
        }
    }
    tabs_->removeTab(index);
    docTabs_.erase(path);
    delete w;
    ctx_.workspace.close(path);
    return true;
}

bool MainWindow::closeEditor(int index) {
    if (dynamic_cast<DocumentTab*>(tabs_->widget(index))) return closeDocTab(index);
    auto* e = qobject_cast<CodeEditor*>(tabs_->widget(index));
    if (!e) return true;
    Document* d = ctx_.workspace.find(e->path());
    if (d && d->dirty) {
        auto r = ask(tr("Unsaved changes"), tr("Save changes to %1?").arg(QFileInfo(fromStd(e->path())).fileName()),
                     QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Discard);
        if (r == QMessageBox::Cancel) return false;
        std::string err;
        if (r == QMessageBox::Save && !ctx_.workspace.save(e->path(), err)) { warn(tr("Save"), fromStd(err)); return false; }
    }
    std::string path = e->path();
    tabs_->removeTab(index);
    editors_.erase(path);
    delete e;
    ctx_.workspace.close(path);
    return true;
}

bool MainWindow::maybeSaveAll() {
    if (!ctx_.workspace.anyDirty()) return true;
    auto r = ask(tr("Unsaved changes"), tr("Save all changes?"), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Discard);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) {
        std::string err;
        if (!ctx_.workspace.saveAll(err)) { warn(tr("Save"), fromStd(err)); return false; }
    }
    return true;
}

bool MainWindow::closeProject() {
    if (!maybeSaveAll()) return false;
    ctx_.debugger.saveBreakpoints();
    ctx_.debugger.stop();
    while (tabs_->count()) {
        if (auto* tab = dynamic_cast<DocumentTab*>(tabs_->widget(0))) {
            QWidget* w = tabs_->widget(0);
            const std::string tpath = tab->tabPath();
            tabs_->removeTab(0);
            docTabs_.erase(tpath);
            delete w;
            ctx_.workspace.close(tpath);
            continue;
        }
        auto* e = qobject_cast<CodeEditor*>(tabs_->widget(0));
        std::string path = e->path();
        tabs_->removeTab(0);
        editors_.erase(path);
        delete e;
        ctx_.workspace.close(path);
    }
    return true;
}

void MainWindow::endProject() {
    if (!ctx_.workspace.hasProject()) return;
    const QString name = fromStd(ctx_.workspace.project().name);
    saveSession();                                  // so that opening the project again brings the same files back
    if (!closeProject()) return;                    // asks about unsaved changes; Cancel keeps everything as it was
    ctx_.workspace.closeProject();
    ctx_.snapshot.reset();
    ctx_.log.fire(tr("Closed project %1").arg(name));
    updateTitle();
}

bool MainWindow::openProject(const QString& file) {
    if (!closeProject()) return false;
    std::vector<std::string> errs;
    std::string path = norm(file);
    if (!ctx_.workspace.openProject(path, errs)) {
        QString m;
        for (auto& e : errs) m += fromStd(e) + "\n";
        warn(tr("Open project"), m);
        return false;
    }
    ctx_.settings.pushRecent("recent.projects", path);
    updateRecentMenu();
    ctx_.snapshot.reset();
    ctx_.debugger.loadBreakpoints();
    restoreSession();
    if (tabs_->count() == 0 && !ctx_.workspace.project().sources.empty()) openFile(ctx_.workspace.project().absolute(ctx_.workspace.project().sources[0]));
    ctx_.log.fire(tr("Opened project %1").arg(fromStd(path)));
    updateTitle();
    return true;
}

bool MainWindow::newProject(const QString& file) {
    if (!closeProject()) return false;
    std::string path = norm(file);
    std::string name = toStd(QFileInfo(file).completeBaseName());
    ctx_.workspace.newProject(path, name);
    Project& p = ctx_.workspace.project();
    p.sources.push_back("main.asm");
    std::string err;
    std::string main = p.absolute("main.asm");
    if (!ctx_.workspace.fileSystem()->exists(main)) {
        ctx_.workspace.setText(main, newProgramText(name));
        ctx_.workspace.save(main, err);
    }
    ctx_.workspace.projectEdited();
    if (!ctx_.workspace.saveProject(err)) { warn(tr("New project"), fromStd(err)); return false; }
    ctx_.settings.pushRecent("recent.projects", path);
    updateRecentMenu();
    ctx_.snapshot.reset();
    openFile(main);
    updateTitle();
    return true;
}

void MainWindow::updateRecentMenu() {
    if (!recentMenu_) return;
    recentMenu_->clear();
    for (const std::string& p : ctx_.settings.getList("recent.projects")) {
        QAction* a = recentMenu_->addAction(fromStd(p));
        connect(a, &QAction::triggered, this, [this, p] { openProject(fromStd(p)); });
    }
    recentMenu_->setEnabled(!recentMenu_->isEmpty());
}

void MainWindow::restoreSession() {
    for (const std::string& p : ctx_.settings.getList("session." + ctx_.workspace.project().file))
        if (ctx_.workspace.fileSystem()->exists(p)) openFile(p);
}

void MainWindow::saveSession() {
    if (!ctx_.workspace.hasProject()) return;
    std::vector<std::string> open;
    for (int i = 0; i < tabs_->count(); i++) {
        if (auto* e = qobject_cast<CodeEditor*>(tabs_->widget(i))) open.push_back(e->path());
        else if (auto* d = dynamic_cast<DocumentTab*>(tabs_->widget(i))) open.push_back(d->tabPath());
    }
    ctx_.settings.setList("session." + ctx_.workspace.project().file, open);
}

// Dialogs go through these two so that a test (setQuiet) never blocks on a modal window: warnings are logged, questions get `quietAnswer`.
void MainWindow::warn(const QString& title, const QString& text) {
    if (quiet_) { ctx_.log.fire(title + ": " + text); warnings_ << text; return; }
    QMessageBox::warning(this, title, text);
}

QMessageBox::StandardButton MainWindow::ask(const QString& title, const QString& text, QMessageBox::StandardButtons buttons, QMessageBox::StandardButton quietAnswer) {
    if (quiet_) { ctx_.log.fire(title + ": " + text); return quietAnswer; }
    return QMessageBox::question(this, title, text, buttons);
}

void MainWindow::updateTitle() {
    QString t = "AssemblyCoreIDE";
    if (ctx_.workspace.hasProject()) t = fromStd(ctx_.workspace.project().name) + " - " + t;
    if (closeProjectAct_) closeProjectAct_->setEnabled(ctx_.workspace.hasProject());
    if (openHardwareAct_) openHardwareAct_->setEnabled(ctx_.workspace.hasProject() && !ctx_.workspace.project().hardware.empty());
    if (ctx_.workspace.anyDirty()) t += " *";
    setWindowTitle(t);
    for (int i = 0; i < tabs_->count(); i++) {
        if (auto* tab = dynamic_cast<DocumentTab*>(tabs_->widget(i))) {
            bool dirty = false;
            for (const std::string& p : tab->workspacePaths()) if (Document* doc = ctx_.workspace.find(p)) dirty |= doc->dirty;
            tabs_->setTabText(i, dirty ? tab->tabTitle() + " *" : tab->tabTitle());
            continue;
        }
        auto* e = qobject_cast<CodeEditor*>(tabs_->widget(i));
        if (!e) continue;
        Document* d = ctx_.workspace.find(e->path());
        QString name = QFileInfo(fromStd(e->path())).fileName();
        tabs_->setTabText(i, d && d->dirty ? name + " *" : name);
    }
}

// ---- builds ------------------------------------------------------------------------------------------------------------------------
void MainWindow::scheduleBuild() {
    if (!ctx_.workspace.hasProject()) return;
    buildTimer_.start();
}

bool MainWindow::checkedLanguage(const QString& code) const {
    if (auto* g = findChild<QActionGroup*>("languageGroup"))
        for (QAction* a : g->actions()) if (a->data().toString() == code) return a->isChecked();
    return false;
}

bool MainWindow::checkedAction(const QString& group, const QString& text) const {
    if (auto* g = findChild<QActionGroup*>(group))
        for (QAction* a : g->actions()) if (a->text() == text) return a->isChecked();
    return false;
}

void MainWindow::flushBuildTimer() {
    if (buildTimer_.isActive()) { buildTimer_.stop(); ctx_.build.request(ctx_.workspace.snapshot()); }
}

void MainWindow::buildNow(bool writeOutputs) {
    if (!ctx_.workspace.hasProject()) { statusBar()->showMessage(tr("No project is open"), 4000); return; }
    buildTimer_.stop();
    if (writeOutputs) {
        std::string err;
        if (!ctx_.workspace.saveAll(err)) warn(tr("Save"), fromStd(err));
    }
    buildLabel_->setText(tr("Building..."));
    uint64_t id = ctx_.build.request(ctx_.workspace.snapshot());
    if (writeOutputs) explicitBuild_ = id;
}

void MainWindow::onSnapshot(SnapshotPtr s) {
    ctx_.snapshot = s;
    ctx_.snapshotChanged.fire(s);
    if (pendingStart_) {
        if (s->revision != ctx_.workspace.revision()) buildNow(false);        // edited meanwhile: build again
        else {
            pendingStart_ = false;
            std::string why;
            if (!ctx_.debugger.start(why)) { warn(tr("Debug"), fromStd(why)); runAfterLoad_ = false; }
        }
    }
    for (auto& kv : editors_) kv.second->setSnapshot(s);
    int e = s->result.errorCount(), w = s->result.warningCount();
    buildLabel_->setText(e ? tr("%1 error(s), %2 warning(s)").arg(e).arg(w)
                           : w ? tr("OK, %1 warning(s) - %2 ms").arg(w).arg(s->milliseconds, 0, 'f', 0) : tr("OK - %1 ms").arg(s->milliseconds, 0, 'f', 0));
    if (s->id == explicitBuild_) {
        explicitBuild_ = 0;
        ctx_.log.fire(tr("Build %1: %2 error(s), %3 warning(s)").arg(e ? tr("failed") : tr("succeeded")).arg(e).arg(w));
        for (auto& d : s->result.diags) ctx_.log.fire(fromStd(d.loc.file) + ":" + QString::number(d.loc.line) + ": " + (d.sev == asmcore::Diagnostic::Error ? "error " : "warning ") + fromStd(d.code) + ": " + fromStd(d.msg));
        if (s->result.ok) {
            std::string err;
            ArtifactOptions ao;
            ao.sources = s->sources;
            auto files = writeArtifacts(*ctx_.workspace.fileSystem(), s->prefix, s->result, ao, err);
            for (auto& f : files) ctx_.log.fire(tr("  wrote %1").arg(fromStd(f)));
            if (!err.empty()) ctx_.log.fire(tr("  cannot write %1").arg(fromStd(err)));
            for (const asmcore::WcetResult& wc : s->result.wcet) ctx_.log.fire(tr("  WCET %1: %2").arg(fromStd(wc.name), wc.bounded ? tr("%1 cycles").arg(wc.cycles) : tr("unbounded")));
        }
        if (docks_.contains("output")) docks_["output"]->show();
    }
}

// ---- search in the project -------------------------------------------------------------------------------------------------------
void MainWindow::findInProject() {
    bool ok = false;
    QString text = QInputDialog::getText(this, tr("Find in project"), tr("Text:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || text.isEmpty()) return;
    std::vector<Location> hits;
    std::string needle = toStd(text);
    auto lower = [](std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    std::string n = lower(needle);
    std::vector<std::string> files;
    for (auto& s : ctx_.workspace.project().sources) files.push_back(ctx_.workspace.project().absolute(s));
    for (auto& p : ctx_.workspace.openPaths()) if (std::find(files.begin(), files.end(), p) == files.end()) files.push_back(p);
    for (const std::string& f : files) {
        std::string t;
        if (Document* d = ctx_.workspace.find(f)) t = d->text;
        else if (!ctx_.workspace.fileSystem()->read(f, t)) continue;
        std::string lt = lower(t);
        int line = 1;
        size_t ls = 0;
        for (size_t at = lt.find(n); at != std::string::npos; at = lt.find(n, at + 1)) {
            for (size_t i = ls; i < at; i++) if (t[i] == '\n') { line++; ls = i + 1; }
            hits.push_back(Location{f, line, (int)(at - ls), (int)(at - ls + n.size())});
        }
    }
    ctx_.showReferences.fire(tr("Find \"%1\" (%2)").arg(text).arg(hits.size()), hits);
}

// ---- window events -------------------------------------------------------------------------------------------------------------------
bool MainWindow::event(QEvent* e) {
    if (e->type() == QEvent::WindowActivate) checkExternalChanges();
    return QMainWindow::event(e);
}

void MainWindow::checkExternalChanges() {
    if (askedExternal_) return;
    askedExternal_ = true;
    for (const std::string& p : ctx_.workspace.externallyChanged()) {
        auto r = ask(tr("File changed"), tr("%1 changed on disk. Reload it?").arg(fromStd(p)), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r == QMessageBox::Yes) ctx_.workspace.reload(p);
        else ctx_.workspace.keepBuffer(p);                                    // keep the edits; they now differ from the disk
    }
    askedExternal_ = false;
}

// ---- debugging ---------------------------------------------------------------------------------------------------------------------
QAction* MainWindow::debugAction(const QString& name) const {
    if (name == "start") return actStart_;
    if (name == "pause") return actPause_;
    if (name == "stop") return actStop_;
    if (name == "restart") return actRestart_;
    if (name == "into") return actInto_;
    if (name == "over") return actOver_;
    if (name == "out") return actOut_;
    if (name == "tocursor") return actToCursor_;
    if (name == "breakpoint") return actToggleBp_;
    return nullptr;
}

void MainWindow::createDebugActions() {
    QMenu* dbg = menuBar()->addMenu(tr("&Debug"));
    QToolBar* tb = addToolBar(tr("Debug"));
    tb->setObjectName("debugToolBar");
    auto make = [&](const QString& text, const QKeySequence& key, auto fn, bool toolbar = true) {
        QAction* a = dbg->addAction(text);
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, fn);
        if (toolbar) tb->addAction(a);
        return a;
    };
    actStart_ = make(tr("Start / Continue"), QKeySequence("F5"), [this] {
        if (!ctx_.debugger.active()) startDebug(true);
        else if (!ctx_.debugger.running()) ctx_.debugger.run();
    });
    actPause_ = make(tr("Pause"), QKeySequence("F6"), [this] { ctx_.debugger.pause(); });
    actStop_ = make(tr("Stop"), QKeySequence("Shift+F5"), [this] { ctx_.debugger.stop(); updateDebugActions(); });
    actRestart_ = make(tr("Restart"), QKeySequence("Ctrl+Shift+F5"), [this] { restartDebug(); });
    dbg->addSeparator();
    tb->addSeparator();
    actInto_ = make(tr("Step Into"), QKeySequence("F11"), [this] { if (!ctx_.debugger.active()) startDebug(false); else ctx_.debugger.stepInto(); });
    actOver_ = make(tr("Step Over"), QKeySequence("F10"), [this] { if (!ctx_.debugger.active()) startDebug(false); else ctx_.debugger.stepOver(); });
    actOut_ = make(tr("Step Out"), QKeySequence("Shift+F11"), [this] { ctx_.debugger.stepOut(); });
    actToCursor_ = make(tr("Run to Cursor"), QKeySequence("Ctrl+F10"), [this] {
        CodeEditor* e = currentEditor();
        if (!e || !ctx_.debugger.active()) { statusBar()->showMessage(tr("Start debugging first"), 4000); return; }
        std::string why;
        if (!ctx_.debugger.runToLine(e->path(), e->textCursor().blockNumber() + 1, why)) statusBar()->showMessage(fromStd(why), 4000);
    });
    dbg->addSeparator();
    tb->addSeparator();
    actToggleBp_ = make(tr("Toggle Breakpoint"), QKeySequence("F9"), [this] {
        CodeEditor* e = currentEditor();
        if (!e) return;
        int line = e->textCursor().blockNumber() + 1;
        int used = ctx_.debugger.toggleLineBreakpoint(e->path(), line);
        if (used == 0) statusBar()->showMessage(tr("No code on or after this line"), 4000);
        else if (used != line) statusBar()->showMessage(tr("That line has no code: breakpoint moved to line %1").arg(used), 4000);
    });
    dbg->addAction(tr("Remove All Breakpoints"), this, [this] { ctx_.debugger.clearLineBreakpoints(); });
    dbg->addSeparator();
    dbg->addAction(tr("Run for N Cycles..."), this, [this] {
        if (!ctx_.debugger.active() || ctx_.debugger.running()) { statusBar()->showMessage(tr("Start debugging first (and pause it)"), 4000); return; }
        bool ok = false;
        int n = QInputDialog::getInt(this, tr("Run for N cycles"), tr("Cycles:"), ctx_.settings.getInt("debug.runCycles", 1000), 1, 2000000000, 1, &ok);
        if (!ok) return;
        ctx_.settings.setInt("debug.runCycles", n);
        RunSpec spec;
        spec.maxCycles = (asmcore::sim::Cycle)n;
        spec.stopOnIdle = false;
        ctx_.debugger.run(spec);
    });
    dbg->addAction(tr("Run Until Expression..."), this, [this] {
        if (!ctx_.debugger.active() || ctx_.debugger.running()) { statusBar()->showMessage(tr("Start debugging first (and pause it)"), 4000); return; }
        bool ok = false;
        QString e = QInputDialog::getText(this, tr("Run until"), tr("Stop when this becomes true (e.g.  csr[0x120] == 5):"), QLineEdit::Normal, fromStd(ctx_.settings.get("debug.untilExpr")), &ok);
        if (!ok || e.trimmed().isEmpty()) return;
        ctx_.settings.set("debug.untilExpr", toStd(e));
        RunSpec spec;
        spec.until = toStd(e.trimmed());
        ctx_.debugger.run(spec);
    });
    dbg->addSeparator();
    auto* focus = dbg->addMenu(tr("Step focus"));
    auto* grp = new QActionGroup(this);
    for (auto kv : {std::pair<const char*, asmcore::Domain>{QT_TR_NOOP("Main sequencer"), asmcore::Domain::Main}, {QT_TR_NOOP("Peri sequencer"), asmcore::Domain::Peri}}) {
        QAction* a = focus->addAction(tr(kv.first));
        a->setCheckable(true);
        a->setActionGroup(grp);
        a->setChecked(kv.second == asmcore::Domain::Main);
        connect(a, &QAction::triggered, this, [this, kv] { ctx_.debugger.setFocus(kv.second); updateDebugActions(); });
    }
    dbg->addSeparator();
    dbg->addAction(tr("Edit Environment File..."), this, [this] { editEnvironmentFile(); });
    updateDebugActions();
}

void MainWindow::updateDebugActions() {
    if (!actStart_) return;
    const bool active = ctx_.debugger.active();
    const bool running = ctx_.debugger.running();
    actStart_->setText(active ? tr("Continue") : tr("Start Debugging"));
    actStart_->setEnabled(!running && ctx_.workspace.hasProject());
    actPause_->setEnabled(running);
    actStop_->setEnabled(active);
    actRestart_->setEnabled(active && !running);
    actInto_->setEnabled(!running && ctx_.workspace.hasProject());
    actOver_->setEnabled(!running && ctx_.workspace.hasProject());
    actOut_->setEnabled(active && !running);
    actToCursor_->setEnabled(active && !running);
    QString text;
    ViewPtr v = ctx_.debugger.session().view();
    if (!active) text = pendingStart_ ? tr("Debug: building...") : tr("Debug: off");
    else if (running) text = tr("Debug: running");
    else text = v ? tr("Debug: paused at cycle %1").arg((qulonglong)v->cycle) : tr("Debug: starting...");
    if (active && ctx_.debugger.stale()) text += tr("  (source changed: restart to apply)");
    debugLabel_->setText(text);
}

void MainWindow::startDebug(bool thenRun) {
    if (!ctx_.workspace.hasProject()) { statusBar()->showMessage(tr("No project is open"), 4000); return; }
    runAfterLoad_ = thenRun;
    pendingStart_ = true;                                    // onSnapshot starts the session once a build of the current text is in
    updateDebugActions();
    buildNow(false);
}

void MainWindow::restartDebug() {
    bool wasRunning = false;
    if (ctx_.debugger.active()) { wasRunning = false; ctx_.debugger.stop(); }
    startDebug(wasRunning);
}

void MainWindow::onStopped(const SimUpdate& u, const DebugLocation& where) {
    using asmcore::sim::StopReason;
    QString why;
    switch (u.stop.reason) {
        case StopReason::Breakpoint: why = tr("breakpoint"); break;
        case StopReason::Watchpoint: why = tr("watchpoint"); break;
        case StopReason::Fault: why = tr("FAULT"); break;
        case StopReason::Idle: why = tr("both sequencers are idle"); break;
        case StopReason::CycleLimit: why = tr("cycle budget used up"); break;
        case StopReason::TimeLimit: why = tr("time budget used up"); break;
        case StopReason::Cancelled: why = tr("paused"); break;
        case StopReason::Condition: why = tr("condition became true"); break;
        case StopReason::InstrLimit: why = QString(); break;           // a step: nothing to say
        default: break;
    }
    QString msg = why;
    if (u.stop.reason == StopReason::Fault || u.stop.reason == StopReason::Watchpoint) msg += ": " + fromStd(u.stop.detail);
    if (u.stop.reason == StopReason::Idle || u.stop.reason == StopReason::CycleLimit || u.stop.reason == StopReason::TimeLimit) msg += ": " + fromStd(u.stop.detail);
    if (!msg.isEmpty()) {
        statusBar()->showMessage(msg, 8000);
        ctx_.log.fire(tr("[cycle %1] %2").arg((qulonglong)u.stop.now).arg(msg));
    }
    if (where.valid) {
        Location l;
        l.file = where.file;
        l.line = where.line;
        ctx_.goTo.fire(l);
    }
    for (const std::string& m : u.messages) ctx_.log.fire(fromStd(m));
    updateDebugActions();
}

void MainWindow::editEnvironmentFile() {
    if (!ctx_.workspace.hasProject()) return;
    Project& p = ctx_.workspace.project();
    if (p.environment.empty()) {
        QString f = QFileDialog::getSaveFileName(this, tr("Environment file"), fromStd(p.absolute("environment.env")), tr("Environment (*.env *.ini);;All files (*)"));
        if (f.isEmpty()) return;
        std::string abs = norm(f);
        if (!ctx_.workspace.fileSystem()->exists(abs)) ctx_.workspace.fileSystem()->write(abs, "; simulator environment (docs/SIM.md)\n");
        p.environment = relativeTo(p.baseDir(), abs);
        ctx_.workspace.projectEdited();
        std::string err;
        ctx_.workspace.saveProject(err);
    }
    openFile(p.absolute(p.environment));
}

void MainWindow::closeEvent(QCloseEvent* e) {
    saveSession();
    ctx_.debugger.saveBreakpoints();
    if (!closeProject()) { e->ignore(); return; }
    saveWindowState();
    e->accept();
}

}  // namespace acide
