#include "block_editor.h"

#include <functional>
#include <set>

#include <QButtonGroup>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QToolButton>
#include <QToolTip>
#include <QSplitter>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QClipboard>
#include <QApplication>
#include <QTimer>
#include <QVBoxLayout>

#include "blocks/block_tidy.h"
#include "asmcore/diagnostics.h"
#include "block_link.h"
#include "hardware_link.h"

namespace acide {

using namespace blocks;

static blocks::BlockTheme blockThemeFor(const Settings& s, const Theme& t) {
    const bool dark = t.background.lightness() < 128;
    return BlockTheme::fromSettings(s, dark);
}

BlockEditor::BlockEditor(AppContext& ctx, const std::string& path, QWidget* parent) : QWidget(parent), ctx_(ctx), path_(path), catalog_(session_.registry()) {
    setObjectName("BlockEditor");
    const Theme theme = Theme::fromSettings(ctx.settings);
    QFont mono = editorFont(ctx.settings);
    mono.setPointSize(std::max(8, mono.pointSize() - 2));
    QFont ui = font();
    ui.setPointSize(std::max(8, ui.pointSize()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // the switch between the two pages
    auto* bar = new QWidget(this);
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(8, 4, 8, 4);
    blocksRadio_ = new QRadioButton(tr("Blocks"), bar);
    codeRadio_ = new QRadioButton(tr("Assembly code"), bar);
    blocksRadio_->setObjectName("pageBlocks");
    codeRadio_->setObjectName("pageCode");
    blocksRadio_->setToolTip(tr("Show the program as blocks"));
    codeRadio_->setToolTip(tr("Show the assembler text the blocks generate"));
    blocksRadio_->setChecked(true);
    auto* grp = new QButtonGroup(bar);
    grp->addButton(blocksRadio_);
    grp->addButton(codeRadio_);
    bl->addWidget(blocksRadio_);
    bl->addWidget(codeRadio_);
    bl->addSpacing(16);
    info_ = new QLabel(bar);
    info_->setObjectName("blockInfo");
    info_->setTextFormat(Qt::RichText);
    info_->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(info_, &QLabel::linkActivated, this, [this](const QString& kind) { jumpTo(kind); });
    bl->addWidget(info_, 1);
    zoomBar_ = new QWidget(bar);
    auto* zl = new QHBoxLayout(zoomBar_);
    zl->setContentsMargins(0, 0, 0, 0);
    zl->setSpacing(2);
    auto button = [&](const QString& text, const QString& tip, const char* name) {
        auto* b = new QToolButton(zoomBar_);
        b->setText(text);
        b->setToolTip(tip);
        b->setObjectName(name);
        b->setAutoRaise(true);
        zl->addWidget(b);
        return b;
    };
    QToolButton* zout = button("\xE2\x88\x92", tr("Zoom out (Ctrl+-)"), "zoomOut");
    zoomLabel_ = new QLabel("100%", zoomBar_);
    zoomLabel_->setMinimumWidth(40);
    zoomLabel_->setAlignment(Qt::AlignCenter);
    zl->addWidget(zoomLabel_);
    QToolButton* zin = button("+", tr("Zoom in (Ctrl++)"), "zoomIn");
    QToolButton* z100 = button("1:1", tr("Actual size (Ctrl+0)"), "zoomReset");
    QToolButton* zfit = button(tr("Fit"), tr("Show the whole program"), "zoomFit");
    QToolButton* toolboxBtn = new QToolButton(bar);
    toolboxBtn->setText(tr("Toolbox"));
    toolboxBtn->setToolTip(tr("Show or hide the toolbox (the blocks to drag onto the canvas)"));
    toolboxBtn->setObjectName("toolboxToggle");
    toolboxBtn->setCheckable(true);
    toolboxBtn->setChecked(ctx.settings.get("blocks.toolbox", "1") != "0");
    toolboxBtn->setAutoRaise(true);
    bl->insertWidget(2, toolboxBtn);
    QToolButton* ztidy = button(tr("Tidy"), tr("Tidy up: give every stack a place so that nothing overlaps (the program does not change)"), "tidy");
    bl->addWidget(zoomBar_);
    root->addWidget(bar);

    banner_ = new QLabel(this);
    banner_->setObjectName("blockBanner");
    banner_->setWordWrap(true);
    banner_->setStyleSheet("background:#fdecea;color:#b3261e;padding:6px 10px;");
    banner_->hide();
    root->addWidget(banner_);

    pages_ = new QStackedWidget(this);
    scene_ = new BlockScene(session_, blockThemeFor(ctx.settings, theme), ui, mono, this);
    view_ = new BlockView(scene_, this);
    view_->setObjectName("blockView");
    {
        blocks::LayoutStyle style;
        style.showEmptyOptional = true;                      // an editor shows the optional parts that are still empty (a shift amount, a flag): they are there to be filled in
        scene_->setLayoutStyle(style);
    }
    scene_->setAnimationsEnabled(ctx.settings.get("blocks.animate", "1") != "0");      // the soft, bouncy effects (blocks.animate = 0 switches them off)
    view_->setTheme(scene_->theme());
    fieldEditor_ = new FieldEditor(this);                  // (like the comment editor: a child of the editor, so the view's shortcuts do not take its keys)
    commentEdit_ = new CommentEdit(this);                  // a child of the editor, not of the view: the view's Esc / Delete shortcuts must not reach it
    code_ = new GeneratedCodePane(ctx.language, theme, mono, this);
    // the blocks page: the toolbox on the left, the canvas
    blocksPage_ = new QSplitter(Qt::Horizontal, this);
    blocksPage_->setObjectName("blocksPage");
    toolbox_ = new BlockToolbox(catalog_, scene_->theme(), blocksPage_);
    blocksPage_->addWidget(toolbox_);
    blocksPage_->addWidget(view_);
    blocksPage_->setStretchFactor(0, 0);
    blocksPage_->setStretchFactor(1, 1);
    blocksPage_->setSizes({210, 700});
    blocksPage_->setCollapsible(1, false);
    {                                                           // the blocks of the user's own
        std::vector<blocks::CustomBlock> mine;
        for (const std::string& line : ctx.settings.getList("blocks.custom")) { blocks::CustomBlock c; if (blocks::decodeCustom(line, c)) mine.push_back(c); }
        catalog_.setCustom(mine);
        toolbox_->refresh();
    }
    typeblock_ = new TypeBlockBox(catalog_, this);
    pages_->addWidget(blocksPage_);
    pages_->addWidget(code_);
    root->addWidget(pages_, 1);

    auto* selectAll = new QShortcut(QKeySequence::SelectAll, view_);
    selectAll->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectAll, &QShortcut::activated, this, [this] { scene_->selectAll(); });
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), view_);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] { if (scene_->dragging()) scene_->cancelDrag(); else session_.clearSelection(); });
    auto shortcut = [&](const QKeySequence& k, std::function<void()> fn) {
        auto* sc = new QShortcut(k, view_);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, [fn] { fn(); });
    };
    shortcut(QKeySequence::Delete, [this] { if (!scene_->dragging()) session_.deleteSelection(); });
    connect(blocksRadio_, &QRadioButton::toggled, this, [this](bool on) { if (on) setPage(Page::Blocks); });
    connect(codeRadio_, &QRadioButton::toggled, this, [this](bool on) { if (on) setPage(Page::Code); });
    connect(zout, &QToolButton::clicked, view_, &BlockView::zoomOut);
    connect(zin, &QToolButton::clicked, view_, &BlockView::zoomIn);
    connect(z100, &QToolButton::clicked, view_, &BlockView::resetZoom);
    connect(zfit, &QToolButton::clicked, view_, &BlockView::fitAll);
    connect(ztidy, &QToolButton::clicked, this, [this] { tidy(); });
    connect(toolboxBtn, &QToolButton::toggled, this, [this, &ctx](bool on) {
        toolbox_->setVisible(on);
        ctx.settings.set("blocks.toolbox", on ? "1" : "0");
    });
    toolbox_->setVisible(toolboxBtn->isChecked());
    connect(toolbox_, &BlockToolbox::activated, this, [this](const QString& id) { insertEntry(id); });
    connect(toolbox_, &BlockToolbox::addRequested, this, [this](const QString& drawer, bool fromSel) { newCustomBlock(drawer, fromSel); });
    connect(toolbox_, &BlockToolbox::editRequested, this, [this](const QString& id) {
        const blocks::CatalogEntry* en = catalog_.find(id.toStdString());
        if (en && en->custom) newCustomBlock(QString::fromStdString(en->category), false, id);
    });
    connect(toolbox_, &BlockToolbox::deleteRequested, this, [this](const QString& id) {
        const blocks::CatalogEntry* en = catalog_.find(id.toStdString());
        if (!en || !en->custom) return;
        if (QMessageBox::question(this, tr("Delete the block"), tr("Delete the block \"%1\" from the toolbox?").arg(QString::fromStdString(en->title))) == QMessageBox::Yes) deleteCustomBlock(id);
    });
    conns_->add(session_.selectionChanged, [this] { toolbox_->setSelectionAvailable(!session_.selection().empty()); });
    // a sample dragged from the toolbox over the canvas
    connect(view_, &BlockView::paletteDrag, this, [this](const QString& id, const QPointF& at, const QPoint& screen, int phase) {
        if (phase == 0) {
            const blocks::CatalogEntry* en = catalog_.find(id.toStdString());
            if (!en) return;
            paletteBlocks_ = session_.fresh([&](asmcore::blocks::Document& d) { return catalog_.instantiate(*en, d); });
            paletteEntry_ = id;
            scene_->beginPaletteDrag(paletteBlocks_, at);
            scene_->updatePaletteDrag(at, screen);
        } else if (phase == 1) {
            scene_->updatePaletteDrag(at, screen);
        } else if (phase == 2) {
            scene_->updatePaletteDrag(at, screen);
            scene_->endPaletteDrag(true);
            paletteEntry_.clear();
            view_->setFocus();
        } else {
            scene_->endPaletteDrag(false);
            paletteEntry_.clear();
        }
    });
    // Typeblock: type on the canvas (or Ctrl+Space) and a block appears
    conns_->add(scene_->typeRequested, [this](const std::string& text) { openTypeblock(QString::fromUtf8(text.c_str())); });
    connect(typeblock_, &TypeBlockBox::chosenEntry, this, [this](const QString& id) { insertEntry(id); });
    connect(typeblock_, &TypeBlockBox::chosenAssembler, this, [this](const QString& text) { insertAssembler(text); });
    connect(typeblock_, &TypeBlockBox::closed, this, [this] { view_->setFocus(); });
    connect(view_, &BlockView::zoomChanged, this, [this](double z) { zoomLabel_->setText(QString::number(qRound(z * 100)) + "%"); });
    connect(code_, &GeneratedCodePane::lineClicked, this, [this](int line) {
        if (syncing_) return;
        const std::string id = session_.blockAtLine(line);
        syncing_ = true;
        if (id.empty()) session_.clearSelection(); else session_.select({id});
        syncing_ = false;
    });

    conns_->add(scene_->contextRequested, [this](const QPointF& p, const QPoint& g, const std::string& id) { showContextMenu(p, g, id); });
    shortcut(QKeySequence("Ctrl+E"), [this] { session_.toggleDisabled(); });
    shortcut(QKeySequence("Ctrl+Space"), [this] { openTypeblock(); });
    conns_->add(scene_->fieldEdit, [this](const BlockScene::EditTarget& t) { beginFieldEdit(t); });
    connect(fieldEditor_, &FieldEditor::committed, this, [this](const QString& text, int navigate) {
        const BlockScene::EditTarget t = fieldEditing_;
        applyFieldValue(t, text);
        view_->setFocus();
        if (navigate != 0) {                                                                   // Tab / Shift+Tab: the next / previous part
            const std::vector<BlockScene::EditTarget> all = scene_->editTargets();
            for (size_t i = 0; i < all.size(); i++)
                if (all[i].blockId == t.blockId && all[i].part == t.part) {
                    const size_t n = all.size();
                    for (size_t step = 1; step <= n; step++) {
                        const BlockScene::EditTarget& next = all[(i + (navigate > 0 ? step : n - step)) % n];
                        const bool closedChoice = next.kind == asmcore::blocks::FieldKind::Choice && !next.open && !next.options.empty();        // (those are picked from a menu: Tab goes past them)
                        if (!next.flag && !closedChoice) { beginTextEdit(next); return; }                                                        // (the others are typed, with the list as help)
                    }
                }
        }
    });
    connect(fieldEditor_, &FieldEditor::cancelled, this, [this] { view_->setFocus(); });
    conns_->add(scene_->commentEdit, [this](const BlockScene::CommentTarget& t) { beginCommentEdit(t); });
    connect(commentEdit_, &CommentEdit::committed, this, [this](const QString& text) {
        std::string t = text.toUtf8().toStdString();
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) t.pop_back();       // trailing blank lines and spaces are not a comment
        session_.beginGroup("comment:" + std::to_string(++commentSerial_));              // each kept comment is its own undo step (typing in the editor is not an edit yet)
        struct EndGroup { BlockSession& s; ~EndGroup() { s.endGroup(); } } endGroup{session_};
        if (editing_.section) {
            session_.apply([&](asmcore::blocks::Editor& ed) { return ed.setSectionComment(editing_.stack, t); });
        } else if (!editing_.blockId.empty() && session_.document().find(editing_.blockId)) {
            const bool changed = t != editing_.text;
            session_.apply([&](asmcore::blocks::Editor& ed) { return ed.setComment(editing_.blockId, t); });
            if (changed) scene_->popEffect(editing_.blockId);                                  // the block settles with a little wobble
        }
        view_->setFocus();
    });
    connect(commentEdit_, &CommentEdit::cancelled, this, [this] { view_->setFocus(); });
    view_->viewport()->installEventFilter(this);
    conns_->add(scene_->blockClicked, [this](const std::string& id, Qt::KeyboardModifiers m) { onBlockClicked(id, m); });
    conns_->add(scene_->emptyClicked, [this](Qt::KeyboardModifiers m) { if (!(m & Qt::ControlModifier)) session_.clearSelection(); });
    conns_->add(session_.changed, [this] { refreshCode(); refreshInfo(); syncToWorkspace(); applyBuildProblems(); });
    snap_ = ctx.snapshot;
    conns_->add(ctx.snapshotChanged, [this](SnapshotPtr s) { snap_ = s; applyBuildProblems(); });
    conns_->add(session_.selectionChanged, [this] {
        if (!syncing_) code_->setMarkedLines(session_.selectedLines(), page() == Page::Code);
        refreshInfo();
    });
    conns_->add(ctx.workspace.documentChanged, [this](const std::string& p) {
        if (p == path_) reload();
        else if (!symbolsFrom_.empty() && p.compare(0, symbolsFrom_.size() - 5, symbolsFrom_, 0, symbolsFrom_.size() - 5) == 0) refreshSymbols();     // the description or the include generated from it
    });

    conns_->add(ctx.workspace.projectChanged, [this] { refreshSymbols(); });                    // a hardware description was attached / changed
    {
        asmcore::blocks::GenerateOptions go;
        go.header = generatedHeader(path_);
        session_.setGenerateOptions(go);
    }
    reload();
    loading_ = false;
}

// Diagnostics of the generated file -> the block that produced the line (the part "from the blocks to the build and back"). Only while the build is of the
// text the blocks generate now: after an edit the lines may have moved, so the badges go until the next build.
void BlockEditor::applyBuildProblems() {
    std::vector<asmcore::blocks::BlockProblem> v;
    const std::string gen = generatedPath();
    if (snap_) {
        const std::string* built = nullptr;
        for (const asmcore::Source& s : snap_->sources) if (s.name == gen) built = &s.text;
        if (built && *built == session_.generated().text) {
            for (const asmcore::Diagnostic& d : snap_->diagnosticsFor(gen)) {
                if (d.sev == asmcore::Diagnostic::Note) continue;
                const std::string id = session_.blockAtLine(d.loc.line);
                if (id.empty()) continue;
                asmcore::blocks::BlockProblem p;
                p.error = d.sev == asmcore::Diagnostic::Error;
                p.blockId = id;
                p.kind = "build";
                p.message = d.code + ": " + d.msg;
                const std::string ex = asmcore::explainDiagnostic(d.code);                      // "...\n  fix: ..." : the way out
                const size_t fix = ex.find("fix:");
                if (fix != std::string::npos) p.message += "\n" + ex.substr(fix, ex.find('\n', fix) == std::string::npos ? std::string::npos : ex.find('\n', fix) - fix);
                v.push_back(p);
            }
        }
    }
    session_.setBuildProblems(v);
}

bool BlockEditor::eventFilter(QObject* o, QEvent* e) {
    if (view_ && o == view_->viewport() && e->type() == QEvent::MouseButtonPress && fieldEditor_ && fieldEditor_->isOpen()) {
        const QPoint at = static_cast<QMouseEvent*>(e)->pos();
        if (!fieldEditor_->geometry().contains(view_->viewport()->mapTo(this, at))) fieldEditor_->commit();                  // (not when the click is on the editor itself)
    }
    return QWidget::eventFilter(o, e);
}

std::string BlockEditor::generatedPath() const { return acide::generatedPathFor(path_, session_.document()); }

// The blocks are the truth. Each edit puts the serialized program and the text generated from it into the workspace (as unsaved buffers: the build and the
// debugger use them at once, Save All writes both). Loading a file writes nothing.
void BlockEditor::syncToWorkspace() {
    if (loading_) return;
    if (session_.document().blockCount() > 300) {                                        // big: after the last edit of a burst
        if (!syncTimer_) {
            syncTimer_ = new QTimer(this);
            syncTimer_->setSingleShot(true);
            connect(syncTimer_, &QTimer::timeout, this, [this] { doSync(); });
        }
        syncTimer_->start(250);
        return;
    }
    doSync();
}

void BlockEditor::flushSync() {
    if (syncTimer_ && syncTimer_->isActive()) {
        syncTimer_->stop();
        doSync();
    }
}

void BlockEditor::doSync() {
    if (loading_) return;
    if (!attached_ && ctx_.workspace.hasProject() && session_.document().output.empty()) {
        // A program that never said where its text goes, next to an assembler file the project already builds under the same name (a program made by an
        // older "show as blocks"): that file is the one it generates (as a conversion would), so the project does not get two copies of the same code.
        const size_t slash = path_.find_last_of('/'), dot = path_.find_last_of('.');
        const std::string sibling = path_.substr(0, dot != std::string::npos && dot > slash ? dot : path_.size()) + ".asm";
        bool inProject = false;
        for (const std::string& s : ctx_.workspace.project().sources) inProject |= ctx_.workspace.project().absolute(s) == sibling;
        if (inProject) {
            const std::string backup = sibling + ".bak";
            if (!ctx_.workspace.fileSystem()->exists(backup)) { std::string old; if (const Document* d = ctx_.workspace.find(sibling)) old = d->text; else ctx_.workspace.fileSystem()->read(sibling, old); ctx_.workspace.fileSystem()->write(backup, old); }
            const bool was = loading_;
            loading_ = true;
            session_.setOutput(sibling.substr(sibling.find_last_of('/') + 1));
            loading_ = was;
            ctx_.log.fire(tr("%1 is now generated from %2 (the earlier text is kept as %3)").arg(QString::fromStdString(sibling)).arg(QString::fromStdString(path_)).arg(QString::fromStdString(backup)));
        }
    }
    const std::string text = asmcore::blocks::serializeDocument(session_.document());
    if (text == syncedText_) return;                                                     // not an edit (the names known to the checks changed, ...)
    syncedText_ = text;
    lastText_ = text;
    ctx_.workspace.setText(path_, text);
    const std::string gen = generatedPath();
    const Document* old = ctx_.workspace.find(gen);
    if (!old || old->text != session_.generated().text) ctx_.workspace.setText(gen, session_.generated().text);
    if (!attached_) {
        attached_ = true;
        std::string err;
        if (!attachBlocks(ctx_.workspace, path_, gen, err)) ctx_.log.fire(tr("Cannot add the block program to the project: %1").arg(QString::fromStdString(err)));
    }
}

// The scene listens to the session, which is a member of this class and so dies before the (child) scene would: take the views down first.
BlockEditor::~BlockEditor() {
    conns_.reset();
    delete view_;
    delete scene_;
    view_ = nullptr;
    scene_ = nullptr;
}

QString BlockEditor::tabTitle() const { return tr("Blocks: %1").arg(QFileInfo(QString::fromStdString(path_)).fileName()); }

BlockEditor::Page BlockEditor::page() const { return pages_->currentWidget() == code_ ? Page::Code : Page::Blocks; }

void BlockEditor::setPage(Page p) {
    pages_->setCurrentWidget(p == Page::Code ? static_cast<QWidget*>(code_) : static_cast<QWidget*>(blocksPage_));
    blocksRadio_->setChecked(p == Page::Blocks);
    codeRadio_->setChecked(p == Page::Code);
    zoomBar_->setVisible(p == Page::Blocks);
    if (p == Page::Code && codeStale_) refreshCode();
    if (p == Page::Code) code_->setMarkedLines(session_.selectedLines(), true);
    else if (!session_.selection().empty()) view_->showBlock(scene_->blockRect(*session_.selection().begin()));
}

QString BlockEditor::errorText() const { return banner_->isHidden() ? QString() : banner_->text(); }
QString BlockEditor::infoText() const { return info_->text(); }

void BlockEditor::reload() {
    Document* doc = ctx_.workspace.find(path_);
    if (!doc) return;
    if (doc->text == lastText_ && !lastText_.empty()) return;          // our own change coming back
    lastText_ = doc->text;
    const bool firstLoad = syncedText_.empty();
    struct Quiet { bool& f; bool was; explicit Quiet(bool& x) : f(x), was(x) { f = true; } ~Quiet() { f = was; } } quiet(loading_);     // loading is not an edit: nothing is written back
    std::string err;
    if (!session_.load(doc->text, err)) {
        banner_->setText(tr("This file cannot be shown as blocks: %1").arg(QString::fromStdString(err)));
        banner_->show();
        info_->clear();
        return;
    }
    banner_->hide();
    refreshSymbols();
    {                                                                          // (the header names this program)
        asmcore::blocks::GenerateOptions go;
        go.header = generatedHeader(path_);
        session_.setGenerateOptions(go);
    }
    syncedText_ = asmcore::blocks::serializeDocument(session_.document());     // what is in the editor now: only a change from this is an edit
    view_->showContent();
    refreshCode();
    refreshInfo();
    // a program changed from outside: the generated file, if it is in the workspace, follows
    const Document* old = ctx_.workspace.find(generatedPath());
    if (!firstLoad && old && old->text != session_.generated().text) ctx_.workspace.setText(generatedPath(), session_.generated().text);        // (opening a program changes no file)
}

// The program's hardware description (the one the file names, else the project's) defines names; they are known to the checks.
void BlockEditor::refreshSymbols() {
    std::string achw = session_.document().hardware;
    if (!achw.empty() && achw[0] != '/' && !(achw.size() > 1 && achw[1] == ':')) {
        const size_t slash = path_.find_last_of('/');
        achw = (slash == std::string::npos ? std::string() : path_.substr(0, slash + 1)) + achw;
    }
    if (achw.empty() && ctx_.workspace.hasProject() && !ctx_.workspace.project().hardware.empty()) achw = ctx_.workspace.project().absolute(ctx_.workspace.project().hardware);
    if (achw.empty() || achw.size() < 5) { symbolsFrom_.clear(); return; }
    symbolsFrom_ = achw;
    asmcore::blocks::SymbolTable table;
    if (hardwareSymbols(ctx_.workspace, achw, table)) session_.setExternalSymbols(table);
}

void BlockEditor::tidy() {
    if (blocks::tidyUp(session_, scene_->layout()) > 0) view_->fitAll();
}

namespace {
class SystemClipboard : public IBlockClipboard {
public:
    void set(const std::string& text, const std::string& blocks) override {
        auto* mime = new QMimeData;
        mime->setText(QString::fromUtf8(text.c_str()));                                       // for a text editor
        mime->setData("application/x-acblk", QByteArray(blocks.c_str()));                    // for another block editor: the blocks themselves
        QApplication::clipboard()->setMimeData(mime);
    }
    void get(std::string& text, std::string& blocks) override {
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        if (!mime) return;
        if (mime->hasFormat("application/x-acblk")) blocks = mime->data("application/x-acblk").toStdString();
        text = mime->text().toUtf8().toStdString();
    }
};
}  // namespace

static void saveCustom(AppContext& ctx, const std::vector<blocks::CustomBlock>& all) {
    std::vector<std::string> lines;
    for (const blocks::CustomBlock& c : all) lines.push_back(blocks::encodeCustom(c));
    ctx.settings.setList("blocks.custom", lines);
}

// the text of the selected blocks without the indentation the program gives them
static QString selectionAsText(BlockSession& s) {
    if (s.selection().empty()) return QString();
    std::string text = s.copySelection().text;
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : text) { if (ch == '\n') { lines.push_back(cur); cur.clear(); } else cur += ch; }
    if (!cur.empty()) lines.push_back(cur);
    size_t indent = std::string::npos;
    for (const std::string& l : lines) {
        if (l.find_first_not_of(' ') == std::string::npos) continue;
        indent = std::min(indent, l.find_first_not_of(' '));
    }
    std::string out;
    for (const std::string& l : lines) out += (indent != std::string::npos && l.size() >= indent ? l.substr(indent) : l) + "\n";
    return QString::fromUtf8(out.c_str());
}

void BlockEditor::newCustomBlock(const QString& drawer, bool fromSelection, const QString& replacingId) {
    if (customDialog_) return;
    customDialog_ = new CustomBlockDialog(catalog_, toolbox_->drawerNames(), replacingId, this);
    customDialog_->setAttribute(Qt::WA_DeleteOnClose);
    QString title, text;
    QString where = drawer.isEmpty() ? QString::fromStdString(toolbox_->drawerNames().empty() ? std::string("Data") : toolbox_->drawerNames()[1 < toolbox_->drawerNames().size() ? 1 : 0]) : drawer;
    if (!replacingId.isEmpty()) {
        if (const blocks::CatalogEntry* en = catalog_.find(replacingId.toStdString())) { title = QString::fromStdString(en->title); text = QString::fromStdString(en->text); where = QString::fromStdString(en->category); }
    }
    customDialog_->setValues(where, title, text);
    const QString sel = selectionAsText(session_);
    customDialog_->setSelectionText(sel);
    if (fromSelection && !sel.isEmpty()) customDialog_->textEdit()->setPlainText(sel);
    connect(customDialog_, &QDialog::accepted, this, [this, replacingId] {
        std::vector<blocks::CustomBlock> all = catalog_.custom();
        const blocks::CustomBlock c = customDialog_->values();
        if (!replacingId.isEmpty()) {
            for (size_t i = 0; i < all.size(); i++) if (blocks::BlockCatalog::customId(all[i]) == replacingId.toStdString()) { all[i] = c; break; }
        } else all.push_back(c);
        catalog_.setCustom(all);
        saveCustom(ctx_, all);
        toolbox_->refresh();
        ctx_.status.fire(tr("Added \"%1\" to the toolbox").arg(QString::fromStdString(c.title)));
    });
    connect(customDialog_, &QObject::destroyed, this, [this] { customDialog_ = nullptr; });
    customDialog_->open();
}

void BlockEditor::deleteCustomBlock(const QString& entryId) {
    std::vector<blocks::CustomBlock> all;
    for (const blocks::CustomBlock& c : catalog_.custom()) if (blocks::BlockCatalog::customId(c) != entryId.toStdString()) all.push_back(c);
    catalog_.setCustom(all);
    saveCustom(ctx_, all);
    toolbox_->refresh();
}

void BlockEditor::openTypeblock(const QString& initial) {
    if (fieldEditor_->isOpen() || commentEdit_->isOpen() || typeblock_->isOpen() || page() != Page::Blocks) return;
    QRect r = view_->geometry();
    r.moveTopLeft(blocksPage_->mapTo(this, r.topLeft()));
    typeblock_->open(r, initial);
}

// the middle of what is shown: where a block lands when nothing says otherwise
size_t BlockEditor::insertEntry(const QString& entryId) {
    const blocks::CatalogEntry* en = catalog_.find(entryId.toStdString());
    if (!en) return 0;
    std::vector<asmcore::blocks::Block> blocks = session_.fresh([&](asmcore::blocks::Document& d) { return catalog_.instantiate(*en, d); });
    const QPointF c = view_->mapToScene(view_->viewport()->rect().center());
    const size_t n = session_.place(blocks, c.x() - 60, c.y() - 20);
    if (n && !session_.selection().empty()) {
        const std::string id = *session_.selection().begin();
        scene_->popEffect(id);
        view_->showBlock(scene_->blockRect(id));
    }
    return n;
}

size_t BlockEditor::insertAssembler(const QString& text) {
    bool raw = false;
    std::vector<asmcore::blocks::Block> blocks = session_.fresh([&](asmcore::blocks::Document& d) { return catalog_.fromAssembler(d, text.toUtf8().toStdString(), &raw); });
    if (blocks.empty()) return 0;
    const QPointF c = view_->mapToScene(view_->viewport()->rect().center());
    const size_t n = session_.place(blocks, c.x() - 60, c.y() - 20);
    if (raw) ctx_.status.fire(tr("Not understood as an instruction: kept as assembler text"));
    if (n && !session_.selection().empty()) {
        const std::string id = *session_.selection().begin();
        scene_->popEffect(id);
        view_->showBlock(scene_->blockRect(id));
    }
    return n;
}

void BlockEditor::copy() {
    if (session_.selection().empty()) return;
    const BlockSession::Clip clip = session_.copySelection();
    if (!clipboard_) clipboard_ = std::make_shared<SystemClipboard>();
    clipboard_->set(clip.text, clip.blocks);
    ctx_.status.fire(tr("Copied %n block(s)", nullptr, (int)session_.selection().size()));
}

void BlockEditor::cut() {
    if (session_.selection().empty()) return;
    BlockSession::Clip clip;
    session_.cutSelection(clip);
    if (!clipboard_) clipboard_ = std::make_shared<SystemClipboard>();
    clipboard_->set(clip.text, clip.blocks);
}

void BlockEditor::paste() {
    if (!clipboard_) clipboard_ = std::make_shared<SystemClipboard>();
    std::string text, blocks;
    clipboard_->get(text, blocks);
    const std::string use = blocks.empty() ? text : blocks;                                  // our own blocks first, else whatever text there is
    const QPointF centre = view_->mapToScene(view_->viewport()->rect().center());
    std::string note;
    const size_t n = session_.paste(use, centre.x() - 60, centre.y() - 20, note);
    if (n == 0) { ctx_.status.fire(note.empty() ? tr("Nothing to paste") : QString::fromUtf8(note.c_str())); return; }
    if (!note.empty()) ctx_.status.fire(QString::fromUtf8(note.c_str()));
    for (const std::string& id : session_.selection()) scene_->popEffect(id);
}

void BlockEditor::showContextMenu(const QPointF& scenePos, const QPoint& globalPos, const std::string& blockId) {
    if (fieldEditor_->isOpen() || commentEdit_->isOpen()) return;
    if (!blockId.empty() && !session_.isSelected(blockId)) session_.select({blockId});
    QMenu menu(this);
    menu.setObjectName("blockContextMenu");
    const bool any = !session_.selection().empty();
    auto add = [&](const QString& text, std::function<void()> fn, bool enabled = true, const QKeySequence& key = QKeySequence()) {
        QAction* a = menu.addAction(text);
        a->setEnabled(enabled);
        if (!key.isEmpty()) a->setShortcut(key);
        a->setShortcutVisibleInContextMenu(true);
        connect(a, &QAction::triggered, this, [fn] { fn(); });
        return a;
    };
    if (!blockId.empty()) {
        BlockScene::EditTarget et;
        if (scene_->editTargetAt(scenePos, et) && !et.flag) add(tr("Edit..."), [this, et] { beginFieldEdit(et); });
        BlockScene::CommentTarget ct;
        if (scene_->commentTargetOf(blockId, ct)) add(ct.text.empty() ? tr("Add a comment...") : tr("Edit the comment..."), [this, ct] { beginCommentEdit(ct); });
        menu.addSeparator();
    }
    add(tr("Cu&t"), [this] { cut(); }, any, QKeySequence::Cut);
    add(tr("&Copy"), [this] { copy(); }, any, QKeySequence::Copy);
    add(tr("&Paste"), [this] { paste(); }, true, QKeySequence::Paste);
    add(tr("&Duplicate"), [this] { session_.duplicateSelection(); }, any, QKeySequence("Ctrl+D"));
    add(tr("Save the selection as a new block..."), [this] { newCustomBlock("Patterns", true); }, any);
    menu.addSeparator();
    bool anyInstruction = false, anyContainer = false, allOff = true;
    for (const std::string& id : session_.selection()) {
        const asmcore::blocks::Block* b = session_.document().find(id);
        const asmcore::blocks::BlockDef* d = b ? session_.registry().find(b->type) : nullptr;
        if (!d) continue;
        if (d->valueType.empty()) { anyInstruction = true; if (!b->disabled) allOff = false; }
        if (!d->stacks.empty()) anyContainer = true;
    }
    add(allOff && anyInstruction ? tr("&Enable") : tr("Disa&ble"), [this] { session_.toggleDisabled(); }, anyInstruction, QKeySequence("Ctrl+E"));
    add(tr("&Fold / unfold"), [this] { session_.toggleCollapsed(); }, anyContainer);
    menu.addSeparator();
    add(tr("&Delete"), [this] { session_.deleteSelection(); }, any, QKeySequence::Delete);
    if (blockId.empty()) {
        menu.addSeparator();
        add(tr("Select &all"), [this] { scene_->selectAll(); }, true, QKeySequence::SelectAll);
        add(tr("&Tidy up"), [this] { tidy(); });
        add(tr("Show everything"), [this] { view_->fitAll(); });
    }
    menu.exec(globalPos);
}

// The names that fit a part: the choices of its field, else the symbols of the kind its socket takes (CSRs, services, devices, numbers, labels, procedures).
// A closed choice (only the listed values make sense: for / forever, ...) is picked from a menu that opens under the part. A menu is what Windows users
// know (the right-button menus work the same way) and it stays until something is picked or Esc is pressed; a drop-down list opened in the middle of a double
// click was closed again by the click that was still going on.
void BlockEditor::showChoiceMenu(const BlockScene::EditTarget& t) {
    if (choiceMenu_) { choiceMenu_->close(); choiceMenu_->deleteLater(); choiceMenu_ = nullptr; }
    auto* menu = new QMenu(this);
    menu->setObjectName("choiceMenu");
    choiceMenu_ = menu;
    const QString current = QString::fromUtf8(t.text.c_str());
    for (const std::string& o : t.options) {
        const QString text = QString::fromUtf8(o.c_str());
        QAction* a = menu->addAction(text);
        a->setCheckable(true);
        a->setChecked(text == current);
        connect(a, &QAction::triggered, this, [this, t, text] { applyFieldValue(t, text); });
    }
    if (t.open) {                                                                       // other values are allowed too: type one
        menu->addSeparator();
        QAction* other = menu->addAction(tr("Other value..."));
        connect(other, &QAction::triggered, this, [this, t] { QTimer::singleShot(0, this, [this, t] { beginTextEdit(t); }); });
    }
    connect(menu, &QMenu::aboutToHide, this, [this, menu] {
        QTimer::singleShot(0, menu, [this, menu] { if (choiceMenu_ == menu) choiceMenu_ = nullptr; menu->deleteLater(); view_->setFocus(); });
    });
    view_->showBlock(t.rect);
    const QRect r = view_->mapFromScene(t.rect).boundingRect();
    menu->popup(view_->viewport()->mapToGlobal(r.bottomLeft()));
}

// Puts a part's new text into the program (one undo step), or says why not.
void BlockEditor::applyFieldValue(const BlockScene::EditTarget& t, const QString& text) {
    const std::string value = text.trimmed().toUtf8().toStdString();
    if (value == t.text) return;
    session_.beginGroup("edit:" + std::to_string(++editSerial_));                       // each kept edit is its own undo step
    const asmcore::blocks::Outcome o = session_.apply([&](asmcore::blocks::Editor& ed) { return ed.setPart(t.blockId, t.part, value); });
    session_.endGroup();
    if (!o.ok) QToolTip::showText(QCursor::pos(), QString::fromUtf8(o.error.c_str()), this);
    else scene_->popEffect(t.blockId);
}

// A part with a list of values (an operation, a sign, a condition, a width ...) is picked from a menu; a part that is free text (a name, a number) is typed into the
// editor over it.
void BlockEditor::beginFieldEdit(const BlockScene::EditTarget& t) {
    if (fieldEditor_->isOpen() || commentEdit_->isOpen()) return;
    if (!t.options.empty()) { showChoiceMenu(t); return; }
    beginTextEdit(t);
}

void BlockEditor::beginTextEdit(const BlockScene::EditTarget& t) {
    if (fieldEditor_->isOpen() || commentEdit_->isOpen()) return;
    fieldEditing_ = t;
    QStringList items;
    std::set<std::string> seen;
    auto add = [&](const std::string& s) { if (!s.empty() && seen.insert(s).second) items << QString::fromUtf8(s.c_str()); };
    const asmcore::blocks::SymbolTable& syms = session_.symbols();
    if (!t.options.empty()) for (const std::string& o : t.options) add(o);
    for (const std::string& a : t.accepts) {
        if (a == "label.flow" || a == "label.proc" || a == "csr" || a == "service" || a == "dev" || a == "int")
            for (const asmcore::blocks::Symbol* s : syms.ofKind(a)) add(s->name);
    }
    items.sort(Qt::CaseInsensitive);
    // the hint for the value: the range of a number, the form of an operand
    const asmcore::blocks::Block* b = session_.document().find(t.blockId);
    const asmcore::blocks::BlockDef* def = b ? session_.registry().find(b->type) : nullptr;
    const asmcore::blocks::FieldDef* fd = nullptr;
    if (def) for (const asmcore::blocks::FieldDef& f : def->fields) if (f.name == t.part) fd = &f;
    auto hint = [def, fd](const QString& text) -> QString {
        if (!def || !fd) return QString();
        return QString::fromUtf8(asmcore::blocks::checkFieldValue(*def, *fd, text.trimmed().toUtf8().toStdString()).c_str());
    };
    QRect r = view_->mapFromScene(t.rect).boundingRect();
    r.moveTopLeft(view_->viewport()->mapTo(this, r.topLeft()));
    QFont f = font();
    f.setPointSizeF(std::max(7.0, f.pointSizeF() * view_->zoom()));
    const blocks::BlockTheme& th = scene_->theme();
    const bool closed = t.kind == asmcore::blocks::FieldKind::Choice && !t.open && !t.options.empty();
    view_->showBlock(t.rect);
    fieldEditor_->open(r, QString::fromUtf8(t.text.c_str()), items, !closed, f, toQColor(th.canvas).lighter(125), toQColor(th.chipText), toQColor(th.selection), hint);
}

void BlockEditor::beginCommentEdit(const BlockScene::CommentTarget& t) {
    if (commentEdit_->isOpen()) return;
    editing_ = t;
    QRect r = view_->mapFromScene(t.rect).boundingRect();
    r.moveTopLeft(view_->viewport()->mapTo(this, r.topLeft()));
    QFont f = font();
    f.setPointSizeF(std::max(7.0, f.pointSizeF() * view_->zoom()));
    const blocks::BlockTheme& th = scene_->theme();
    commentEdit_->open(QRect(r.x(), r.y(), std::max(r.width(), (int)(190 * view_->zoom())), r.height()), QString::fromUtf8(t.text.c_str()), f, toQColor(th.canvas).lighter(125), toQColor(th.chipText), toQColor(th.selection));
}

// The assembly page is filled when it is shown (setting a thousand lines costs on every edit, and nobody looks at it while editing blocks).
void BlockEditor::refreshCode() {
    if (page() != Page::Code) { codeStale_ = true; return; }
    codeStale_ = false;
    code_->setCode(QString::fromUtf8(session_.generated().text.c_str()));
}

void BlockEditor::refreshInfo() {
    const BlockSession::Stats s = session_.stats();
    QString t = tr("%n block(s)", nullptr, (int)s.blocks);
    auto link = [](const char* kind, const QString& text) { return QString("<a href=\"%1\">%2</a>").arg(kind, text); };         // each count jumps to the blocks it counts
    if (s.floatingStacks) t += ", " + link("floating", tr("%n floating stack(s)", nullptr, (int)s.floatingStacks));
    if (s.rawBlocks) t += ", " + link("raw", tr("%n text line(s)", nullptr, (int)s.rawBlocks));
    if (s.problems) t += ", " + link("problems", tr("%n problem(s)", nullptr, (int)s.problems));
    if (!session_.selection().empty()) t += tr(" \xC2\xB7 %n selected", nullptr, (int)session_.selection().size());
    info_->setText(t);
}

// The blocks a count in the status line stands for ("floating": the first block of each floating stack; "raw": the text blocks; "problems": the blocks with a
// badge), in program order.
std::vector<std::string> BlockEditor::blocksFor(const QString& kind) const {
    std::vector<std::string> ids;
    std::function<void(const std::vector<asmcore::blocks::Block>&)> walk = [&](const std::vector<asmcore::blocks::Block>& v) {
        for (const asmcore::blocks::Block& b : v) {
            if (kind == "raw" && b.type == "raw") ids.push_back(b.id);
            for (auto& st : b.stacks) walk(st.second);
        }
    };
    if (kind == "problems") {
        for (const asmcore::blocks::BlockProblem& p : session_.problems()) if (!p.blockId.empty() && std::find(ids.begin(), ids.end(), p.blockId) == ids.end()) ids.push_back(p.blockId);
    } else {
        for (const asmcore::blocks::Section& s : session_.document().sections) {
            if (kind == "floating") { if (s.domain == "floating" && !s.blocks.empty()) ids.push_back(s.blocks[0].id); }
            else walk(s.blocks);
        }
    }
    return ids;
}

// Selects the next of them (a click again goes on to the one after) and brings it into view.
void BlockEditor::jumpTo(const QString& kind) {
    const std::vector<std::string> ids = blocksFor(kind);
    if (ids.empty()) return;
    unsigned& at = jumpAt_[kind];
    const std::string id = ids[at++ % ids.size()];
    if (page() != Page::Blocks) setPage(Page::Blocks);
    selectBlock(id, true);
    view_->setFocus();
}

void BlockEditor::onBlockClicked(const std::string& id, Qt::KeyboardModifiers m) {
    if (m & Qt::ControlModifier) session_.toggle(id);
    else session_.select({id});
}

void BlockEditor::selectBlock(const std::string& id, bool reveal) {
    session_.select({id});
    if (reveal) view_->showBlock(scene_->blockRect(id));
}

}  // namespace acide
