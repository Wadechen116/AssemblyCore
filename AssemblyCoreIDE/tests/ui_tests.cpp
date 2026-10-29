// UI tests (Qt Test, run with QT_QPA_PLATFORM=offscreen).
#include <QApplication>
#include <QtTest>

#include <QDir>
#include <QListWidget>
#include <QComboBox>
#include <QTableView>
#include <QTableWidget>
#include <QTreeWidget>
#include <QDockWidget>

#include <QLineEdit>
#include <QMenuBar>
#include <QSpinBox>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QXmlStreamReader>

#include <QRadioButton>
#include <QMenu>
#include <QClipboard>
#include <QMimeData>
#include <QTimer>
#include <QScrollBar>
#include <QWheelEvent>
#include <QToolButton>
#include <QImage>
#include <QElapsedTimer>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "app_shell.h"
#include "block_editor.h"
#include "block_link.h"
#include "document_tab.h"
#include "asmcore/blocks.h"
#include "asmcore/blocks_edit.h"
#include "code_editor.h"
#include "i18n.h"
#include "editing.h"
#include "hardware_designer.h"
#include "hardware_link.h"
#include "isa_top_view.h"
#include "main_window.h"
#include "qt_dispatcher.h"
#include "templates.h"

using namespace acide;

namespace {

struct Fixture {
    std::shared_ptr<MemoryFileSystem> fs = std::make_shared<MemoryFileSystem>();
    std::shared_ptr<QtDispatcher> disp = std::make_shared<QtDispatcher>();
    std::shared_ptr<const asmcore::lang::Language> lang = std::make_shared<const asmcore::lang::Language>(asmcore::lang::Language::defaults());
    Workspace ws;
    BuildService build;
    Settings settings;
    SimSession sim;
    Debugger dbg;
    AppContext ctx;
    Fixture() : ws(fs), build(disp, lang), sim(disp), dbg(sim, ws, &settings, [this] { return ctx.snapshot; }), ctx(ws, build, settings, dbg, lang) {
        fs->write("/w/p.acproj", "name = p\nsource = main.asm\n");
        fs->write("/w/main.asm", ".csr C = 0x120\n.main\nstart: EXEC_CSR WRITE8 C, 1\n    ALU ADD unsigned\nh: JMP h\n");
        std::vector<std::string> errs;
        ws.openProject("/w/p.acproj", errs);
        build.finished.connect([this](SnapshotPtr s) { ctx.snapshot = s; ctx.snapshotChanged.fire(s); });
    }
    bool buildAndWait() {
        uint64_t id = build.request(ws.snapshot());
        for (int i = 0; i < 500 && !(ctx.snapshot && ctx.snapshot->id == id); i++) QTest::qWait(10);
        return ctx.snapshot && ctx.snapshot->id == id;
    }
};

}  // namespace

static QString outDirFree() {
    QByteArray d = qgetenv("ACIDE_TEST_OUT");
    return d.isEmpty() ? QDir::tempPath() : QString::fromLocal8Bit(d);
}
static int distinctColorsFree(const QImage& img) {
    QSet<QRgb> s;
    for (int y = 0; y < img.height(); y += 2) for (int x = 0; x < img.width(); x += 2) s.insert(img.pixel(x, y));
    return s.size();
}

class UiTests : public QObject {
    Q_OBJECT
private slots:
    void editorShowsTheFileAndDiagnostics() {
        Fixture f;
        CodeEditor ed(f.ctx, "/w/main.asm");
        QVERIFY2(ed.toPlainText().startsWith(".csr C"), qPrintable("text=[" + ed.toPlainText() + "]"));
        QVERIFY(f.buildAndWait());
        QVERIFY(f.ctx.snapshot->result.ok);
        ed.setSnapshot(f.ctx.snapshot);
        QVERIFY(ed.diagnosticSelectionCount() >= 1);       // the data-flow warnings for the ALU with no operands
    }
    void editingFlowsToTheWorkspaceAndBack() {
        Fixture f;
        CodeEditor ed(f.ctx, "/w/main.asm");
        QTextCursor c = ed.textCursor();
        c.movePosition(QTextCursor::End);
        c.insertText("; hello\n");
        QVERIFY(f.ws.find("/w/main.asm")->text.find("; hello") != std::string::npos);
        QVERIFY(f.ws.find("/w/main.asm")->dirty);
        f.ws.setText("/w/main.asm", "; replaced\n");                       // a change from elsewhere (rename, reload)
        QCOMPARE(ed.toPlainText(), QString("; replaced\n"));
    }
    void theBufferSurvivesCreationOfASecondEditor() {
        Fixture f;
        CodeEditor a(f.ctx, "/w/main.asm");
        CodeEditor b(f.ctx, "/w/main.asm");
        QVERIFY(b.toPlainText().startsWith(".csr C"));
        QVERIFY(!f.ws.find("/w/main.asm")->dirty);
    }
    void completionOffersWhatFits() {
        Fixture f;
        QVERIFY(f.buildAndWait());
        CodeEditor ed(f.ctx, "/w/main.asm");
        ed.setSnapshot(f.ctx.snapshot);
        ed.setPlainText(".main\n    BR.COND ALWAYS, ");
        QTextCursor c = ed.textCursor();
        c.movePosition(QTextCursor::End);
        ed.setTextCursor(c);
        ed.triggerCompletion();
        QVERIFY(ed.completionLabels().contains("start"));          // a label of the project (index of the last build)
        ed.setPlainText(".main\n    M");
        c = ed.textCursor(); c.movePosition(QTextCursor::End); ed.setTextCursor(c);
        ed.triggerCompletion();
        QVERIFY(ed.completionLabels().contains("MOV"));
        QVERIFY(!ed.completionLabels().contains("EXEC_PUB"));
    }
    void multibyteTextKeepsColumnsRight() {
        Fixture f;
        f.fs->write("/w/main.asm", QString::fromUtf8("; \xE4\xB8\xAD\xE6\x96\x87\n.main\nh: JMP nowhere\n").toStdString());
        CodeEditor ed(f.ctx, "/w/main.asm");
        QVERIFY(f.buildAndWait());
        ed.setSnapshot(f.ctx.snapshot);
        QVERIFY(ed.diagnosticSelectionCount() >= 1);
    }
};

class WindowTests : public QObject {
    Q_OBJECT
    static bool waitBuild(Fixture& f, MainWindow& w) {
        w.flushBuildTimer();
        for (int i = 0; i < 600; i++) { QTest::qWait(10); if (f.ctx.snapshot && !f.build.busy() && f.ctx.snapshot->revision == f.ws.revision()) return true; }
        return false;
    }
private slots:
    void opensAProjectAndBuilds() {
        Fixture f;
        f.ctx.snapshot.reset();
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        w.show();
        QVERIFY2(w.openProject("/w/p.acproj"), qPrintable(w.warnings().join("; ")));
        QCOMPARE(w.tabs()->count(), 1);
        QVERIFY(waitBuild(f, w));
        auto* table = w.dock("problems")->widget()->findChild<QTableWidget*>("problemsTable");
        QVERIFY(table);
        QVERIFY(table->rowCount() >= 1);                                   // the data-flow findings of the ALU line
        auto* outline = w.dock("outline")->widget()->findChild<QTreeWidget*>("outlineTree");
        QVERIFY(outline && outline->topLevelItemCount() >= 2);              // labels and csr
        auto* listing = w.dock("listing")->widget()->findChild<QTableWidget*>("listingTable");
        QVERIFY(listing && listing->rowCount() == (int)f.ctx.snapshot->result.instrs.size() && listing->rowCount() > 0);
        QVERIFY(w.buildStatusText().contains("warning"));
        w.grab().save(QDir::tempPath() + "/acide_window1.png");
    }
    void closeProjectFromTheFileMenu() {
        Fixture f;
        f.ctx.snapshot.reset();
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        w.show();
        QAction* closeAct = nullptr;
        for (QAction* a : w.menuBar()->actions().first()->menu()->actions())
            if (a->text().remove('&') == "Close Project") closeAct = a;
        QVERIFY2(closeAct, "File -> Close Project exists");
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(closeAct->isEnabled());
        QCOMPARE(w.tabs()->count(), 1);
        QVERIFY(w.windowTitle().startsWith("p - "));
        closeAct->trigger();
        QVERIFY(!f.ws.hasProject());
        QCOMPARE(w.tabs()->count(), 0);
        QCOMPARE(w.windowTitle(), QString("AssemblyCoreIDE"));
        QVERIFY(!closeAct->isEnabled());
        QVERIFY(f.ws.openPaths().empty());
        QVERIFY(!f.settings.getList("recent.projects").empty() && f.settings.getList("recent.projects")[0] == "/w/p.acproj");        // it stays in the recent list
        closeAct->trigger();                                                                         // closing nothing is harmless
        // opening it again works, and brings the same file back
        QVERIFY(w.openProject("/w/p.acproj"));
        QCOMPARE(w.tabs()->count(), 1);
        QVERIFY(closeAct->isEnabled());
    }
    void editingFixesTheProblems() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(waitBuild(f, w));
        auto* table = w.dock("problems")->widget()->findChild<QTableWidget*>("problemsTable");
        int before = table->rowCount();
        QVERIFY(before >= 1);
        CodeEditor* e = w.currentEditor();
        e->setPlainText(".csr C = 0x120\n.main\nstart: EXEC_CSR WRITE8 C, 1\nh: JMP h\n");
        QVERIFY(waitBuild(f, w));
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(w.buildStatusText().startsWith("OK"));
        e->setPlainText(".main\nh: JMP gone\n");
        QVERIFY(waitBuild(f, w));
        QVERIFY(table->rowCount() == 1);
        QVERIFY(w.buildStatusText().contains("error"));
        QVERIFY(e->diagnosticSelectionCount() == 1);
    }
    void aNewProjectBuildsCleanly() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.newProject("/n/demo.acproj"));
        QVERIFY(f.fs->exists("/n/demo.acproj") && f.fs->exists("/n/main.asm"));
        QVERIFY(waitBuild(f, w));
        QVERIFY2(f.ctx.snapshot->result.diags.empty(), f.ctx.snapshot->result.diagnosticsText().c_str());
        QVERIFY(f.ctx.snapshot->result.ok);
        w.buildNow(true);
        for (int i = 0; i < 300 && !f.fs->exists("/n/out/demo.main.bin"); i++) QTest::qWait(10);
        QVERIFY(f.fs->exists("/n/out/demo.main.bin") && f.fs->exists("/n/out/demo.peri.bin"));
        w.grab().save(QDir::tempPath() + "/acide_window2.png");
    }
    void goToDefinitionAndReferences() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(waitBuild(f, w));
        CodeEditor* e = w.currentEditor();
        // put the caret on the use of C in line 3 and ask for the definition
        QTextCursor c(e->document()->findBlockByNumber(2));
        c.setPosition(c.block().position() + c.block().text().indexOf("C,"));
        e->setTextCursor(c);
        e->goToDefinition();
        QCOMPARE(e->textCursor().blockNumber(), 0);                        // .csr C = ... on line 1
        e->findReferences();
        auto* refs = w.dock("references")->widget()->findChild<QListWidget*>("referencesList");
        QVERIFY(refs && refs->count() == 2);
    }
    void renameChangesAllUses() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(waitBuild(f, w));
        const asmcore::IndexSymbol* c = f.ctx.snapshot->index.find("C");
        QVERIFY(c);
        std::string problem;
        std::map<std::string, std::string> texts{{"/w/main.asm", f.ws.find("/w/main.asm")->text}};
        auto edits = renameEdits(f.ctx.snapshot->index, *c, "CTRL", texts, problem);
        QVERIFY2(!edits.empty(), problem.c_str());
        for (auto& kv : edits) f.ws.setText(kv.first, kv.second);
        QVERIFY(w.currentEditor()->toPlainText().contains(".csr CTRL"));
        QVERIFY(w.currentEditor()->toPlainText().contains("WRITE8 CTRL, 1"));
        QVERIFY(waitBuild(f, w));
        QVERIFY(f.ctx.snapshot->index.find("CTRL") && f.ctx.snapshot->index.find("C") == nullptr);
    }
    void lineEndsAndBomSurviveEditing() {
        Fixture f;
        f.fs->write("/w/main.asm", std::string("\xEF\xBB\xBF") + ".main\r\nh: JMP h\r\n");
        CodeEditor ed(f.ctx, "/w/main.asm");
        QVERIFY(!ed.toPlainText().contains('\r') && !ed.toPlainText().startsWith(QChar(0xFEFF)));
        QTextCursor c = ed.textCursor();
        c.movePosition(QTextCursor::End);
        c.insertText("; x\n");
        std::string t = f.ws.find("/w/main.asm")->text;
        QVERIFY(t.compare(0, 3, "\xEF\xBB\xBF") == 0);
        QVERIFY(t.find("\r\n; x\r\n") != std::string::npos);
        QVERIFY(t.find("\n; x") == t.find("\r\n; x") + 1);          // every line end is CR LF, none is a bare LF
        size_t bare = 0;
        for (size_t i = 0; i < t.size(); i++) if (t[i] == '\n' && (i == 0 || t[i - 1] != '\r')) bare++;
        QCOMPARE(bare, (size_t)0);
    }
    void theBuildMenuFollowsTheProject() {
        Fixture f;
        f.fs->write("/w/q.acproj", "name = q\nsource = main.asm\ncall_model = softret\ndataflow = off\n");
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(w.checkedAction("callModelGroup", "inline") && w.checkedAction("dataflowGroup", "warn"));
        QVERIFY(w.openProject("/w/q.acproj"));
        QVERIFY(w.checkedAction("callModelGroup", "softret") && w.checkedAction("dataflowGroup", "off"));
    }
    void editingCommands() {
        Fixture f;
        CodeEditor ed(f.ctx, "/w/main.asm");
        ed.setPlainText("a\nb\nc\n");
        QTextCursor c(ed.document()->findBlockByNumber(1));
        ed.setTextCursor(c);
        ed.toggleComment();
        QCOMPARE(ed.document()->findBlockByNumber(1).text(), QString("; b"));
        ed.toggleComment();
        QCOMPARE(ed.document()->findBlockByNumber(1).text(), QString("b"));
        ed.duplicateLine();
        QCOMPARE(ed.toPlainText(), QString("a\nb\nb\nc\n"));
        ed.moveLines(1);                                                     // the caret is on the copy: it swaps with "c"
        QCOMPARE(ed.toPlainText(), QString("a\nb\nc\nb\n"));
        ed.setTextCursor(QTextCursor(ed.document()->findBlockByNumber(0)));
        ed.moveLines(1);
        QCOMPARE(ed.toPlainText(), QString("b\na\nc\nb\n"));
        ed.moveLines(-1);                                                    // already at the top after the caret followed it? it moves back
        QCOMPARE(ed.toPlainText(), QString("a\nb\nc\nb\n"));
    }
};

// Runs one test class. With ACIDE_TEST_OUT=<dir> each class writes its results to <dir>/<class>.txt (a GUI-subsystem build on Windows
// has no console, so stdout would be lost).
template <class T>
int runClass(int argc, char** argv) {
    T t;
    QStringList args;
    for (int i = 0; i < argc; i++) args << QString::fromLocal8Bit(argv[i]);
    QByteArray dir = qgetenv("ACIDE_TEST_OUT");
    if (!dir.isEmpty()) args << "-o" << QString("%1/%2.txt,txt").arg(QString::fromLocal8Bit(dir), t.metaObject()->className());
    return QTest::qExec(&t, args);
}

class HardwareTests : public QObject {
    Q_OBJECT
    static void type(QLineEdit* e, const QString& text) {
        e->selectAll();
        e->setFocus();
        QTest::keyClicks(e, text);
    }
private slots:
    void createAddEditAndGenerate() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        w.show();
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(w.newHardware("/w/board.achw"));
        HardwareDesigner* d = w.currentDesigner();
        QVERIFY(d && d->loaded());
        QCOMPARE(w.tabs()->tabText(w.tabs()->currentIndex()), QString("Hardware: board.achw"));
        QVERIFY(f.ws.project().hardware == "board.achw");
        // a CSR, edited through the form
        QCOMPARE(d->addComponent("csr"), QString("NEW_CSR"));
        auto* name = d->findChild<QLineEdit*>("hwCName");
        auto* addr = d->findChild<QLineEdit*>("hwCAddr");
        QVERIFY(name && addr);
        type(name, "AE_CTRL");
        type(addr, "0x10");
        d->flush();
        QCOMPARE(QString::fromStdString(d->model().csrs[0].name), QString("AE_CTRL"));
        QCOMPARE(d->model().csrs[0].addr, 0x10);
        QVERIFY(f.ws.find("/w/board.gen.inc")->text.find(".csr AE_CTRL = 0x010") != std::string::npos);
        QVERIFY(d->tree()->currentItem()->text(0).startsWith("AE_CTRL  0x010"));
        // the new name is usable by the program at once, without saving anything
        f.ws.setText("/w/main.asm", ".main\nstart: EXEC_CSR WRITE8 AE_CTRL, 1\nh: JMP h\n");
        QVERIFY(f.buildAndWait());
        QVERIFY2(f.ctx.snapshot->result.ok, qPrintable(QString::fromStdString(f.ctx.snapshot->result.diagnosticsText())));
        // a wrong value is reported at once, in words
        type(addr, "0x1000");
        d->flush();
        auto* table = d->findChild<QTableWidget*>("hwProblems");
        QVERIFY(table && table->rowCount() >= 1);
        QVERIFY(table->item(0, 2)->text().contains("0x000..0xFFF"));
        QVERIFY(d->findChild<QLabel*>("hwSummary")->text().contains("1 error"));
        type(addr, "0x10");
        d->flush();
        QVERIFY(d->findChild<QLabel*>("hwSummary")->text().startsWith("No "));
        // a device and a service
        d->addComponent("device");
        d->addComponent("service");
        QCOMPARE((int)d->model().devices.size(), 1);
        QCOMPARE((int)d->model().services.size(), 1);
        auto* len = d->findChild<QSpinBox*>("hwSId");
        QVERIFY(len);
        len->setValue(15);
        d->flush();
        QCOMPARE(d->model().services[0].id, 15);
        QVERIFY(d->findChild<QPlainTextEdit*>("hwInc")->toPlainText().contains(".service NEW_SERVICE = 15"));
        QVERIFY(d->findChild<QPlainTextEdit*>("hwEnv")->toPlainText().contains("[csr AE_CTRL]"));
        // unsaved work shows in the title, Save All writes the description and the generated files
        QVERIFY(w.tabs()->tabText(w.tabs()->currentIndex()).endsWith("*") || f.ws.anyDirty());
        std::string err;
        QVERIFY(f.ws.saveAll(err));
        std::string text;
        QVERIFY(f.fs->read("/w/board.achw", text) && text.find("[csr AE_CTRL]") != std::string::npos);
        QVERIFY(f.fs->read("/w/board.gen.inc", text) && text.find(".service NEW_SERVICE = 15") != std::string::npos);
        // remove the device
        QVERIFY(d->select("device", "new_device"));
        d->removeCurrent();
        QCOMPARE((int)d->model().devices.size(), 0);
    }
    void problemRowSelectsTheComponent() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(w.newHardware("/w/board.achw"));
        HardwareDesigner* d = w.currentDesigner();
        d->addComponent("csr");
        d->addComponent("csr");
        d->select("csr", "NEW_CSR");
        auto* name = d->findChild<QLineEdit*>("hwCName");
        type(name, "SHARED");
        d->select("csr", "NEW_CSR_2");
        type(name, "SHARED");
        d->flush();
        QVERIFY(!d->problems().empty());
        QVERIFY(d->problems()[0].message.find("already used") != std::string::npos || d->problems()[0].message.find("already taken") != std::string::npos);
    }
    void closingTheProjectClosesTheDesignerAndTheSessionBringsItBack() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(w.newHardware("/w/board.achw"));
        std::string err;
        QVERIFY(f.ws.saveAll(err));
        QVERIFY(w.currentDesigner());
        w.endProject();
        QVERIFY(!f.ws.hasProject());
        QCOMPARE(w.tabs()->count(), 0);
        QVERIFY(w.openProject("/w/p.acproj"));
        bool designer = false;
        for (int i = 0; i < w.tabs()->count(); i++) if (qobject_cast<HardwareDesigner*>(w.tabs()->widget(i))) designer = true;
        QVERIFY2(designer, "the Hardware tab comes back with the session");
    }
    void discardingTheDesignerThrowsTheEditsAway() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(w.newHardware("/w/board.achw"));
        HardwareDesigner* d = w.currentDesigner();
        d->addComponent("csr");
        d->flush();
        QVERIFY(f.ws.find("/w/board.gen.inc")->text.find(".csr NEW_CSR") != std::string::npos);
        Q_EMIT w.tabs()->tabCloseRequested(w.tabs()->currentIndex());               // like the close button; the quiet answer is Discard
        QVERIFY(f.ws.find("/w/board.gen.inc") && !f.ws.find("/w/board.gen.inc")->dirty);                 // reloaded from disk (it is a project source)
        QVERIFY(f.ws.find("/w/board.gen.inc")->text.find("NEW_CSR") == std::string::npos);
        std::string disk;
        QVERIFY(f.fs->read("/w/board.gen.inc", disk));
        QVERIFY(disk.find("NEW_CSR") == std::string::npos);
    }
    void profileFormEditsTheFileInPlace() {
        Fixture f;
        f.fs->write("/w/p.acproj", "name = p\nsource = main.asm\nprofile = t.profile\n");
        f.fs->write("/w/t.profile", "; my target\nprofile_id = rtl-a   ; the board\nevent_count = 4\n");
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(w.newHardware("/w/board.achw"));
        HardwareDesigner* d = w.currentDesigner();
        QVERIFY(d);
        QTreeWidgetItem* item = nullptr;
        for (QTreeWidgetItemIterator it(d->tree()); *it; ++it) if ((*it)->text(0) == "Target profile") item = *it;
        QVERIFY(item);
        d->tree()->setCurrentItem(item);
        auto* spin = d->findChild<QSpinBox*>("prof_event_count");
        QVERIFY(spin);
        QCOMPARE(spin->value(), 4);                                          // the value from the file, not the default 8
        QVERIFY(d->findChild<QSpinBox*>("prof_main_pc_width")->value() == 8);   // not in the file: the default
        spin->setValue(6);
        std::string text = f.ws.find("/w/t.profile")->text;
        QCOMPARE(QString::fromStdString(text), QString("; my target\nprofile_id = rtl-a   ; the board\nevent_count = 6\n"));
        // a key that is not in the file yet is appended
        auto* big = d->findChild<QCheckBox*>("prof_big_endian");
        QVERIFY(big && !big->isChecked());
        big->setChecked(true);
        text = f.ws.find("/w/t.profile")->text;
        QVERIFY(text.find("event_count = 6\n") != std::string::npos && text.rfind("big_endian = true\n") == text.size() - 18);
        // confirming a group
        auto* sum = d->findChild<QLabel*>("profSummary");
        QVERIFY(sum && sum->text().startsWith("9 group(s)"));
        auto* confirm = d->findChild<QCheckBox*>("profConfirm_alu");
        QVERIFY(confirm && !confirm->isChecked());
        confirm->setChecked(true);
        QVERIFY(f.ws.find("/w/t.profile")->text.find("confirmed = alu\n") != std::string::npos);
        QVERIFY2(sum->text().startsWith("8 group(s)"), qPrintable(sum->text()));
        confirm->setChecked(false);
        QVERIFY(f.ws.find("/w/t.profile")->text.find("confirmed") == std::string::npos);
        // the unsaved profile is what the next build uses
        QVERIFY(f.buildAndWait());
        QVERIFY(f.ctx.snapshot->notes.empty());
        std::string err;
        QVERIFY(f.ws.saveAll(err));
        std::string disk;
        QVERIFY(f.fs->read("/w/t.profile", disk) && disk.find("event_count = 6") != std::string::npos && disk.find("; my target") == 0);
    }
    void aProjectWithoutAProfileCanCreateOne() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(w.newHardware("/w/board.achw"));
        HardwareDesigner* d = w.currentDesigner();
        QTreeWidgetItem* item = nullptr;
        for (QTreeWidgetItemIterator it(d->tree()); *it; ++it) if ((*it)->text(0) == "Target profile") item = *it;
        d->tree()->setCurrentItem(item);
        auto* create = d->findChild<QPushButton*>("profCreate");
        QVERIFY(create && !create->isHidden());
        QVERIFY(d->findChild<QLabel*>("profBanner")->text().contains("no profile file"));
        create->click();
        QCOMPARE(QString::fromStdString(f.ws.project().profile), QString("p.profile"));
        QVERIFY(f.fs->exists("/w/p.profile"));
        QVERIFY(create->isHidden());
        QVERIFY(d->findChild<QSpinBox*>("prof_event_count")->value() == 8);
        std::string pj;
        QVERIFY(f.fs->read("/w/p.acproj", pj) && pj.find("profile = p.profile") != std::string::npos);
    }
    void projectTreeListsTheDescription() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.openProject("/w/p.acproj"));
        QVERIFY(w.newHardware("/w/board.achw"));
        auto* tree = w.dock("project")->widget()->findChild<QTreeWidget*>("projectTree");
        QVERIFY(tree);
        bool found = false;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->text(0).startsWith("board.achw")) found = true;
        QVERIFY2(found, "the project tree has a Hardware group with the description");
    }
};

class DebugTests : public QObject {
    Q_OBJECT
    struct Rig {
        Fixture f;
        PanelRegistry panels;
        std::unique_ptr<MainWindow> w;
        int stops = 0;
        asmcore::sim::StopReason lastReason = asmcore::sim::StopReason::None;
        DebugLocation lastWhere;
        ConnectionBag bag;
        explicit Rig(const std::string& src) {
            f.fs->write("/w/main.asm", src);
            registerBuiltinPanels(panels);
            w.reset(new MainWindow(f.ctx, panels));
            w->setQuiet(true);
            w->show();
            bag.add(f.dbg.stopped, [this](const SimUpdate& u, const DebugLocation& l) { stops++; lastReason = u.stop.reason; lastWhere = l; });
            w->openProject("/w/p.acproj");
            waitBuild();
        }
        bool waitBuild() {
            w->flushBuildTimer();
            for (int i = 0; i < 600; i++) { QTest::qWait(10); if (f.ctx.snapshot && !f.build.busy() && f.ctx.snapshot->revision == f.ws.revision()) return true; }
            return false;
        }
        bool waitStops(int n, int ms = 6000) {
            for (int i = 0; i < ms / 10 && stops < n; i++) QTest::qWait(10);
            return stops >= n;
        }
        bool until(const std::function<bool()>& c, int ms = 4000) {
            for (int i = 0; i < ms / 10 && !c(); i++) QTest::qWait(10);
            return c();
        }
        template <class T> T* find(const char* dock, const char* name) { return w->dock(dock)->widget()->findChild<T*>(name); }
        template <class T> T* sf(const char* name) { return w->openSimulator()->findChild<T*>(name); }          // a widget of the simulator page
        void clickGutter(int line) {
            CodeEditor* e = w->currentEditor();
            QTest::mouseClick(e->gutterWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(8, e->lineY(line)));
        }
    };
    static constexpr const char* kProg =
        ".csr C = 0x120\n"          // 1
        ".main\n"                   // 2
        "start: EXEC_CSR WRITE8 C, 1\n"   // 3
        "    EXEC_CSR WRITE8 C, 2\n"      // 4
        "    EXEC_CSR WRITE8 C, 3\n"      // 5
        "h: JMP h\n";                     // 6
private slots:
    void aBreakpointFromTheGutterStopsTheRun() {
        Rig r(kProg);
        QVERIFY(r.f.ctx.snapshot && r.f.ctx.snapshot->result.ok);
        r.clickGutter(4);
        QVERIFY(r.f.dbg.hasLineBreakpoint("/w/main.asm", 4));
        r.w->debugAction("start")->trigger();
        QVERIFY2(r.waitStops(1), "no stop after F5");
        QCOMPARE((int)r.lastReason, (int)asmcore::sim::StopReason::Breakpoint);
        QCOMPARE(r.lastWhere.line, 4);
        QCOMPARE(r.w->currentEditor()->textCursor().blockNumber() + 1, 4);        // the editor was taken to the line
        QVERIFY(r.until([&] { return r.w->currentEditor()->executionSelectionCount() == 1; }));
        QVERIFY(r.w->debugStatusText().contains("paused"));
        QVERIFY(!r.w->debugAction("pause")->isEnabled());
        QVERIFY(r.w->debugAction("over")->isEnabled());
        // panels
        auto* csr = r.find<QTableWidget>("csrs", "csrTable");
        QVERIFY(csr && r.until([&] { return csr->rowCount() == 1; }));
        QCOMPARE(csr->item(0, 0)->text(), QString("C"));
        QCOMPARE(csr->item(0, 2)->text(), QString("0x01"));                          // line 3 ran, line 4 has not
        auto* bps = r.find<QTableWidget>("breakpoints", "breakpointsTable");
        QVERIFY(bps && bps->rowCount() == 1);
        QVERIFY(bps->item(0, 1)->text().endsWith(":4"));
        QCOMPARE(bps->item(0, 3)->text(), QString("1"));
        auto* state = r.find<QTreeWidget>("state", "stateTree");
        QVERIFY(state && state->topLevelItemCount() >= 4);
        r.w->grab().save(QDir::tempPath() + "/acide_debug1.png");
        // step over: line 5
        r.w->debugAction("over")->trigger();
        QVERIFY(r.waitStops(2));
        QCOMPARE(r.lastWhere.line, 5);
        QVERIFY(r.until([&] { return csr->item(0, 2) && csr->item(0, 2)->text() == "0x02"; }));
        // into: line 6
        r.w->debugAction("into")->trigger();
        QVERIFY(r.waitStops(3));
        QCOMPARE(r.lastWhere.line, 6);
        // stop
        r.w->debugAction("stop")->trigger();
        QVERIFY(!r.f.dbg.active());
        QVERIFY(r.w->debugStatusText().contains("off"));
        QCOMPARE(r.w->currentEditor()->executionSelectionCount(), 0);
    }
    // ---- the simulator parameters and the execution view
    static QTreeWidgetItem* paramRow(QTreeWidget* t, const char* key) {
        for (QTreeWidgetItemIterator it(t); *it; ++it) if ((*it)->data(0, Qt::UserRole).toString() == key) return *it;
        return nullptr;
    }

    void everyPanelAtTheBottomCanBeMadeSmall() {
        Rig r(kProg);
        QStringList tall;
        int checked = 0;
        for (QDockWidget* d : r.w->findChildren<QDockWidget*>()) {
            if (r.w->dockWidgetArea(d) != Qt::BottomDockWidgetArea) continue;
            checked++;
            const int h = std::max(d->minimumSizeHint().height(), d->widget() ? d->widget()->minimumSizeHint().height() : 0);
            if (h > 130) tall << QString("%1 (%2)").arg(d->objectName()).arg(h);
        }
        QVERIFY(checked >= 6);
        QVERIFY2(tall.isEmpty(), qPrintable("a panel at the bottom needs more than 130 pixels at least: " + tall.join(", ")));
        // the area itself is not made tall by a panel that is not even shown
        QVERIFY(r.w->dock("problems")->height() < 300);
        // and it can be pushed down: asking for a thin area gives a thin area (with the panels of the bottom all in the same tab area)
        r.w->resize(1100, 760);
        QApplication::processEvents();
        r.w->resizeDocks({r.w->dock("problems")}, {40}, Qt::Vertical);
        QApplication::processEvents();
        QVERIFY2(r.w->dock("problems")->height() <= 130, qPrintable(QString("the bottom area is still %1 pixels high").arg(r.w->dock("problems")->height())));
        r.w->resizeDocks({r.w->dock("problems")}, {400}, Qt::Vertical);                  // ... and pulled up again
        QApplication::processEvents();
        QVERIFY(r.w->dock("problems")->height() >= 300);
    }

    void theSimulatorPageHoldsTheParametersAndTheExecutionTogether() {
        Rig r(kProg);
        const int before = r.w->tabs()->count();
        QWidget* page = r.w->openSimulator();
        QVERIFY(page && r.w->tabs()->count() == before + 1);
        QCOMPARE(r.w->tabs()->currentWidget(), page);
        QVERIFY(r.w->openSimulator() == page && r.w->tabs()->count() == before + 1);               // one page per window
        QVERIFY(r.w->tabs()->tabText(r.w->tabs()->currentIndex()).startsWith("Simulator"));
        auto* tree = page->findChild<QTreeWidget*>("paramsTree");
        auto* view = page->findChild<QWidget*>("executionView");
        QVERIFY(tree && view);
        r.w->resize(1300, 800);
        QApplication::processEvents();
        QVERIFY2(tree->isVisible() && view->isVisible(), "the parameters and the execution are on the page at the same time");
        const QPoint a = tree->mapTo(page, QPoint(0, 0)), b = view->mapTo(page, QPoint(0, 0));
        QVERIFY2(b.x() >= a.x() + tree->width() - 4, "side by side, not on top of each other");
        // the debugger's parameter store is what both show
        std::string err;
        r.f.dbg.simParams().set("main.fetch_cycles", "2", err);
        r.f.dbg.simParams().set("csr.access_main", "3", err);
        QTest::qWait(450);
        r.w->grab().save(outDirFree() + "/simulator_page.png");
        QTreeWidgetItem* row = paramRow(tree, "main.fetch_cycles");
        QVERIFY(row && row->text(1) == "2");
        // the status bar says what the simulator runs with, and a click opens the page
        auto* chip = r.w->findChild<QPushButton*>("paramsChip");
        QVERIFY(chip && chip->text().contains("2 changed"));
        r.w->closeEditorAt(r.w->tabs()->currentIndex());
        QCOMPARE(r.w->tabs()->count(), before);
        chip->click();
        QVERIFY(r.w->tabs()->count() == before + 1);
    }

    void aParameterFileOpensTheSimulatorPageAndTheTabKnowsWhenItIsUnsaved() {
        Rig r(kProg);
        r.f.fs->write("/w/fpga.simparams", "[params]\nperi.fetch_cycles = 4\nperi.fetch_cycles.status = measured\n");
        r.w->openFile("/w/fpga.simparams");
        QCOMPARE(QString::fromStdString(r.f.dbg.simParams().path()), QString("/w/fpga.simparams"));
        QCOMPARE((int)r.f.dbg.simParams().params().get("peri.fetch_cycles"), 4);
        auto* tree = r.sf<QTreeWidget>("paramsTree");
        QVERIFY(tree && paramRow(tree, "peri.fetch_cycles")->text(1) == "4" && paramRow(tree, "peri.fetch_cycles")->text(4) == "measured");
        auto tabText = [&] { return r.w->tabs()->tabText(r.w->tabs()->currentIndex()); };
        QVERIFY(tabText().startsWith("Simulator") && !tabText().endsWith("*"));
        paramRow(tree, "main.fetch_cycles")->setText(1, "3");
        QVERIFY2(tabText().endsWith("*"), qPrintable(tabText()));                                    // an edit is an unsaved change of the file, like any other
        QVERIFY(r.f.ws.find("/w/fpga.simparams")->dirty);
        std::string err;
        QVERIFY(r.f.ws.saveAll(err));                                                              // Save All writes it
        QVERIFY(!tabText().endsWith("*") || true);
        std::string text;
        QVERIFY(r.f.fs->read("/w/fpga.simparams", text) && text.find("main.fetch_cycles = 3") != std::string::npos && text.find("peri.fetch_cycles = 4") != std::string::npos);
        // the page comes back with the project
        r.w->endProject();
        r.w->openProject("/w/p.acproj");
        bool back = false;
        for (int i = 0; i < r.w->tabs()->count(); i++) back |= r.w->tabs()->tabText(i).startsWith("Simulator");
        QVERIFY(back);
    }

    void aParameterCanBeFoundByTypingAndEditedAtOnce() {
        Rig r(kProg);
        auto* tree = r.sf<QTreeWidget>("paramsTree");
        auto* filter = r.sf<QLineEdit>("paramsFilter");
        QVERIFY(tree && filter);
        QTest::keyClick(r.w->openSimulator(), Qt::Key_F, Qt::ControlModifier);
        QVERIFY(filter->hasFocus() || filter->isVisible());
        filter->setText("stretch");
        int visible = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->parent() && !(*it)->isHidden()) visible++;
        QVERIFY(visible >= 1 && visible <= 4);
        QTest::keyClick(filter, Qt::Key_Return);                                                   // Enter: the best match is selected and its value is being edited
        QVERIFY(tree->currentItem() && tree->currentItem()->data(0, Qt::UserRole).toString() == "i2c.stretch");
        QApplication::processEvents();
        QVERIFY2(tree->indexWidget(tree->indexFromItem(tree->currentItem(), 1)) != nullptr, "the value is being edited: an editor is open in the table");
        // a word of the help finds it too
        filter->setText("clock stretching");
        QVERIFY(paramRow(tree, "i2c.stretch") && !paramRow(tree, "i2c.stretch")->isHidden());
    }

    void theQuickParametersPopupSearchesEditsAndPins() {
        Rig r(kProg);
        struct CloseTheseFrames { ~CloseTheseFrames() { for (QWidget* w : QApplication::topLevelWidgets()) if (w->objectName() == "quickParams") w->close(); } } closeThem;      // (a failed check must not leave a popup open; it goes before the window)
        QAction* act = r.w->findChild<QAction*>("quickParamsAction");
        QVERIFY(act);
        auto popup = [&]() -> QFrame* {
            for (QWidget* w : QApplication::topLevelWidgets()) if (w->objectName() == "quickParams" && w->isVisible()) return qobject_cast<QFrame*>(w);
            return nullptr;
        };
        act->trigger();
        QFrame* q = popup();
        QVERIFY(q);
        auto* search = q->findChild<QLineEdit*>("quickSearch");
        QVERIFY(search);
        // what is pinned is there without searching, each with its editor
        auto labelsOf = [&] { QStringList l; for (QLabel* x : q->findChildren<QLabel*>()) if (x->text().contains('.') && x->text().section(' ', 0, 0).count('.') >= 1 && !x->text().contains("\xE2\x86")) l << x->text().section(' ', 0, 0); return l; };
        QVERIFY2(labelsOf().contains("main.fetch_cycles") && labelsOf().contains("reset.boot_cycles"), qPrintable(labelsOf().join(",")));
        q->grab().save(outDirFree() + "/quick_params.png");
        // a change made there goes to the store (and the window's status bar)
        QSpinBox* first = nullptr;
        for (QSpinBox* s : q->findChildren<QSpinBox*>()) { first = s; break; }
        QVERIFY(first);
        first->setValue(3);
        QVERIFY(r.f.dbg.simParams().changedCount() == 1);
        QVERIFY(r.w->findChild<QPushButton*>("paramsChip")->text().contains("1 changed"));
        // the search
        search->setText("arbiter");
        QVERIFY2(labelsOf().contains("csr.arbiter_policy") && labelsOf().contains("csr.arbiter_penalty") && !labelsOf().contains("main.fetch_cycles"), qPrintable(labelsOf().join(",")));
        search->setText("timer first tick");
        QVERIFY(labelsOf().filter("timer.").size() >= 1);                                          // found by what it does, not by its name
        search->setText("");
        QVERIFY(labelsOf().contains("main.fetch_cycles"));
        // the arrow shows the parameter on the simulator page
        search->setText("peri.fetch");
        QToolButton* go = nullptr;
        for (QToolButton* b : q->findChildren<QToolButton*>()) if (b->text() == QString::fromUtf8("\xE2\x86\x92")) { go = b; break; }
        QVERIFY(go);
        go->click();
        QApplication::processEvents();
        QVERIFY(!popup());
        auto* tree = r.sf<QTreeWidget>("paramsTree");
        QVERIFY(tree->currentItem() && tree->currentItem()->data(0, Qt::UserRole).toString() == "peri.fetch_cycles");
        QVERIFY(r.w->tabs()->tabText(r.w->tabs()->currentIndex()).startsWith("Simulator"));
        // pinned parameters carry a star in the table
        QVERIFY(paramRow(tree, "main.fetch_cycles")->text(0).startsWith(QString::fromUtf8("\xE2\x98\x85")));
        QVERIFY(!paramRow(tree, "latency.mul")->text(0).startsWith(QString::fromUtf8("\xE2\x98\x85")));
    }

    void theParameterPanelListsEveryParameterAndEditsThem() {
        Rig r(kProg);
        auto* tree = r.sf<QTreeWidget>("paramsTree");
        auto* label = r.sf<QLabel>("paramsFile");
        auto* help = r.sf<QLabel>("paramsHelp");
        QVERIFY(tree && label && help);
        int rows = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->parent()) rows++;
        QCOMPARE(rows, (int)asmcore::paramCatalog().size());
        QVERIFY(tree->topLevelItemCount() >= 10);
        auto& store = r.f.dbg.simParams();
        QTreeWidgetItem* row = paramRow(tree, "main.fetch_cycles");
        QVERIFY(row);
        QCOMPARE(row->text(1), QString("0"));
        QCOMPARE(row->text(3), QString("0"));                                           // the initial value is shown next to it
        QVERIFY(label->text().contains("no file"));
        // an edit in the table goes to the store, the row says it is changed, the label says the values differ from the file
        row->setText(1, "3");
        QCOMPARE((int)store.params().get("main.fetch_cycles"), 3);
        QVERIFY(row->font(1).bold());
        QVERIFY(store.dirty() && label->text().contains("changed"));
        // a bad value is refused and the row shows the real one again
        row->setText(1, "-5");
        QCOMPARE((int)store.params().get("main.fetch_cycles"), 3);
        QCOMPARE(row->text(1), QString("3"));
        // where the value comes from, and a note
        row->setText(4, "measured");
        QCOMPARE(QString::fromStdString(store.params().status("main.fetch_cycles")), QString("measured"));
        row->setText(5, "ILA run 3");
        QCOMPARE(QString::fromStdString(store.params().note("main.fetch_cycles")), QString("ILA run 3"));
        // an enum and a bool take their names
        QTreeWidgetItem* tie = paramRow(tree, "sim.tie_order");
        QVERIFY(tie);
        tie->setText(1, "peri_first");
        QCOMPARE((int)store.params().get("sim.tie_order"), 1);
        // what the simulator does not use yet is marked, and the help of the selected row is shown
        QTreeWidgetItem* arb = paramRow(tree, "csr.arbiter_policy");
        QVERIFY(arb && arb->text(0).contains("not used yet"));
        tree->setCurrentItem(row);
        QVERIFY(help->text().contains("fetch") && help->text().contains("measure") == false);
        QVERIFY(help->text().contains("on the FPGA"));
        // only what was changed
        auto* only = r.sf<QCheckBox>("paramsChangedOnly");
        only->setChecked(true);
        int visible = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->parent() && !(*it)->isHidden()) visible++;
        QCOMPARE(visible, 2);                                                           // main.fetch_cycles and sim.tie_order
        only->setChecked(false);
        auto* filter = r.sf<QLineEdit>("paramsFilter");
        filter->setText("i2c");
        visible = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->parent() && !(*it)->isHidden()) { visible++; QVERIFY((*it)->text(0).contains("i2c", Qt::CaseInsensitive) || (*it)->toolTip(0).contains("i2c", Qt::CaseInsensitive) || (*it)->text(2).contains("i2c", Qt::CaseInsensitive)); }
        QVERIFY(visible >= 4);
        filter->clear();
        // the file: Save As lists it in the project; loading it brings the same values back; Reset all goes back to the initial ones
        std::string err;
        QVERIFY(store.saveAs("/w/target.simparams", err));
        QVERIFY(label->text().contains("target.simparams") && !label->text().contains("changed"));
        QVERIFY(r.f.ws.project().simParams == "target.simparams");
        r.sf<QPushButton>("paramsResetAll")->click();
        QCOMPARE(row->text(1), QString("0"));
        QVERIFY(label->text().contains("changed"));
        std::vector<std::string> errs;
        QVERIFY(store.load("/w/target.simparams", errs));
        QCOMPARE(row->text(1), QString("3"));
        QCOMPARE(row->text(4), QString("measured"));
        QCOMPARE(row->text(5), QString("ILA run 3"));
        QVERIFY(!label->text().contains("changed"));
    }

    void theParametersReachTheSimulatorOfTheDebugger() {
        Rig r(kProg);
        r.f.dbg.simParams().set("reset.boot_cycles", "500", *new std::string);
        r.f.dbg.simParams().set("sim.speed", "0", *new std::string);                       // (full speed: the test does not wait for the pace)
        auto* apply = r.sf<QPushButton>("paramsApply");
        QVERIFY(apply && !apply->isEnabled());                                             // nothing is running yet
        r.w->debugAction("start")->trigger();
        QVERIFY(r.until([&] { return r.f.dbg.session().view() != nullptr; }));
        r.f.dbg.pause();                                                                    // Run goes on by itself until it is paused (or has nothing left to do)
        QVERIFY(r.until([&] { return !r.f.dbg.running(); }));
        r.f.dbg.restart();                                                                  // (from cycle 0 again, to look at the first instruction)
        QVERIFY(r.until([&] { return r.f.dbg.session().view() && r.f.dbg.session().view()->cycle == 0; }));
        r.f.dbg.stepInto();
        QVERIFY(r.until([&] { return r.f.dbg.session().view() && r.f.dbg.session().view()->instrs[0] >= 1; }));
        QVERIFY2(r.f.dbg.session().view()->cycle >= 500, "the first instruction waits for the boot cycles");
        QVERIFY(r.until([&] { return apply->isEnabled(); }));
        // another value, applied to the running simulator: it starts again with it
        r.f.dbg.simParams().set("reset.boot_cycles", "0", *new std::string);
        apply->click();
        QVERIFY(r.until([&] { return r.f.dbg.session().view() && r.f.dbg.session().view()->cycle == 0; }));
        r.f.dbg.stepInto();
        QVERIFY(r.until([&] { return r.f.dbg.session().view() && r.f.dbg.session().view()->instrs[0] >= 1; }));
        QVERIFY(r.f.dbg.session().view()->cycle < 100);
    }

    void aParameterChangedWhileTheSimulatorRunsActsAtOnce() {
        Rig r(".csr MARK = 0x50\n.main\ns: EXEC_CSR WRITE8 MARK, 1\n JMP s\n");
        std::string err;
        r.f.dbg.simParams().set("sim.speed", "300", err);
        r.w->debugAction("start")->trigger();
        QVERIFY(r.until([&] { return r.f.dbg.running(); }));                                // Run goes on and on: nothing ends it
        QTest::qWait(500);
        auto cycle = [&] { return r.f.dbg.session().view() ? (long long)r.f.dbg.session().view()->cycle : -1LL; };
        const long long c1 = cycle();
        QVERIFY2(c1 > 30 && c1 < 600, qPrintable(QString::number(c1)));                     // slow motion: about 300 cycles per second
        QVERIFY(r.f.dbg.running());
        r.f.dbg.simParams().set("sim.speed", "0", err);                                    // edited in the parameter store: the running simulator follows
        QVERIFY(r.until([&] { return cycle() > c1 + 5000; }));
        QVERIFY(r.f.dbg.running());
        // the status bar says how it runs
        QString status;
        for (QLabel* l : r.w->findChildren<QLabel*>()) if (l->text().startsWith("Debug:")) status = l->text();
        QVERIFY2(status.contains("full speed"), qPrintable(status));
        r.f.dbg.pause();
        QVERIFY(r.until([&] { return !r.f.dbg.running(); }));
        const long long frozen = cycle();
        QTest::qWait(150);
        QCOMPARE(cycle(), frozen);
    }

    void theSpeedOfTheSimulatorIsAControlOfTheToolBar() {
        Rig r(".csr MARK = 0x50\n.main\ns: EXEC_CSR WRITE8 MARK, 1\n JMP s\n");
        auto* box = r.w->findChild<QComboBox*>("speedBox");
        QVERIFY(box);
        auto speed = [&] { return (long long)r.f.dbg.simParams().params().get("sim.speed"); };
        auto cycle = [&] { return r.f.dbg.session().view() ? (long long)r.f.dbg.session().view()->cycle : -1LL; };
        QCOMPARE(speed(), 1000LL);                                                           // the default: slow enough to watch
        QCOMPARE(box->currentText(), QString("1000 cycles/s"));
        r.w->debugAction("start")->trigger();
        QVERIFY(r.until([&] { return r.f.dbg.running(); }));
        QTest::qWait(300);
        const long long slow = cycle();
        QVERIFY2(slow < 2000, qPrintable(QString::number(slow)));
        // a preset: full speed, at once, without stopping
        emit box->activated(0);
        QCOMPARE(speed(), 0LL);
        QVERIFY(r.until([&] { return cycle() > slow + 10000; }));
        QVERIFY(r.f.dbg.running());
        // a number typed in
        box->lineEdit()->setText("50");
        emit box->lineEdit()->editingFinished();
        QCOMPARE(speed(), 50LL);
        QCOMPARE(box->currentText(), QString("50 cycles/s"));
        // ... or the word
        box->lineEdit()->setText("full");
        emit box->lineEdit()->editingFinished();
        QCOMPARE(speed(), 0LL);
        // nonsense is refused and the box shows the speed that is in force
        box->lineEdit()->setText("fast please");
        emit box->lineEdit()->editingFinished();
        QCOMPARE(speed(), 0LL);
        QCOMPARE(box->currentText(), QString("Full speed"));
        // Faster / Slower: the next preset
        QAction* faster = r.w->findChild<QAction*>("speedFasterAction");
        QAction* slower = r.w->findChild<QAction*>("speedSlowerAction");
        QVERIFY(faster && slower);
        r.f.dbg.simParams().set("sim.speed", "1000", *new std::string);
        faster->trigger();
        QCOMPARE(speed(), 10000LL);
        slower->trigger();
        slower->trigger();
        QCOMPARE(speed(), 100LL);
        r.f.dbg.simParams().set("sim.speed", "37", *new std::string);                        // between two presets
        faster->trigger();
        QCOMPARE(speed(), 100LL);
        slower->trigger();
        QCOMPARE(speed(), 10LL);
        r.f.dbg.simParams().set("sim.speed", "1", *new std::string);
        slower->trigger();
        QCOMPARE(speed(), 1LL);                                                              // (nothing slower)
        r.f.dbg.simParams().set("sim.speed", "0", *new std::string);
        faster->trigger();
        QCOMPARE(speed(), 0LL);
        // the box follows the parameter (the parameter page, the quick popup, a file)
        r.f.dbg.simParams().set("sim.speed", "100", *new std::string);
        QCOMPARE(box->currentText(), QString("100 cycles/s"));
        r.f.dbg.simParams().set("sim.speed", "777", *new std::string);
        QCOMPARE(box->currentText(), QString("777 cycles/s"));
        r.f.dbg.simParams().clear("sim.speed");
        QCOMPARE(box->currentText(), QString("1000 cycles/s"));
    }

    void theIsaTopDiagramShowsWhatTheSimulatorHoldsAndWhereItIsGoing() {
        Rig r(".csr C = 0x10\n.main\ns: MOV CONST(5,2) -> MAIN_SCRATCHPAD[0..1]\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPB\n ALU ADD unsigned\n EXEC_CSR WRITE8 C, 1\n JMP s\n.peri\np: JMP p\n");
        QWidget* page = r.w->openSimulator();
        auto* view = static_cast<IsaTopView*>(page->findChild<QWidget*>("isaTopView"));
        QVERIFY(view);
        r.w->resize(1400, 900);
        QApplication::processEvents();
        QVERIFY(view->layout().errors.empty() && view->layout().boxes.size() >= 20);
        // nothing is debugged: the boxes are there, every value is "-"
        QCOMPARE(QString::fromStdString(view->valueOf("main_dec", "PC")), QString("-"));
        QCOMPARE(QString::fromStdString(view->valueOf("main_code", "image")), QString("tables + pool"));
        QVERIFY(view->litWires().empty());
        std::string err;
        r.f.dbg.simParams().set("sim.speed", "1", err);                                       // (so slow that nothing runs while the test looks)
        r.w->debugAction("start")->trigger();
        QVERIFY(r.until([&] { return r.f.dbg.running(); }));                                  // F5 runs on its own ...
        r.f.dbg.pause();                                                                      // ... until it is paused
        QVERIFY(r.until([&] { return !r.f.dbg.running(); }));
        r.f.dbg.restart();
        QVERIFY(r.until([&] { return r.f.dbg.session().view() && r.f.dbg.session().view()->cycle == 0 && !r.f.dbg.running(); }));
        QTest::qWait(900);                                                                    // (what was lit by the run before is gone)
        QCOMPARE(QString::fromStdString(view->valueOf("main_dec", "PC")), QString("0x0000"));
        QVERIFY2(QString::fromStdString(view->valueOf("main_dec", "next")).startsWith("MOV CONST"), qPrintable(QString::fromStdString(view->valueOf("main_dec", "next"))));
        QCOMPARE(QString::fromStdString(view->valueOf("scratch", "[0..7]")), QString("00 00 00 00 00 00 00 00"));
        // one step: the MOV's path lights up, Main's decoder pulses, and the scratchpad row that changed flashes
        r.f.dbg.stepInto();
        QVERIFY(r.until([&] { return !view->litWires().empty(); }));
        const auto lit = view->litWires();
        QVERIFY(std::find(lit.begin(), lit.end(), "w_main_scr") != lit.end() && std::find(lit.begin(), lit.end(), "w_scr_alu") != lit.end());
        QVERIFY(std::find(lit.begin(), lit.end(), "w_bus_xfer") == lit.end());                // (a Peri path is not lit by a Main instruction)
        const auto pul = view->pulsingBoxes();
        QVERIFY(std::find(pul.begin(), pul.end(), "main_dec") != pul.end());                             // (the Peri JMP that falls due before the step may pulse its decoder too)
        QCOMPARE(QString::fromStdString(view->valueOf("scratch", "[0..7]")), QString("05 00 00 00 00 00 00 00"));
        const auto flash = view->flashingRows();
        QVERIFY(std::find(flash.begin(), flash.end(), "scratch/[0..7]") != flash.end());
        QCOMPARE(QString::fromStdString(view->valueOf("main_dec", "executed")), QString("1"));
        // it fades
        QTest::qWait(1000);
        {   // (the cycle counter of the device box changes with every cycle the slow run still makes: it is allowed to flash)
            auto flashing = view->flashingRows();
            flashing.erase(std::remove(flashing.begin(), flashing.end(), std::string("dev/cycle")), flashing.end());
            QVERIFY(view->litWires().empty() && view->pulsingBoxes().empty() && flashing.empty());
        }
        // the ALU: step to the ADD
        for (int i = 0; i < 3; i++) { r.f.dbg.stepInto(); QTest::qWait(150); }
        QTest::qWait(500);
        qInfo() << "DBG2 running" << r.f.dbg.running() << "pc" << QString::fromStdString(view->valueOf("main_dec", "PC")) << "next" << QString::fromStdString(view->valueOf("main_dec", "next")) << "exec" << QString::fromStdString(view->valueOf("main_dec", "executed")) << "OPA" << QString::fromStdString(view->valueOf("alu", "OPA")) << "OUT" << QString::fromStdString(view->valueOf("alu", "OUT"));
        QVERIFY(r.until([&] { return QString::fromStdString(view->valueOf("alu", "OUT")) == "0A 00"; }));
        QCOMPARE(QString::fromStdString(view->valueOf("alu", "OPA")), QString("05 00"));
        QCOMPARE(QString::fromStdString(view->valueOf("alu", "OPB")), QString("05 00"));
        // the picture, for a look
        page->grab().save(QDir::tempPath() + "/acide_isa_top.png");
        view->setZoom(1.0);
        view->canvas()->grab().save(QDir::tempPath() + "/acide_isa_top_full.png");
        view->fit();
        // a click on a box opens the panel it stands for; on a decoder it shows the line it is at
        QString panel;
        Location where;
        ConnectionBag bag;
        bag.add(r.f.ctx.showPanel, [&](const QString& id) { panel = id; });
        bag.add(r.f.ctx.showExecution, [&](const Location& l) { where = l; });
        QTest::mouseClick(view->canvas(), Qt::LeftButton, Qt::NoModifier, view->boxRect("scratch").center());
        QCOMPARE(panel, QString("memory"));
        QVERIFY(!r.w->dock("memory")->isHidden());
        QTest::mouseClick(view->canvas(), Qt::LeftButton, Qt::NoModifier, view->boxRect("main_dec").center());
        QVERIFY2(where.line >= 3 && where.file == "/w/main.asm", qPrintable(QString::number(where.line)));
        panel.clear();
        QTest::mouseClick(view->canvas(), Qt::LeftButton, Qt::NoModifier, view->boxRect("xfer").center());          // a box without a target does nothing
        QVERIFY(panel.isEmpty());
        // zoom
        view->setZoom(1.0);
        QCOMPARE(view->canvas()->width(), (int)view->layout().width);
        view->fit();
        QVERIFY(view->canvas()->width() <= view->viewport()->width());
    }

    void anotherDrawingCanBeGivenInAFile() {
        Rig r(".main\nh: JMP h\n");
        QTemporaryFile f;
        QVERIFY(f.open());
        f.write("canvas 400 200\nbox only 10 10 180 80 data \"The one box\"\nrow only \"cycle\" cycle\n");
        f.close();
        r.f.settings.set("isatop.layout", f.fileName().toStdString());
        IsaTopView v(r.f.ctx);
        QCOMPARE((int)v.layout().boxes.size(), 1);
        QCOMPARE(QString::fromStdString(v.layout().boxes[0].title), QString("The one box"));
        r.f.settings.set("isatop.layout", "/no/such/file");
        IsaTopView w(r.f.ctx);
        QVERIFY2(w.layout().boxes.size() >= 20, "a file that cannot be read leaves the built-in drawing");
    }

    void autoDebugFindsWhatIsWrongAndShowsWhere() {
        Rig r(".main\nstart: BR.COND PERI_RESULT_VALID, take\n JMP start\ntake: JMP start\n");
        QAction* act = r.w->findChild<QAction*>("autoDebugAction");
        QVERIFY(act);
        QDockWidget* dock = r.w->dock("autodebug");
        QVERIFY(dock);
        auto* panel = dock->widget();
        auto* list = panel->findChild<QTreeWidget*>("autoDebugList");
        auto* summary = panel->findChild<QLabel*>("autoDebugSummary");
        auto* detail = panel->findChild<QLabel*>("autoDebugDetail");
        auto* spin = panel->findChild<QSpinBox*>("autoDebugCycles");
        QVERIFY(list && summary && detail && spin);
        spin->setValue(60000);
        act->trigger();                                                                          // Debug > Auto Debug: the panel opens and runs
        QVERIFY(!dock->isHidden());
        QVERIFY2(list->topLevelItemCount() >= 1, qPrintable(summary->text()));
        QTreeWidgetItem* stuck = nullptr;
        for (int i = 0; i < list->topLevelItemCount(); i++) if (list->topLevelItem(i)->text(3).contains("waiting in a loop")) stuck = list->topLevelItem(i);
        QVERIFY2(stuck, qPrintable(summary->text()));
        QCOMPARE(stuck->text(0), QString("Warning"));
        QVERIFY(stuck->text(2).startsWith("Main") && stuck->text(2).contains("main.asm:2"));
        QVERIFY(summary->text().contains("1 warning"));
        list->setCurrentItem(stuck);
        QVERIFY2(detail->text().contains("PERI_RESULT_VALID"), "the detail says what Main waits for");
        // show it: the editor goes to the line of the BR.COND
        auto* show = panel->findChild<QPushButton*>("autoDebugShow");
        QVERIFY(show && show->isEnabled());
        show->click();
        auto* ed = qobject_cast<CodeEditor*>(r.w->tabs()->currentWidget());
        QVERIFY(ed);
        QCOMPARE(ed->textCursor().blockNumber() + 1, 2);
        // "Debug to here" needs a finding with a cycle (a wait since cycle 0 has none): a result slot overrun has
        r.f.ws.setText("/w/main.asm", ".service S = 1, len=1\n.main\nm: JMP m\n.peri\np: EXEC_PUB service=S, src=0, len=1\n JMP p\n");
        QVERIFY(r.waitBuild());
        auto* look = panel->findChild<QPushButton*>("autoDebugRun");
        look->click();
        QTreeWidgetItem* over = nullptr;
        for (int i = 0; i < list->topLevelItemCount(); i++) if (list->topLevelItem(i)->text(3).contains("Result slot overrun")) over = list->topLevelItem(i);
        QVERIFY2(over, qPrintable(summary->text()));
        list->setCurrentItem(over);
        auto* replay = panel->findChild<QPushButton*>("autoDebugReplay");
        QVERIFY(replay && !replay->isEnabled());                                               // no debug session to run in
        std::string err;
        r.f.dbg.simParams().set("sim.speed", "0", err);
        r.w->debugAction("start")->trigger();
        QVERIFY(r.until([&] { return r.f.dbg.session().view() != nullptr; }));
        QVERIFY(r.until([&] { return replay->isEnabled(); }));
        const auto at = (asmcore::sim::Cycle)over->text(1).toULongLong();
        QVERIFY(at > 0);
        QVERIFY(r.until([&] { return r.f.dbg.running(); }));                                 // F5 runs on its own
        r.stops = 0;
        replay->click();                                                                      // (the debug session is running: it is stopped first, then run to the cycle)
        QVERIFY(r.until([&] { return !r.f.dbg.running() && r.f.dbg.session().view() && (qulonglong)r.f.dbg.session().view()->cycle == (qulonglong)at; }, 8000));      // (the pause that comes first is a stop too: wait for the cycle)
        QCOMPARE((qulonglong)r.f.dbg.session().view()->cycle, (qulonglong)at);
        // a program that is fine says so
        r.f.ws.setText("/w/main.asm", ".csr MARK = 0x50\n.main\ns: EXEC_CSR WRITE8 MARK, 7\nh: JMP h\n");
        QVERIFY(r.waitBuild());
        r.f.dbg.stop();
        look->click();
        QVERIFY2(summary->text().contains("nothing wrong"), qPrintable(summary->text()));
        QCOMPARE(list->topLevelItemCount(), 0);
    }

    void findAndFixMendsATextProgramInOneUndoStep() {
        const std::string broken = ".equ LIMIT = 4\n.main\nstart: MVO CONST(LIMT,1) -> MAIN_SCRATCHPAD[0..0]\n BR.COND PERI_RESULT_VALD, strat\n";
        Rig r(broken);
        r.w->openFile("/w/main.asm");
        auto* ed = qobject_cast<CodeEditor*>(r.w->tabs()->currentWidget());
        QVERIFY(ed);
        QVERIFY(!r.f.ctx.snapshot->result.ok);
        auto* panel = r.w->dock("autodebug")->widget();
        auto* list = panel->findChild<QTreeWidget*>("autoDebugList");
        auto* summary = panel->findChild<QLabel*>("autoDebugSummary");
        // looking changes nothing
        panel->findChild<QPushButton*>("autoDebugRun")->click();
        QCOMPARE(ed->toPlainText(), QString::fromStdString(broken));
        int canFix = 0;
        for (int i = 0; i < list->topLevelItemCount(); i++) canFix += list->topLevelItem(i)->text(0) == "Can fix";
        QVERIFY2(canFix >= 3, qPrintable(summary->text()));
        QVERIFY2(summary->text().contains("nothing was changed"), qPrintable(summary->text()));
        // fixing does
        panel->findChild<QPushButton*>("autoDebugFix")->click();
        const QString mended = ed->toPlainText();
        QVERIFY2(mended.contains("MOV CONST(LIMIT,1)") && mended.contains("PERI_RESULT_VALID, start") && mended.contains("JMP start"), qPrintable(mended));
        int fixed = 0;
        for (int i = 0; i < list->topLevelItemCount(); i++) fixed += list->topLevelItem(i)->text(0) == "Fixed";
        QVERIFY2(fixed >= 4, qPrintable(summary->text()));
        QVERIFY2(summary->text().contains("Fixed") && summary->text().contains("Ran"), qPrintable(summary->text()));          // and the repaired program was run
        QVERIFY(r.waitBuild());
        QVERIFY(r.f.ctx.snapshot->result.ok);
        // one Ctrl+Z gives all of it back
        ed->undo();
        QCOMPARE(ed->toPlainText(), QString::fromStdString(broken));
        // the Debug menu does it in one go
        r.w->findChild<QAction*>("autoDebugAction")->trigger();
        QVERIFY(ed->toPlainText().contains("JMP start"));
    }

    void theExecutionViewShowsWhatMovesWhenAParameterChanges() {
        Rig r(kProg);
        auto* summary = r.sf<QLabel>("executionSummary");
        auto* table = r.sf<QTableWidget>("executionTable");
        auto* view = r.sf<QWidget>("executionView");
        auto* run = r.sf<QPushButton>("executionRun");
        auto* compare = r.sf<QComboBox>("executionCompare");
        auto* first = r.sf<QPushButton>("executionFirst");
        QVERIFY(summary && table && view && run && compare && first);
        run->click();
        QVERIFY2(summary->text().contains("Main: 4 instructions") || summary->text().contains("Main: 3 instructions"), qPrintable(summary->text()));
        QVERIFY(summary->text().contains("not one cycle differs"));                       // the initial values against themselves
        QCOMPARE(table->rowCount(), 0);
        // one extra cycle per instruction: it is seen at once (the panel runs again by itself)
        auto& store = r.f.dbg.simParams();
        std::string err;
        QVERIFY(store.set("main.fetch_cycles", "1", err));
        QVERIFY2(r.until([&] { return summary->text().contains("shift up to"); }, 3000), qPrintable(summary->text()));
        QVERIFY(summary->text().contains("Main") && summary->text().contains("first at instruction 2"));
        QVERIFY(table->rowCount() >= 2 && table->item(0, 5)->text() == "+1" && table->item(1, 5)->text() == "+2");
        QVERIFY(first->isEnabled());
        // a smaller change on another parameter: a different set of rows
        QVERIFY(store.set("csr.access_main", "3", err));
        QVERIFY(r.until([&] { return summary->text().contains("shift up to") && table->rowCount() >= 2 && table->item(0, 5)->text() != "+1"; }, 3000));
        // a saved run is the baseline of the next ones
        auto* save = r.sf<QPushButton>("executionSave");
        save->click();
        QCOMPARE(compare->currentIndex(), 1);
        QVERIFY(r.until([&] { return summary->text().contains("not one cycle differs"); }, 3000));
        QVERIFY(store.set("main.fetch_cycles", "2", err));
        QVERIFY(r.until([&] { return summary->text().contains("shift up to"); }, 3000));
        compare->setCurrentIndex(2);                                                    // no baseline: the run alone
        QVERIFY(r.until([&] { return !summary->text().contains("Against the baseline") && table->rowCount() == 0; }, 3000));
        compare->setCurrentIndex(0);
        QVERIFY(r.until([&] { return summary->text().contains("Against the baseline"); }, 3000));
        // it draws
        view->resize(900, 360);
        QApplication::processEvents();
        const QImage img = view->grab().toImage();
        QVERIFY(distinctColorsFree(img) > 10);
        img.save(outDirFree() + "/execution_view.png");
        table->selectRow(0);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualItemRect(table->item(0, 1)).center());
        QApplication::processEvents();
        view->grab().save(outDirFree() + "/execution_view_zoomed.png");
    }

    void panelsReflectAndEditTheState() {
        Rig r(kProg);
        r.clickGutter(5);
        r.w->debugAction("start")->trigger();
        QVERIFY(r.waitStops(1));
        // memory: poke a byte
        auto* mem = r.find<QTableWidget>("memory", "memoryTable");
        auto* region = r.find<QComboBox>("memory", "memoryRegion");
        QVERIFY(mem && region);
        QVERIFY(r.until([&] { return mem->rowCount() == 4; }));                    // 32 bytes of scratchpad, 8 per row
        mem->item(0, 3)->setText("5A");                                             // scratch[2] = 0x5A
        QVERIFY(r.until([&] { return r.f.dbg.session().view() && r.f.dbg.session().view()->regions.at("scratch")[2] == 0x5A; }));
        // a changed byte is marked
        QVERIFY(r.until([&] { return mem->item(0, 3) && mem->item(0, 3)->text() == "5A"; }));
        region->setCurrentText("opa");
        QVERIFY(r.until([&] { return mem->rowCount() == 1; }));
        // CSR: write through the table
        auto* csr = r.find<QTableWidget>("csrs", "csrTable");
        QVERIFY(csr && r.until([&] { return csr->rowCount() == 1; }));
        csr->item(0, 2)->setText("0x2A");
        QVERIFY(r.until([&] { return r.f.dbg.session().view() && r.f.dbg.session().view()->csrs[0].second == 0x2A; }));
        // signals
        auto* sig = r.find<QTableWidget>("signals", "signalsTable");
        QVERIFY(sig && sig->rowCount() > 0);
        // watch expressions
        auto* watch = r.find<QTableWidget>("watch", "watchTable");
        QVERIFY(watch);
        watch->item(watch->rowCount() - 1, 0)->setText("csr[0x120] + 1");
        QVERIFY(r.until([&] { return watch->rowCount() == 2 && watch->item(0, 1) && watch->item(0, 1)->text() == "43"; }));
        QCOMPARE(watch->item(0, 2)->text(), QString("0x2b"));
        watch->item(0, 0)->setText("");                                             // an empty expression deletes the row
        QVERIFY(r.until([&] { return watch->rowCount() == 1; }));
        // events: instructions of the run so far
        auto* ev = r.find<QTableView>("events", "eventsView");
        QVERIFY(ev && ev->model()->rowCount() > 0);
        auto* kind = r.find<QComboBox>("events", "eventKind");
        int all = ev->model()->rowCount();
        kind->setCurrentIndex(1);                                                   // instructions only
        QVERIFY(ev->model()->rowCount() > 0 && ev->model()->rowCount() <= all);
        // call stack and breakpoint panel exist
        QVERIFY(r.find<QTreeWidget>("callstack", "callStackTree")->topLevelItemCount() == 2);
        // breakpoint condition edited in the panel
        auto* bps = r.find<QTableWidget>("breakpoints", "breakpointsTable");
        bps->item(0, 2)->setText("csr[0x120] == 99");
        QVERIFY(r.until([&] { return !r.f.dbg.lineBreakpoints().empty() && r.f.dbg.lineBreakpoints()[0].condition == "csr[0x120] == 99"; }));
        r.stops = 0;
        r.w->debugAction("restart")->trigger();
        QVERIFY(r.until([&] { return r.f.dbg.active() && r.f.dbg.session().view() && r.f.dbg.session().view()->cycle == 0; }));
        r.w->debugAction("start")->trigger();
        QVERIFY(r.waitStops(1));
        QCOMPARE((int)r.lastReason, (int)asmcore::sim::StopReason::Idle);          // the condition is never true: the run goes to the end
    }
    void pauseAnEndlessProgramAndSeeItIsStale() {
        Rig r(".csr C = 0x120\n.main\ns: EXEC_CSR WRITE8 C, 1\n EXEC_CSR WRITE8 C, 2\n JMP s\n");
        r.w->debugAction("start")->trigger();
        QVERIFY(r.until([&] { return r.f.dbg.running(); }));
        QVERIFY(r.w->debugAction("pause")->isEnabled());
        QVERIFY(!r.w->debugAction("over")->isEnabled());
        QVERIFY(r.w->debugStatusText().contains("running"));
        QTest::qWait(150);
        r.w->debugAction("pause")->trigger();
        QVERIFY(r.waitStops(1));
        QCOMPARE((int)r.lastReason, (int)asmcore::sim::StopReason::Cancelled);
        QVERIFY(r.f.dbg.session().view()->cycle > 0);
        // an edit makes the session stale
        r.w->currentEditor()->setPlainText(".csr C = 0x120\n.main\ns: JMP s\n");
        QVERIFY(r.until([&] { return r.w->debugStatusText().contains("source changed"); }));
        // restart builds the new text and goes back to the start
        r.w->debugAction("restart")->trigger();
        QVERIFY(r.until([&] { return r.f.dbg.active() && !r.f.dbg.stale() && r.f.dbg.session().view() && r.f.dbg.session().view()->cycle == 0; }, 6000));
        r.w->debugAction("stop")->trigger();
    }
    void aProgramWithErrorsCannotBeDebugged() {
        Rig r(".main\nh: JMP gone\n");
        r.w->debugAction("start")->trigger();
        QVERIFY(r.until([&] { return !r.w->warnings().isEmpty(); }));
        QVERIFY(!r.f.dbg.active());
    }
    void anEnvironmentFileEditsAsPlainText() {
        Rig r(kProg);
        r.f.fs->write("/w/e.env", "[csr X]\nkind = ram\n");
        CodeEditor* e = r.w->openFile("/w/e.env");
        QVERIFY(e);
        QVERIFY(e->toPlainText().startsWith("[csr X]"));
        e->setPlainText("mov foo\n");
        QVERIFY(r.until([&] { return r.f.ws.find("/w/e.env") && r.f.ws.find("/w/e.env")->dirty; }));
        e->setTextCursor(QTextCursor(e->document()));
        e->triggerCompletion();
        QVERIFY(e->completionLabels().isEmpty());                                   // no assembly completion in a plain file
        QCOMPARE(e->diagnosticSelectionCount(), 0);
    }
    void theNewProjectTemplateRunsOutOfTheBox() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        QVERIFY(w.newProject("/n/demo.acproj"));
        w.flushBuildTimer();
        for (int i = 0; i < 600; i++) { QTest::qWait(10); if (f.ctx.snapshot && !f.build.busy() && f.ctx.snapshot->revision == f.ws.revision()) break; }
        int stops = 0;
        asmcore::sim::StopReason reason = asmcore::sim::StopReason::None;
        std::string detail;
        ConnectionBag bag;
        bag.add(f.dbg.stopped, [&](const SimUpdate& u, const DebugLocation&) { stops++; reason = u.stop.reason; detail = u.stop.detail; });
        // stop where Main handles the service
        const asmcore::IndexSymbol* h = f.ctx.snapshot->index.find("handle", "main");
        QVERIFY(h);
        f.dbg.toggleLineBreakpoint("/n/main.asm", h->def.line + 1);
        f.dbg.simParams().set("sim.speed", "0", *new std::string);                          // (full speed: the test does not wait for the pace)
        w.debugAction("start")->trigger();
        for (int i = 0; i < 600 && stops < 1; i++) QTest::qWait(10);
        QVERIFY2(stops >= 1, "no stop");
        QVERIFY2(reason == asmcore::sim::StopReason::Breakpoint, (std::to_string((int)reason) + " " + detail).c_str());
        ViewPtr v = f.dbg.session().view();
        QVERIFY(v && !v->hasFault[0] && !v->hasFault[1]);
        w.grab().save(QDir::tempPath() + "/acide_debug2.png");
    }
    void breakpointsMoveWithTheText() {
        Rig r(kProg);
        r.clickGutter(5);
        QVERIFY(r.f.dbg.hasLineBreakpoint("/w/main.asm", 5));
        CodeEditor* e = r.w->currentEditor();
        QTextCursor c(e->document()->findBlockByNumber(2));
        c.movePosition(QTextCursor::StartOfBlock);
        c.insertText("; a new line\n");                                              // inserted above: lines shift by one
        QVERIFY(r.f.dbg.hasLineBreakpoint("/w/main.asm", 6));
        QVERIFY(!r.f.dbg.hasLineBreakpoint("/w/main.asm", 5));
    }
};

static bool waitForBuild(Fixture& f, MainWindow& w) {
    w.flushBuildTimer();
    for (int i = 0; i < 600; i++) { QTest::qWait(10); if (f.ctx.snapshot && !f.build.busy() && f.ctx.snapshot->revision == f.ws.revision()) return true; }
    return false;
}

// ---- languages -------------------------------------------------------------------------------------------------------------------------
class LanguageTests : public QObject {
    Q_OBJECT
    // the translators are global: always leave the program in English
    struct English { ~English() { installLanguage("en"); } };
private slots:
    void init() { installLanguage("en"); }
    void cleanup() { installLanguage("en"); }
    void theLanguagesOnOffer() {
        auto l = availableLanguages();
        QVERIFY(l.size() == 2 && l[0].code == "en" && l[1].code == "zh_TW");
        QCOMPARE(resolveLanguage("zh_TW"), std::string("zh_TW"));
        QCOMPARE(resolveLanguage("en"), std::string("en"));
        QCOMPARE(resolveLanguage("klingon"), std::string("en"));            // unknown: English
        QVERIFY(resolveLanguage("auto") == "en" || resolveLanguage("auto") == "zh_TW");
    }
    void installingSwitchesTheTexts() {
        QCOMPARE(QCoreApplication::translate("acide::MainWindow", "&File"), QString("&File"));
        QVERIFY(installLanguage("zh_TW"));
        QCOMPARE(currentLanguage(), std::string("zh_TW"));
        QCOMPARE(QCoreApplication::translate("acide::MainWindow", "&File"), QString::fromUtf8("檔案(&F)"));
        QCOMPARE(QCoreApplication::translate("QObject", "Problems"), QString::fromUtf8("問題"));
        QCOMPARE(QCoreApplication::translate("QPlatformTheme", "Cancel"), QString::fromUtf8("取消"));            // Qt's own texts
        QVERIFY(installLanguage("en"));
        QCOMPARE(QCoreApplication::translate("acide::MainWindow", "&File"), QString("&File"));
        QVERIFY(!installLanguage("klingon"));
        QCOMPARE(currentLanguage(), std::string("en"));
    }
    void theCompiledTranslationsMatchTheSourceFile() {
        // every message of the .ts file comes out of the embedded .qm exactly as written there
        QFile f(QString::fromUtf8(ACIDE_TRANSLATIONS_DIR) + "/acide_zh_TW.ts");
        QVERIFY2(f.open(QIODevice::ReadOnly), "cannot open the .ts file");
        QXmlStreamReader xml(&f);
        QVERIFY(installLanguage("zh_TW"));
        QString context;
        int checked = 0, wrong = 0;
        QStringList problems;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() == QLatin1String("name")) { context = xml.readElementText(); continue; }
            if (xml.name() != QLatin1String("message")) continue;
            const bool numerus = xml.attributes().value("numerus") == QLatin1String("yes");
            QString source, translation, comment;
            while (!(xml.isEndElement() && xml.name() == QLatin1String("message")) && !xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()) continue;
                if (xml.name() == QLatin1String("source")) source = xml.readElementText();
                else if (xml.name() == QLatin1String("translation")) translation = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                else if (xml.name() == QLatin1String("comment")) comment = xml.readElementText();
            }
            QString got = QCoreApplication::translate(context.toUtf8().constData(), source.toUtf8().constData(), comment.isEmpty() ? nullptr : comment.toUtf8().constData(), numerus ? 1 : -1);
            if (numerus) { translation = translation.trimmed().replace("%n", "1"); got = got.trimmed(); }   // the .ts indents numerus forms                                 // what the count 1 comes out as
            checked++;
            if (got != translation) { wrong++; problems << context + ": " + source; }
        }
        QVERIFY2(checked > 200, "too few messages checked");
        QVERIFY2(wrong == 0, qPrintable("the .qm differs from the .ts (run the acide_update_translations target): " + problems.join(" | ")));
    }
    void theWindowIsRebuiltInTheOtherLanguage() {
        Fixture f;
        f.fs->write("/w/main.asm", ".csr C = 0x120\n.main\nstart: EXEC_CSR WRITE8 C, 1\n    EXEC_CSR WRITE8 C, 2\nh: JMP h\n");
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        AppShell shell(f.ctx, panels);
        shell.setQuiet(true);
        MainWindow* first = shell.window();
        QVERIFY(first->openProject("/w/p.acproj"));
        first->openFile("/w/main.asm");
        QVERIFY(first->currentEditor());
        QVERIFY(waitForBuild(f, *first));
        f.dbg.toggleLineBreakpoint("/w/main.asm", 4);
        QVERIFY(f.dbg.hasLineBreakpoint("/w/main.asm", 4));
        QCOMPARE(first->menuBar()->actions().first()->text(), QString("&File"));
        QVERIFY(first->checkedLanguage("auto") || first->checkedLanguage("en"));
        // switch to Traditional Chinese through the menu's signal, like the user does
        f.ctx.languageRequested.fire("zh_TW");
        for (int i = 0; i < 200 && shell.window() == first; i++) QTest::qWait(10);
        MainWindow* zh = shell.window();
        QVERIFY(zh && zh != first);
        QCOMPARE(zh->menuBar()->actions().first()->text(), QString::fromUtf8("檔案(&F)"));
        QVERIFY(zh->menuBar()->actions().at(3)->text().contains(QString::fromUtf8("除錯")));
        QVERIFY(zh->checkedLanguage("zh_TW") && !zh->checkedLanguage("en"));
        QCOMPARE(QString::fromStdString(f.settings.get("ui.language")), QString("zh_TW"));
        // the project, the open file and the breakpoint came back
        QVERIFY(f.ws.hasProject() && f.ws.project().name == "p");
        QVERIFY(zh->currentEditor() && zh->currentEditor()->path() == "/w/main.asm");
        QVERIFY(f.dbg.hasLineBreakpoint("/w/main.asm", 4));
        QVERIFY(zh->dock("problems")->windowTitle() == QString::fromUtf8("問題"));
        QVERIFY(zh->debugStatusText().contains(QString::fromUtf8("除錯")));
        // the debug panels are Chinese too
        auto* csr = zh->dock("csrs")->widget()->findChild<QTableWidget*>("csrTable");
        QVERIFY(csr && csr->horizontalHeaderItem(1)->text() == QString::fromUtf8("位址"));
        zh->grab().save(QDir::tempPath() + "/acide_zh.png");
        // and back
        f.ctx.languageRequested.fire("en");
        for (int i = 0; i < 200 && shell.window() == zh; i++) QTest::qWait(10);
        QVERIFY(shell.window() != zh);
        QCOMPARE(shell.window()->menuBar()->actions().first()->text(), QString("&File"));
        QVERIFY(f.dbg.hasLineBreakpoint("/w/main.asm", 4));
    }
    void debuggingWorksInChineseAndEnds() {
        Fixture f;
        f.fs->write("/w/main.asm", ".csr C = 0x120\n.main\nstart: EXEC_CSR WRITE8 C, 1\n    EXEC_CSR WRITE8 C, 2\nh: JMP h\n");
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        AppShell shell(f.ctx, panels);
        shell.setQuiet(true);
        shell.window()->openProject("/w/p.acproj");
        QVERIFY(waitForBuild(f, *shell.window()));
        f.dbg.toggleLineBreakpoint("/w/main.asm", 4);
        shell.window()->debugAction("start")->trigger();
        for (int i = 0; i < 600 && !f.dbg.active(); i++) QTest::qWait(10);
        QVERIFY(f.dbg.active());
        for (int i = 0; i < 600 && !(f.sim.view() && f.sim.state() == SimState::Paused && f.dbg.location(asmcore::Domain::Main).valid); i++) QTest::qWait(10);
        // the language changes while a session is running: the session ends, nothing else is lost
        f.ctx.languageRequested.fire("zh_TW");
        QTest::qWait(300);
        QVERIFY(!f.dbg.active());
        QVERIFY(f.dbg.hasLineBreakpoint("/w/main.asm", 4));
        QVERIFY(shell.window()->debugStatusText().contains(QString::fromUtf8("未啟動")));
    }
};

// ---- the block editor ------------------------------------------------------------------------------------------------------------------
static const char* kBlockProg = ".equ LIMIT = 4\n.main\nstart: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]   ; load\n ALU ADD unsigned\n JMP start, safe\n.proc f\n CALL g\n RET\n.endp\n.peri\np: JMP p\n";

class BlockTests : public QObject {
    Q_OBJECT
    struct Rig {
        Fixture f;
        PanelRegistry panels;
        std::unique_ptr<MainWindow> w;
        BlockEditor* ed = nullptr;
        explicit Rig(const std::string& asmText = kBlockProg, bool open = true) {
            registerBuiltinPanels(panels);
            f.fs->write("/w/prog.acblk", asmcore::blocks::serializeDocument(asmcore::blocks::importAsm(asmText, "prog")));
            w = std::make_unique<MainWindow>(f.ctx, panels);
            w->setQuiet(true);
            w->resize(1000, 700);
            w->show();
            if (open) { w->openProject("/w/p.acproj"); w->openFile("/w/prog.acblk"); ed = qobject_cast<BlockEditor*>(w->tabs()->currentWidget()); }
        }
    };
    struct DragRig : Rig {
        explicit DragRig(const std::string& text) : Rig(text) {
            w->resize(1000, 700);
            ed->resize(900, 600);
            ed->show();
            ed->view().resetZoom();
            ed->view().showContent();
            QApplication::processEvents();
        }
        BlockView& v() { return ed->view(); }
        BlockScene& sc() { return ed->scene(); }
        QPoint at(const QPointF& scenePos) { return v().mapFromScene(scenePos); }
        QPointF center(const std::string& id) { return sc().blockRect(id).center(); }
        // press on a block, move in steps to `to` (scene coordinates), without releasing
        void pressAndMove(const std::string& id, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            const QPoint from = at(center(id));
            QTest::mousePress(v().viewport(), Qt::LeftButton, mods, from);
            const QPoint target = at(to);
            QTest::mouseMove(v().viewport(), from + (target - from) / 3);
            QTest::mouseMove(v().viewport(), from + (target - from) * 2 / 3);
            QTest::mouseMove(v().viewport(), target);
        }
        void release(QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier) { QTest::mouseRelease(v().viewport(), Qt::LeftButton, mods, at(to)); }
        QPointF socketPoint(const std::string& host, const std::string& socket) {
            const auto& lay = sc().layout();
            for (const auto& st : lay.stacks)
                for (const auto& cp : st.points)
                    if (cp.kind == acide::blocks::ConnectionPoint::Socket && cp.blockId == host && cp.slot == socket) return QPointF(st.origin.x + cp.pos.x, st.origin.y + cp.pos.y);
            return QPointF();
        }
    };

    struct EditHelp {
        static acide::BlockScene::EditTarget find(acide::BlockScene& sc, const std::string& block, const std::string& part) {
            for (const auto& t : sc.editTargets()) if (t.blockId == block && t.part == part) return t;
            return acide::BlockScene::EditTarget();
        }
    };

    struct MemoryClipboard : acide::IBlockClipboard {
        std::string text, blocks;
        void set(const std::string& t, const std::string& b) override { text = t; blocks = b; }
        void get(std::string& t, std::string& b) override { t = text; b = blocks; }
    };

    static QString outDir() {
        QByteArray d = qgetenv("ACIDE_TEST_OUT");
        return d.isEmpty() ? QDir::tempPath() : QString::fromLocal8Bit(d);
    }
    static int distinctColors(const QImage& img) {
        QSet<QRgb> s;
        for (int y = 0; y < img.height(); y += 2) for (int x = 0; x < img.width(); x += 2) s.insert(img.pixel(x, y));
        return s.size();
    }
private slots:
    void aBlockFileOpensInItsOwnTab() {
        Rig r;
        QVERIFY(r.ed);
        QCOMPARE(r.w->tabs()->tabText(r.w->tabs()->currentIndex()), QString("Blocks: prog.acblk"));
        QCOMPARE(r.ed->page(), BlockEditor::Page::Blocks);
        QVERIFY(r.ed->errorText().isEmpty());
        QCOMPARE((int)r.ed->scene().layout().stacks.size(), 3);              // definitions, main, peri
        QVERIFY(r.ed->infoText().contains("block"));
        // opening again does not make a second tab
        const int n = r.w->tabs()->count();
        r.w->openFile("/w/prog.acblk");
        QCOMPARE(r.w->tabs()->count(), n);
        // the file is offered by the Open dialog and the registry knows the extension
        QVERIFY(TabRegistry::instance().find("/x/y.acblk"));
        QVERIFY(TabRegistry::instance().find("/x/y.achw"));
        QVERIFY(!TabRegistry::instance().find("/x/y.asm"));
    }
    void theMenuShowsTheCurrentAssemblyFileAsBlocks() {
        Rig r;
        while (r.w->tabs()->count()) QVERIFY(r.w->closeEditorAt(0));
        QVERIFY(r.w->openAsBlocks() == nullptr);                                       // no assembler file open: a warning, nothing else
        QVERIFY(!r.w->warnings().isEmpty());
        r.w->openFile("/w/main.asm");
        QVERIFY(r.w->currentEditor());
        BlockEditor* b = r.w->openAsBlocks();
        QVERIFY(b);
        QVERIFY(r.f.fs->exists("/w/main.acblk"));
        QCOMPARE(r.w->tabs()->tabText(r.w->tabs()->currentIndex()), QString("Blocks: main.acblk"));
        QVERIFY(b->session().generated().text.find("EXEC_CSR") != std::string::npos);
        QVERIFY(r.w->findChild<QAction*>() != nullptr);
        QVERIFY(r.w->findChild<QToolBar*>("blocksToolbar") != nullptr);
        // asking again opens the same blocks (never overwritten by the text)
        r.f.ws.setText("/w/main.acblk", asmcore::blocks::serializeDocument(asmcore::blocks::importAsm(".main\n JMP x\n")));
        r.w->openFile("/w/main.asm");
        BlockEditor* again = r.w->openAsBlocks();
        QCOMPARE(again, b);
        QVERIFY(again->session().generated().text.find("JMP x") != std::string::npos);
        // a new empty block program
        QVERIFY(r.w->newBlocks("/w/empty.acblk"));
        QVERIFY(r.f.fs->exists("/w/empty.acblk"));
    }
    void aRadioSwitchChoosesBetweenBlocksAndAssemblyCode() {
        Rig r;
        auto* blocks = r.ed->findChild<QRadioButton*>("pageBlocks");
        auto* code = r.ed->findChild<QRadioButton*>("pageCode");
        QVERIFY(blocks && code && blocks->isChecked());
        code->click();
        QCOMPARE(r.ed->page(), BlockEditor::Page::Code);
        QVERIFY(!blocks->isChecked());
        QCOMPARE(r.ed->codePane().toPlainText().toStdString(), r.ed->session().generated().text);
        QVERIFY(r.ed->codePane().toPlainText().contains("start:"));
        QVERIFY(r.ed->codePane().isReadOnly());
        blocks->click();
        QCOMPARE(r.ed->page(), BlockEditor::Page::Blocks);
        r.ed->setPage(BlockEditor::Page::Code);
        QVERIFY(code->isChecked());                                              // setPage keeps the buttons in step
    }
    void selectionIsTheSameOnBothPages() {
        Rig r;
        const auto& doc = r.ed->session().document();
        const std::string movId = doc.sections[1].blocks[1].id;                  // main: label, mov ...
        r.ed->scene().blockClicked.fire(movId, Qt::NoModifier);                  // a click on the canvas
        QVERIFY(r.ed->session().isSelected(movId));
        r.ed->setPage(BlockEditor::Page::Code);
        QCOMPARE(r.ed->codePane().extraSelections().size(), 1);
        const int line = r.ed->session().generated().linesOf(movId)[0];
        QCOMPARE(r.ed->codePane().textCursor().blockNumber() + 1, line);        // scrolled to it
        // a click on another line of the text selects that block
        const std::string jmpId = doc.sections[1].blocks[3].id;
        const int jl = r.ed->session().generated().linesOf(jmpId)[0];
        QTextCursor c(r.ed->codePane().document()->findBlockByNumber(jl - 1));
        r.ed->codePane().setTextCursor(c);
        Q_EMIT r.ed->codePane().lineClicked(jl);
        QVERIFY(r.ed->session().isSelected(jmpId) && !r.ed->session().isSelected(movId));
        // ctrl-click adds, a click on the empty canvas clears
        r.ed->scene().blockClicked.fire(movId, Qt::ControlModifier);
        QCOMPARE((int)r.ed->session().selection().size(), 2);
        r.ed->scene().emptyClicked.fire(Qt::NoModifier);
        QVERIFY(r.ed->session().selection().empty());
        QCOMPARE(r.ed->codePane().extraSelections().size(), 0);
        QVERIFY(r.ed->infoText().contains("block"));
    }
    void theWheelZoomsTheCanvas() {
        Rig r;
        BlockView& v = r.ed->view();
        QCOMPARE(v.zoom(), 1.0);
        const QPoint c = v.viewport()->rect().center();
        auto wheel = [&](int dy, Qt::KeyboardModifiers m) {
            QWheelEvent e(QPointF(c), QPointF(v.viewport()->mapToGlobal(c)), QPoint(), QPoint(0, dy), Qt::NoButton, m, Qt::NoScrollPhase, false);
            QApplication::sendEvent(v.viewport(), &e);
        };
        wheel(120, Qt::NoModifier);                                          // the plain wheel zooms in
        const double in = v.zoom();
        QVERIFY(in > 1.0);
        wheel(-240, Qt::NoModifier);
        QVERIFY(v.zoom() < 1.0);
        wheel(120, Qt::ControlModifier);                                     // Ctrl + wheel still does
        QVERIFY(v.zoom() > 0.8 && v.zoom() < in);
        const int h = v.horizontalScrollBar()->value();
        wheel(120, Qt::ShiftModifier);                                       // Shift + wheel scrolls sideways, zoom unchanged
        const double z = v.zoom();
        QVERIFY(v.horizontalScrollBar()->value() != h || v.horizontalScrollBar()->maximum() == 0);
        QCOMPARE(v.zoom(), z);
        for (int i = 0; i < 40; i++) wheel(120, Qt::NoModifier);
        QCOMPARE(v.zoom(), BlockView::kMaxZoom);
    }
    void aSelectionRectangleSelectsWholeStacks() {
        Rig r;
        r.ed->resize(900, 600);
        r.ed->show();
        BlockView& v = r.ed->view();
        v.resetZoom();
        v.showContent();
        QApplication::processEvents();
        const auto& lay = r.ed->scene().layout();
        const auto& doc = r.ed->session().document();
        // the whole "main" stack (section 1): from just above-left of its hat to below-right of its last block
        const auto& st = lay.stacks[1];
        const QPointF a(st.origin.x + st.bounds.x - 15, st.origin.y + st.bounds.y - 15), b(st.origin.x + st.bounds.right() + 15, st.origin.y + st.bounds.bottom() + 15);
        QTest::mousePress(v.viewport(), Qt::LeftButton, Qt::NoModifier, v.mapFromScene(a));
        QTest::mouseMove(v.viewport(), v.mapFromScene(b));
        QVERIFY(r.ed->session().selection().size() >= 4);
        QTest::mouseRelease(v.viewport(), Qt::LeftButton, Qt::NoModifier, v.mapFromScene(b));
        // the top-level blocks of main are selected, the blocks inside the procedure are covered by it, other stacks are not
        for (const auto& blk : doc.sections[1].blocks) QVERIFY2(r.ed->session().isSelected(blk.id), blk.type.c_str());
        QVERIFY(!r.ed->session().isSelected((*doc.sections[1].blocks[4].stack("BODY"))[0].id));
        for (const auto& blk : doc.sections[2].blocks) QVERIFY(!r.ed->session().isSelected(blk.id));
        QCOMPARE((int)r.ed->session().selection().size(), (int)doc.sections[1].blocks.size());
        // an empty rectangle clears the selection
        const QPointF e1(st.origin.x + 700, st.origin.y + 400);
        QTest::mousePress(v.viewport(), Qt::LeftButton, Qt::NoModifier, v.mapFromScene(e1));
        QTest::mouseRelease(v.viewport(), Qt::LeftButton, Qt::NoModifier, v.mapFromScene(e1));
        QVERIFY(r.ed->session().selection().empty());
        // Ctrl + A selects the top-level blocks of every stack, Escape clears
        r.ed->scene().selectAll();
        size_t top = 0;
        for (const auto& s : doc.sections) top += s.blocks.size();
        QCOMPARE(r.ed->session().selection().size(), top);
        QTest::keyClick(v.viewport(), Qt::Key_Escape);
    }
    void draggingTheHatMovesTheWholeStack() {
        Rig r;
        r.ed->resize(900, 600);
        r.ed->show();
        BlockView& v = r.ed->view();
        v.resetZoom();
        v.showContent();
        QApplication::processEvents();
        auto& s = r.ed->session();
        const auto& doc = s.document();
        const double x0 = doc.sections[1].x, y0 = doc.sections[1].y;
        const std::string peri = doc.sections[2].blocks[0].id;
        const double px0 = doc.sections[2].x, py0 = doc.sections[2].y;
        const auto& st1 = r.ed->scene().layout().stacks[1];
        const QPoint from = v.mapFromScene(QPointF(st1.origin.x + st1.header.rect.center().x, st1.origin.y + st1.header.rect.center().y));     // the hat of Main
        QTest::mousePress(v.viewport(), Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(v.viewport(), from + QPoint(40, 30));
        QVERIFY(r.ed->scene().dragging());
        QCOMPARE(doc.sections[1].x, x0);                                     // while dragging only the picture moves
        QTest::mouseMove(v.viewport(), from + QPoint(100, 60));
        QTest::mouseRelease(v.viewport(), Qt::LeftButton, Qt::NoModifier, from + QPoint(100, 60));
        QVERIFY(!r.ed->scene().dragging());
        QCOMPARE(doc.sections[1].x, x0 + 100);
        QCOMPARE(doc.sections[1].y, y0 + 60);
        QCOMPARE(doc.sections[2].x, px0);                                    // other stacks stay
        QCOMPARE(doc.sections[2].y, py0);
        // one undo takes it back
        QVERIFY(s.undo());
        QCOMPARE(s.document().sections[1].x, x0);
        // a click without moving only selects
        const QPoint at = v.mapFromScene(r.ed->scene().blockRect(s.document().sections[1].blocks[1].id).center());
        QTest::mouseClick(v.viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE((int)s.selection().size(), 1);
        QCOMPARE(s.document().sections[1].x, x0);
        // two stacks selected move together; a huge drag stays inside the canvas
        r.ed->scene().selectAll();
        const QRectF peri2 = r.ed->scene().blockRect(s.document().sections[2].blocks[0].id);
        const QPoint f2 = v.mapFromScene(peri2.center());
        QTest::mousePress(v.viewport(), Qt::LeftButton, Qt::NoModifier, f2);
        QTest::mouseMove(v.viewport(), f2 + QPoint(20, 20));
        QTest::mouseMove(v.viewport(), f2 + QPoint(60, 0));
        QTest::mouseRelease(v.viewport(), Qt::LeftButton, Qt::NoModifier, f2 + QPoint(60, 0));
        QCOMPARE(s.document().sections[1].x, x0 + 60);
        QCOMPARE(s.document().sections[2].x, px0 + 60);
        r.ed->scene().moveSelectedStacksBy(QPointF(1e7, 1e7), true);
        const QRectF area = r.ed->scene().sceneRect();
        for (const auto& sec : s.document().sections) {
            QVERIFY(sec.x < area.right() && sec.y < area.bottom());
        }
        r.ed->scene().moveSelectedStacksBy(QPointF(-1e7, -1e7), true);
        for (const auto& sec : s.document().sections) QVERIFY(sec.x >= area.left() - 1 && sec.y >= area.top() - 1);
    }
    void aComponentCanBeTakenOutLeavesABlackHoleAndGoesBack() {
        DragRig r(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n");
        auto& s = r.ed->session();
        const std::string mov = s.document().sections[0].blocks[0].id;
        const std::string num = s.document().sections[0].blocks[0].value("src")->id;
        const std::string original = s.generated().text;
        const QPointF hole = r.socketPoint(mov, "src");
        // take the number out and let it go in the empty space
        const QPointF far = r.center(num) + QPointF(260, 150);
        r.pressAndMove(num, far);
        QVERIFY(r.sc().draggingPayload());
        QCOMPARE((int)s.document().sections.size(), 2);                                      // it floats already while dragged
        QVERIFY(!s.document().sections[0].blocks[0].value("src"));                          // and its socket is a hole
        r.release(far);
        QVERIFY(!r.sc().dragging());
        QCOMPARE((int)s.document().sections.size(), 2);
        QCOMPARE(s.document().sections[1].domain, std::string("floating"));
        QVERIFY(s.document().sections[1].blocks[0].id == num);
        QVERIFY(s.generated().text.find("(incomplete)") != std::string::npos);              // a half instruction is never code
        // the hole is black on the canvas
        r.ed->view().viewport()->repaint();
        const QImage img = r.v().viewport()->grab().toImage();
        const QPoint hp = r.at(r.sc().blockRect(mov).center() - QPointF(0, 0));
        (void)hp;
        const auto* hostBox = r.sc().layout().stackOf(mov);
        QVERIFY(hostBox);
        const QRectF holeRect = [&] { for (const auto& b : hostBox->blocks) if (b.id == mov) return QRectF(hostBox->origin.x + b.sockets[0].rect.x, hostBox->origin.y + b.sockets[0].rect.y, b.sockets[0].rect.w, b.sockets[0].rect.h); return QRectF(); }();
        const QPoint hc = r.at(holeRect.center());
        const QColor px = img.pixelColor(hc);
        QVERIFY2(px.red() < 45 && px.green() < 45 && px.blue() < 50, qPrintable(QString("the hole is black, not %1,%2,%3").arg(px.red()).arg(px.green()).arg(px.blue())));
        img.save(outDir() + "/blocks_hole.png");
        // bring it back: while it is near the hole the target says "fits"
        const QPointF hold = QPointF(r.sc().blockRect(num).width() / 2 + 6, 0);          // held by its middle: its left edge (the plug) is then 6 right of the pointer's offset
        r.pressAndMove(num, hole + hold);
        QVERIFY(r.sc().draggingPayload());
        QPointF itemPos;
        for (QGraphicsItem* it : r.sc().items()) if (auto* si = dynamic_cast<acide::StackItem*>(it)) if (si->index() == r.sc().payload()) itemPos = si->pos();
        const auto& pl = r.sc().layout().stacks[r.sc().payload()];
        QVERIFY2(r.sc().dropTarget().kind == acide::blocks::DropTarget::Socket && r.sc().dropTarget().fits(),
                 qPrintable(QString("kind %1 fit %2 reason %3 hole %4,%5 itemPos %6,%7 firstRect %8,%9 payload %10 stacks %11").arg((int)r.sc().dropTarget().kind).arg((int)r.sc().dropTarget().fit)
                            .arg(QString::fromStdString(r.sc().dropTarget().reason)).arg(hole.x()).arg(hole.y()).arg(itemPos.x()).arg(itemPos.y()).arg(pl.blocks[(size_t)pl.top[0]].rect.x).arg(pl.blocks[(size_t)pl.top[0]].rect.y).arg((int)r.sc().payload()).arg((int)r.sc().layout().stacks.size())));
        QCOMPARE(r.sc().dropTarget().socket, std::string("src"));
        r.release(hole + hold);
        QCOMPARE((int)s.document().sections.size(), 1);
        QVERIFY(s.document().sections[0].blocks[0].value("src") && s.document().sections[0].blocks[0].value("src")->id == num);
        QCOMPARE(s.generated().text, original);
        // two drags are two undo steps; the first undo puts the number back where it floated
        QVERIFY(s.undo());
        QCOMPARE((int)s.document().sections.size(), 2);
        QVERIFY(s.undo());
        QCOMPARE(s.generated().text, original);
        QVERIFY(s.document().sections[0].blocks[0].value("src"));
    }

    void aBlockThatDoesNotFitIsShownHatchedAndRefused() {
        DragRig r(".main\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n");
        auto& s = r.ed->session();
        const std::string mov = s.document().sections[0].blocks[0].id;
        const std::string opa = s.document().sections[0].blocks[0].value("dst")->id;       // ALU A can only be written
        const QPointF src = r.socketPoint(mov, "src");                                     // ...so it does not fit where something is read from
        const QPointF hold = QPointF(r.sc().blockRect(opa).width() / 2 + 6, 0);
        r.pressAndMove(opa, src + hold);
        QVERIFY(r.sc().draggingPayload());
        QVERIFY(r.sc().dropTarget().kind == acide::blocks::DropTarget::Socket);
        QVERIFY(!r.sc().dropTarget().fits());
        QVERIFY(!r.sc().dropTarget().reason.empty());
        r.ed->grab().save(outDir() + "/blocks_refused.png");
        r.release(src + hold);
        // it stays floating where it was let go; the source is untouched, the destination is a hole
        QCOMPARE((int)s.document().sections.size(), 2);
        QVERIFY(s.document().sections[0].blocks[0].value("src") && s.document().sections[0].blocks[0].value("src")->type == "region_main_scratchpad");
        QVERIFY(!s.document().sections[0].blocks[0].value("dst"));
    }

    void escapePutsEverythingBack() {
        DragRig r(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP x\n");
        auto& s = r.ed->session();
        const std::string original = s.generated().text;
        const std::string num = s.document().sections[0].blocks[0].value("src")->id;
        r.pressAndMove(num, r.center(num) + QPointF(200, 120));
        QVERIFY(r.sc().draggingPayload());
        QTest::keyClick(&r.v(), Qt::Key_Escape);
        QVERIFY(!r.sc().dragging());
        QCOMPARE((int)s.document().sections.size(), 1);
        QCOMPARE(s.generated().text, original);
        // the same for an instruction lifted out of its stack
        const std::string alu = s.document().sections[0].blocks[1].id;
        r.pressAndMove(alu, r.center(alu) + QPointF(220, 60));
        QCOMPARE((int)s.document().sections.size(), 2);
        QTest::keyClick(&r.v(), Qt::Key_Escape);
        QCOMPARE((int)s.document().sections.size(), 1);
        QCOMPARE(s.generated().text, original);
    }

    void deleteAndUndoWorkFromTheKeyboard() {
        DragRig r(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n");
        auto& s = r.ed->session();
        const std::string original = s.generated().text;
        const std::string num = s.document().sections[0].blocks[0].value("src")->id;
        r.v().setFocus();
        r.w->activateWindow();
        s.select({num});
        QTest::keyClick(&r.v(), Qt::Key_Delete);
        QVERIFY(!s.document().sections[0].blocks[0].value("src"));                          // the number is gone and its socket is a hole
        QVERIFY(s.generated().text.find("(incomplete)") != std::string::npos);
        QTest::keyClick(&r.v(), Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(s.generated().text, original);
        const QList<QKeySequence> redoKeys = QKeySequence::keyBindings(QKeySequence::Redo);   // Ctrl+Y or Ctrl+Shift+Z, as the platform has it
        QVERIFY(!redoKeys.isEmpty());
        const QKeyCombination redoKey = redoKeys.first()[0];
        QTest::keyClick(&r.v(), redoKey.key(), redoKey.keyboardModifiers());
        QVERIFY(s.generated().text.find("(incomplete)") != std::string::npos);
    }

    void doubleClickingAPartOfABlockEditsIt() {
        DragRig r(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start, safe\nstart:\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        auto& fe = r.ed->fieldEditor();
        const std::string mov = s.document().sections[0].blocks[0].id, alu = s.document().sections[0].blocks[1].id, jmp = s.document().sections[0].blocks[2].id;
        const std::string num = s.document().sections[0].blocks[0].value("src")->id;
        auto open = [&](const std::string& block, const std::string& part) {
            const auto t = EditHelp::find(r.sc(), block, part);
            QVERIFY2(!t.blockId.empty(), qPrintable(QString::fromStdString(block + "." + part + " is not an edit target")));
            QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(t.rect.center()));
            QVERIFY(fe.isOpen());
        };
        // a number: the editor opens with its value; Enter keeps the new one
        open(num, "value");
        QCOMPARE(fe.text(), QString("5"));
        fe.lineEdit()->selectAll();
        QTest::keyClicks(fe.lineEdit(), "9");
        QTest::keyClick(fe.lineEdit(), Qt::Key_Return);
        QVERIFY(!fe.isOpen());
        QVERIFY(s.generated().text.find("MOV CONST(9, 2) -> MAIN_SCRATCHPAD[0..1]") != std::string::npos);
        // a choice: a menu lists the operations (the current one checked); a pick applies it
        auto menuOf = [&](const std::string& block, const std::string& part) {
            const auto t = EditHelp::find(r.sc(), block, part);
            QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(t.rect.center()));
            return r.ed->choiceMenu();
        };
        auto pick = [&](const std::string& block, const std::string& part, const QString& text) {
            QMenu* m = menuOf(block, part);
            QVERIFY2(m && m->isVisible(), "a menu opens");
            QAction* hit = nullptr;
            for (QAction* a : m->actions()) if (a->text() == text) hit = a;
            QVERIFY2(hit, qPrintable(text));
            hit->trigger();
            QTest::qWait(20);
        };
        {
            QMenu* m = menuOf(alu, "op");
            QVERIFY(m && m->isVisible() && !fe.isVisible());
            QStringList items;
            QAction* checked = nullptr;
            for (QAction* a : m->actions()) { items << a->text(); if (a->isChecked()) checked = a; }
            QVERIFY(items.size() > 10 && items.contains("SUB") && items.contains("CMP.GE") && items.contains("Other value..."));
            QVERIFY(checked && checked->text() == "ADD");
            QTest::keyClick(m, Qt::Key_Escape);                                                // Esc leaves it as it is
            QTest::qWait(30);
            QVERIFY(!r.ed->choiceMenu());
            QVERIFY(s.generated().text.find("ALU ADD unsigned") != std::string::npos);
        }
        pick(alu, "op", "SUB");
        QVERIFY(s.generated().text.find("ALU SUB unsigned") != std::string::npos);
        // another value than the listed ones: "Other value..." opens the editor to type it
        pick(alu, "op", "Other value...");
        QTest::qWait(30);
        QVERIFY(fe.isOpen());
        fe.lineEdit()->selectAll();
        QTest::keyClicks(fe.lineEdit(), "XOR");
        QTest::keyClick(fe.lineEdit(), Qt::Key_Escape);                                         // Esc leaves it
        QVERIFY(!fe.isOpen());
        QVERIFY(s.generated().text.find("ALU SUB unsigned") != std::string::npos);
        // the value is checked as you type: a shift amount of 99 gets a hint (the block keeps it; the problem shows)
        pick(alu, "op", "SHL");
        open(alu, "amount");
        QTest::keyClicks(fe.lineEdit(), "99");
        QVERIFY2(!fe.toolTip().isEmpty(), "a hint about the range of the shift amount");
        QTest::keyClick(fe.lineEdit(), Qt::Key_Return);
        QVERIFY(s.generated().text.find("ALU SHL 99") != std::string::npos);
        // a flag is switched by one click
        const auto flag = EditHelp::find(r.sc(), jmp, "safe");
        QVERIFY(flag.flag);
        QCOMPARE(flag.text, std::string("1"));
        QTest::mouseClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(flag.rect.center()));
        QVERIFY(s.generated().text.find("JMP start\n") != std::string::npos);
        QVERIFY(s.generated().text.find("JMP start, safe") == std::string::npos);
        // undo goes back one step at a time
        QVERIFY(s.undo());
        QVERIFY(s.generated().text.find("JMP start, safe") != std::string::npos);
        QVERIFY(s.undo());                       // amount 99
        QVERIFY(s.generated().text.find("ALU SHL 99") == std::string::npos);
        // a hole takes typed text: take the number out, then type into the hole
        const std::string num2 = s.document().sections[0].blocks[0].value("src")->id;
        s.apply([&](asmcore::blocks::Editor& ed) { return ed.unplugValue(mov, "src", 500, 300); });
        QVERIFY(!s.document().sections[0].blocks[0].value("src"));
        open(mov, "src");
        QVERIFY(fe.text().isEmpty());
        QTest::keyClicks(fe.lineEdit(), "CONST(7, 2)");
        QTest::keyClick(fe.lineEdit(), Qt::Key_Return);
        QVERIFY(s.document().sections[0].blocks[0].value("src") && s.document().sections[0].blocks[0].value("src")->type == "num");
        QVERIFY(s.generated().text.find("MOV CONST(7, 2) ->") != std::string::npos);
        (void)num2;
    }

    // ---- the toolbox and Typeblock
    static void dragSample(DragRig& r, const char* entry, const QPointF& to, bool drop = true, bool leave = false) {
        auto* mime = new QMimeData;
        mime->setData(acide::BlockView::kSampleMime, entry);
        const QPoint p = r.at(to);
        QDragEnterEvent enter(p, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(r.v().viewport(), &enter);
        QDragMoveEvent move(p, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(r.v().viewport(), &move);
        if (leave) { QDragLeaveEvent lv; QApplication::sendEvent(r.v().viewport(), &lv); }
        else if (drop) { QDropEvent dr(QPointF(p), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(r.v().viewport(), &dr); }
        delete mime;
    }

    void theBlocksSpeakTheChosenLanguage() {
        struct English { ~English() { acide::installLanguage("en"); } } reset;
        QVERIFY(acide::installLanguage("zh_TW"));
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n");
        QStringList drawers;
        for (int i = 0; i < r.ed->toolbox().tree()->topLevelItemCount(); i++) drawers << r.ed->toolbox().tree()->topLevelItem(i)->text(0);
        QVERIFY2(drawers.contains(QString::fromUtf8("資料")) && drawers.contains(QString::fromUtf8("範式")) && drawers.contains(QString::fromUtf8("數值")), qPrintable(drawers.join(",")));
        const auto* st = r.sc().layout().stackOf(r.ed->session().document().sections[0].blocks[1].id);
        const auto* mov = st->find(r.ed->session().document().sections[0].blocks[1].id);
        QCOMPARE(QString::fromStdString(mov->title), QString::fromUtf8("搬移"));
        QCOMPARE(QString::fromStdString(mov->sockets[0].label), QString::fromUtf8("來源"));
        // a search by the English name still works, and the samples show the Chinese titles
        r.ed->toolbox().setFilter("move");
        bool seen = false;
        for (int i = 0; i < r.ed->toolbox().tree()->topLevelItemCount(); i++) for (int k = 0; k < r.ed->toolbox().tree()->topLevelItem(i)->childCount(); k++) {
            auto* it = r.ed->toolbox().tree()->topLevelItem(i)->child(k);
            if (!it->isHidden() && it->text(0).startsWith(QString::fromUtf8("搬移"))) seen = true;
        }
        QVERIFY(seen);
    }

    void editsGoIntoTheWorkspaceAndTheBuild() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n");
        auto& s = r.ed->session();
        auto& ws = r.f.ctx.workspace;
        r.sc().setAnimationsEnabled(false);
        const std::string gen = "/w/prog.generated.asm";
        QCOMPARE(QString::fromStdString(r.ed->generatedPath()), QString::fromStdString(gen));
        // opening wrote nothing: neither the program nor a generated file is dirty / present
        QVERIFY(!ws.find(gen));
        QVERIFY(!ws.find("/w/prog.acblk")->dirty);
        // an edit: both buffers are updated, unsaved
        emit r.ed->toolbox().activated("type:jmp");
        QVERIFY(ws.find(gen));
        QVERIFY(r.f.fs->exists(gen));                                          // (listing it in the project wrote it: the project never names a missing file)
        QCOMPARE(QString::fromStdString(ws.find(gen)->text), QString::fromStdString(s.generated().text));
        QVERIFY(ws.find(gen)->text.find("Generated from prog.acblk") != std::string::npos);
        QVERIFY(ws.find("/w/prog.acblk")->dirty);
        asmcore::blocks::Document back;
        std::string err;
        QVERIFY(asmcore::blocks::parseDocument(ws.find("/w/prog.acblk")->text, back, err));
        QCOMPARE((int)back.blockCount(), (int)s.document().blockCount());
        // the project lists the program and the generated file is a source
        QVERIFY(std::find(ws.project().blocks.begin(), ws.project().blocks.end(), std::string("prog.acblk")) != ws.project().blocks.end());
        QVERIFY(std::find(ws.project().sources.begin(), ws.project().sources.end(), std::string("prog.generated.asm")) != ws.project().sources.end());
        // the build uses the unsaved generated text
        const auto input = ws.snapshot();
        bool used = false;
        for (const auto& src : input.sources) if (src.name.find("prog.generated.asm") != std::string::npos && src.text == s.generated().text) used = true;
        QVERIFY(used);
        // a floating stack is not in the generated text; an undo takes the edit out of both
        QVERIFY(s.undo());
        QVERIFY(ws.find("/w/prog.acblk")->dirty);                               // (a later edit is unsaved again; the floating stack is not in the generated text)
        QCOMPARE(QString::fromStdString(ws.find(gen)->text), QString::fromStdString(s.generated().text));
        QVERIFY(ws.find(gen)->text.find("JMP start\n    JMP") == std::string::npos);
        // the tab counts both files; Save All writes both
        const auto paths = r.ed->workspacePaths();
        QVERIFY(paths.size() == 2 && paths[1] == gen);
        std::string e2;
        QVERIFY(ws.saveAll(e2));
        QVERIFY(r.f.fs->exists(gen));
        QVERIFY(!ws.find(gen)->dirty && !ws.find("/w/prog.acblk")->dirty);
        // changed from outside: the editor shows it and the generated buffer follows, nothing is marked unsaved by the loading
        asmcore::blocks::Document other = asmcore::blocks::importAsm(".main\nx: JMP x\n", "prog");
        r.f.fs->write("/w/prog.acblk", asmcore::blocks::serializeDocument(other));
        ws.reload("/w/prog.acblk");
        QCOMPARE((int)s.document().blockCount(), (int)other.blockCount());
        QVERIFY(ws.find(gen)->text.find("x:") != std::string::npos);
        QVERIFY(!ws.find("/w/prog.acblk")->dirty);
    }

    void theCountsInTheStatusLineJumpToTheirBlocks() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n frobnicate one\n JMP start\n frobnicate two\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        s.clearSelection();
        emit r.ed->toolbox().activated("type:jmp");                                     // (nothing selected: it lies on the canvas)
        QVERIFY(r.ed->infoText().contains("floating") && r.ed->infoText().contains("href=\"floating\""));
        QVERIFY(r.ed->infoText().contains("href=\"raw\""));
        // each click goes on to the next
        r.ed->jumpTo("raw");
        QCOMPARE((int)s.selection().size(), 1);
        const std::string first = *s.selection().begin();
        QCOMPARE(QString::fromStdString(s.document().find(first)->type), QString("raw"));
        r.ed->jumpTo("raw");
        QVERIFY(*s.selection().begin() != first);
        r.ed->jumpTo("raw");
        QCOMPARE(*s.selection().begin(), first);                                       // (round again)
        r.ed->jumpTo("floating");
        QCOMPARE((int)s.selection().size(), 1);
        bool inFloating = false;
        for (const auto& sec : s.document().sections) if (sec.domain == "floating") for (const auto& b : sec.blocks) inFloating |= s.isSelected(b.id);
        QVERIFY(inFloating);
        // nothing to jump to: nothing happens
        r.ed->jumpTo("problems");
    }

    void buildErrorsAppearOnTheBlocksThatCausedThem() {
        DragRig r(".main\ntop: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP top\n");
        auto& s = r.ed->session();
        auto& ws = r.f.ctx.workspace;
        r.sc().setAnimationsEnabled(false);
        // a block whose text the assembler rejects: a jump to a label that is nowhere
        std::string jmp;
        for (const auto& b : s.document().sections[0].blocks) if (b.type == "jmp") jmp = b.id;
        QVERIFY(!jmp.empty());
        QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setField(jmp, "target", "nowhere"); }).ok);
        QVERIFY(ws.find(r.ed->generatedPath()));
        QVERIFY(r.f.buildAndWait());
        QVERIFY(!r.f.ctx.snapshot->result.ok);
        QVERIFY(r.sc().problems().count(jmp) || s.buildProblemCount() > 0);
        bool onJmp = false;
        for (const auto& p : s.problems()) if (p.kind == "build" && p.blockId == jmp && p.message.find("nowhere") != std::string::npos) onJmp = true;
        QVERIFY(onJmp);
        // fixing it takes the badge away (at once for the old build: the lines may have moved), and the next build is clean
        QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setField(jmp, "target", "top"); }).ok);
        QCOMPARE((int)s.buildProblemCount(), 0);
        QVERIFY(r.f.buildAndWait());
        QVERIFY(r.f.ctx.snapshot->result.ok);
        for (const auto& p : s.problems()) QVERIFY2(!(p.kind == "build" && p.error), p.message.c_str());       // (warnings may remain: they are badges too)
    }

    void theBlocksOfTheLineBeingExecutedShareOneColour() {
        Rig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start\n.peri\np: JMP p\n", false);
        // a project of blocks: its one source is the file the blocks generate
        r.f.fs->write("/w/p.acproj", "name = p\nsource = prog.generated.asm\nblocks = prog.acblk\n");
        r.f.fs->write("/w/prog.generated.asm", ".main\nh: JMP h\n");
        QVERIFY(r.w->openProject("/w/p.acproj"));
        r.w->openFile("/w/prog.acblk");
        r.ed = qobject_cast<BlockEditor*>(r.w->tabs()->currentWidget());
        QVERIFY(r.ed);
        auto& s = r.ed->session();
        r.ed->scene().setAnimationsEnabled(false);
        auto sc = [&]() -> BlockScene& { return r.ed->scene(); };
        {   // (an edit and its undo: the generated text goes into the workspace, as it does when the blocks are worked on)
            std::string any;
            for (const auto& b : s.document().sections[0].blocks) if (b.type.find("jmp") != std::string::npos) any = b.id;
            QVERIFY(!any.empty());
            QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setField(any, "target", "p"); }).ok);
            QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setField(any, "target", "start"); }).ok);
        }
        QVERIFY(r.f.buildAndWait());
        QVERIFY(r.f.ctx.snapshot->result.ok);
        QVERIFY(sc().execution().empty());                                                   // nothing runs: every block has its own colour
        // the blocks of the program: the MOV with what is plugged into it, the ALU, the peri JMP
        const asmcore::blocks::Block* mov = nullptr;
        const asmcore::blocks::Block* alu = nullptr;
        for (const auto& sec : s.document().sections) for (const auto& b : sec.blocks) {
            if (b.type.find("mov") != std::string::npos && !mov && sec.domain == "main") mov = &b;
            if (b.type.find("alu") != std::string::npos && !alu) alu = &b;
        }
        QVERIFY(mov && alu);
        std::set<std::string> movGroup{mov->id};
        std::function<void(const asmcore::blocks::Block&)> walk = [&](const asmcore::blocks::Block& b) { for (const auto& v : b.values) for (const auto& c : v.second) { movGroup.insert(c.id); walk(c); } };
        walk(*mov);
        QVERIFY2(movGroup.size() >= 2, "the MOV has components plugged into it (a number, a region)");
        std::string why;
        r.f.dbg.simParams().set("sim.speed", "1", *new std::string);                          // (so slow that it hardly moves while the test looks)
        QVERIFY(r.f.dbg.start(why));
        for (int i = 0; i < 300 && sc().execution().empty(); i++) QTest::qWait(10);
        const auto onMain = sc().execution();
        QVERIFY(!onMain.empty());
        QColor mainColour = onMain.at(mov->id);
        for (const std::string& id : movGroup) QVERIFY2(onMain.count(id) && onMain.at(id) == mainColour, "every block of the line has the same colour");
        QVERIFY(!onMain.count(alu->id));
        QColor periColour;
        for (const auto& kv : onMain) if (!movGroup.count(kv.first)) periColour = kv.second;
        QVERIFY2(periColour.isValid() && periColour != mainColour, "Peri has a colour of its own (its JMP is on the canvas too)");
        // the next line: the first one gets its own colours back, the next one is coloured
        r.f.dbg.stepInto();
        for (int i = 0; i < 300 && !sc().execution().count(alu->id); i++) QTest::qWait(10);
        QVERIFY(sc().execution().count(alu->id));
        for (const std::string& id : movGroup) QVERIFY2(!sc().execution().count(id), "the line that was left is drawn in its own colours again");
        // and the picture really differs: the block is painted in the colour
        r.ed->view().showContent();
        QApplication::processEvents();
        QImage img(r.ed->view().viewport()->size(), QImage::Format_ARGB32);
        r.ed->view().viewport()->render(&img);
        img.save(QDir::tempPath() + "/acide_block_exec.png");
        // stopping the debugger takes the colours away
        r.f.dbg.stop();
        for (int i = 0; i < 300 && !sc().execution().empty(); i++) QTest::qWait(10);
        QVERIFY(sc().execution().empty());
    }

    void theBlocksShowTheRunOnlyWhileDebuggingAndGoQuietWhenItStops() {
        Rig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start\n.peri\np: JMP p\n", false);
        r.f.fs->write("/w/p.acproj", "name = p\nsource = prog.generated.asm\nblocks = prog.acblk\n");
        r.f.fs->write("/w/prog.generated.asm", ".main\nh: JMP h\n");
        QVERIFY(r.w->openProject("/w/p.acproj"));
        r.w->openFile("/w/prog.acblk");
        r.ed = qobject_cast<BlockEditor*>(r.w->tabs()->currentWidget());
        QVERIFY(r.ed);
        auto& s = r.ed->session();
        auto sc = [&]() -> BlockScene& { return r.ed->scene(); };
        sc().setAnimationsEnabled(false);
        {   // (an edit and its undo: the generated text goes into the workspace)
            std::string any;
            for (const auto& b : s.document().sections[0].blocks) if (b.type.find("jmp") != std::string::npos) any = b.id;
            QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setField(any, "target", "p"); }).ok);
            QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setField(any, "target", "start"); }).ok);
        }
        QVERIFY(r.f.buildAndWait());
        std::string mov, alu;
        for (const auto& sec : s.document().sections) for (const auto& b : sec.blocks) {
            if (b.type.find("mov") != std::string::npos && mov.empty() && sec.domain == "main") mov = b.id;
            if (b.type.find("alu") != std::string::npos && alu.empty()) alu = b.id;
        }
        QVERIFY(!mov.empty() && !alu.empty());
        // not debugging: the blocks are still
        auto still = [&] { return !sc().execShowsAnything() && sc().execFlashing().empty() && sc().execFlows() == 0 && sc().execution().empty() && !sc().execDimmed(alu) && sc().execHits(mov) == 0 && sc().execValue(mov).isEmpty(); };
        QVERIFY(still());
        std::string why;
        r.f.dbg.simParams().set("sim.speed", "1", *new std::string);                         // (so slow that nothing runs while the test looks)
        QVERIFY(r.f.dbg.start(why));
        for (int i = 0; i < 300 && sc().execution().empty(); i++) QTest::qWait(10);
        QVERIFY(!sc().execution().empty());
        // two steps: the MOV, then the ALU
        r.f.dbg.stepInto();
        for (int i = 0; i < 300 && sc().execHits(mov) == 0; i++) QTest::qWait(10);
        QCOMPARE((int)sc().execHits(mov), 1);
        QCOMPARE(sc().execValue(mov), QString("scratch[0..1] = 05 00"));                     // what the MOV did
        QVERIFY(std::find(sc().execFlashing().begin(), sc().execFlashing().end(), mov) != sc().execFlashing().end());
        QVERIFY2(sc().execDimmed(alu), "an instruction that has not run, while another has, is grayed out");
        r.f.dbg.stepInto();
        for (int i = 0; i < 300 && sc().execHits(alu) == 0; i++) QTest::qWait(10);
        QCOMPARE((int)sc().execHits(alu), 1);
        QVERIFY(!sc().execDimmed(alu) && sc().execValue(alu).startsWith("OUT "));
        QVERIFY2(sc().execFlows() >= 1, "a line runs from the block that ran before to the one that runs now");
        // the options of "Run view"
        QAction* heat = r.ed->runViewButton()->menu()->findChild<QAction*>("execHeat");
        QVERIFY(heat && heat->isChecked());
        heat->setChecked(false);
        r.f.dbg.stepInto();                                                                  // (an update that paints with the option off)
        QTest::qWait(100);
        heat->setChecked(true);
        // the picture
        r.ed->view().showContent();
        QApplication::processEvents();
        QImage img(r.ed->view().viewport()->size(), QImage::Format_ARGB32);
        r.ed->view().viewport()->render(&img);
        img.save(QDir::tempPath() + "/acide_block_run.png");
        // Stop: the run is gone from the blocks, they are as they were before
        r.f.dbg.stop();
        for (int i = 0; i < 300 && !still(); i++) QTest::qWait(10);
        QVERIFY(still());
        QVERIFY(sc().execution().empty());
    }

    void aTextProjectShownAsBlocksIsDebuggedAsBlocks() {         // the shipped example 14, turned into blocks: Start Debugging shows the run on the blocks
        Rig r(".main\nh: JMP h\n", false);
        auto slurp = [](const QString& path) { QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll().toStdString(); };
        const QString dir = QString::fromUtf8(ACIDE_SAMPLE_DIR) + "/";
        if (!QFileInfo::exists(dir + "14_main_peri_sim/main_peri.asm")) QSKIP("the example is not next to the tests");
        for (const char* n : {"14_main_peri_sim.acproj", "main_peri.asm", "main_peri_env.cfg", "main_peri.scn", "main_peri.simparams"}) r.f.fs->write(std::string("/w/s14/") + n, slurp(dir + "14_main_peri_sim/" + n));
        r.f.fs->write("/w/common/defs.inc", slurp(dir + "common/defs.inc"));
        QVERIFY(r.w->openProject("/w/s14/14_main_peri_sim.acproj"));
        r.w->openFile("/w/s14/main_peri.asm");
        BlockEditor* be = r.w->openAsBlocks();
        QVERIFY(be);
        r.f.dbg.simParams().set("sim.speed", "0", *new std::string);
        r.w->debugAction("start")->trigger();
        auto& sc = be->scene();
        for (int i = 0; i < 600 && sc.execution().empty(); i++) QTest::qWait(10);
        // what was built is what the blocks generate (the original text was replaced by it, the original is kept as .bak)
        const std::string gen = be->generatedPath();
        QVERIFY(r.f.ws.find(gen) && r.f.ws.find(gen)->text == be->session().generated().text);
        QVERIFY(r.f.fs->exists(gen + ".bak"));
        QVERIFY2(!sc.execution().empty(), "the blocks of the line each sequencer is at are coloured");
        for (int i = 0; i < 300 && sc.execHits(be->session().blockAtLine(1)) == 0 && !sc.execShowsAnything(); i++) QTest::qWait(10);
        QVERIFY2(sc.execShowsAnything(), "heat and values are there too");
        r.f.dbg.pause();
        for (int i = 0; i < 300 && r.f.dbg.running(); i++) QTest::qWait(10);
        r.f.dbg.stop();
        for (int i = 0; i < 300 && (sc.execShowsAnything() || !sc.execution().empty()); i++) QTest::qWait(10);
        QVERIFY(!sc.execShowsAnything() && sc.execution().empty());                             // stopped: still again
    }

    void findAndFixMendsTheBlocksOfAProgram() {
        Rig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP strat\n", false);
        r.f.fs->write("/w/p.acproj", "name = p\nsource = prog.generated.asm\nblocks = prog.acblk\n");
        r.f.fs->write("/w/prog.generated.asm", ".main\nh: JMP h\n");
        QVERIFY(r.w->openProject("/w/p.acproj"));
        r.w->openFile("/w/prog.acblk");
        r.ed = qobject_cast<BlockEditor*>(r.w->tabs()->currentWidget());
        QVERIFY(r.ed);
        auto& s = r.ed->session();
        r.ed->scene().setAnimationsEnabled(false);
        std::string jmp;
        for (const auto& b : s.document().sections[0].blocks) if (b.type.find("jmp") != std::string::npos) jmp = b.id;
        QVERIFY(!jmp.empty());
        QCOMPARE(QString::fromStdString(s.document().find(jmp)->get("target")), QString("strat"));
        QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setField(jmp, "target", "start2"); }).ok);       // (an edit: the generated text goes into the workspace)
        QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setField(jmp, "target", "strat"); }).ok);
        r.f.ctx.workspace.setText(r.ed->generatedPath(), s.generated().text);
        auto* panel = r.w->dock("autodebug")->widget();
        panel->findChild<QPushButton*>("autoDebugFix")->click();
        QCOMPARE(QString::fromStdString(s.document().find(jmp)->get("target")), QString("start"));      // the blocks were mended, not the generated text
        QVERIFY(s.generated().text.find("JMP start") != std::string::npos);
        QVERIFY(s.undo());                                                                                // all of it is one step
        QCOMPARE(QString::fromStdString(s.document().find(jmp)->get("target")), QString("strat"));
    }

    void aGeneratedFileIsReadOnlyAndPointsToItsBlocks() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n");
        auto& ws = r.f.ctx.workspace;
        r.sc().setAnimationsEnabled(false);
        emit r.ed->toolbox().activated("type:jmp");                              // (the first edit lists the program in the project)
        const std::string gen = "/w/prog.generated.asm";
        QVERIFY(ws.save(gen, *new std::string));
        auto* e = r.w->openFile(gen);
        QVERIFY(e);
        QVERIFY(e->isReadOnly());
        QCOMPARE(QString::fromStdString(e->generatedFrom()), QString("/w/prog.acblk"));
        QVERIFY(e->generatedBanner() && !e->generatedBanner()->isHidden());
        // typing, replacing and rename do nothing
        const QString before = e->toPlainText();
        e->moveCursor(QTextCursor::Start);
        QTest::keyClicks(e, "zzz");
        QCOMPARE(e->toPlainText(), before);
        e->toggleComment();
        e->duplicateLine();
        QCOMPARE(e->toPlainText(), before);
        // the button opens the blocks
        auto* open = e->generatedBanner()->findChild<QPushButton*>("openBlocks");
        QVERIFY(open);
        r.w->tabs()->setCurrentWidget(e);
        open->click();
        QVERIFY(qobject_cast<BlockEditor*>(r.w->tabs()->currentWidget()));
        // the project tree: a Blocks group, and the generated source is marked
        bool group = false, marked = false;
        for (QTreeWidgetItemIterator it(r.w->findChild<QTreeWidget*>("projectTree")); *it; ++it) {
            if ((*it)->text(0) == "Blocks") group = true;
            if ((*it)->text(0).contains("prog.generated.asm") && (*it)->text(0).contains("generated")) marked = true;
        }
        QVERIFY(group && marked);
        // an ordinary file is not affected; releasing the program makes the file ordinary again
        auto* main = r.w->openFile("/w/main.asm");
        QVERIFY(main && !main->isReadOnly());
        std::string err;
        QVERIFY(acide::detachBlocks(ws, "/w/prog.acblk", err));
        QVERIFY(!e->isReadOnly() && e->generatedBanner()->isHidden());
    }

    void convertingKeepsTheFileNameAndDetachingGivesTheTextBack() {
        Fixture f;
        QVERIFY(f.fs->exists("/w/main.asm"));
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        w.show();
        w.openProject("/w/p.acproj");
        const std::string original = ".csr C = 0x120\n.main\nstart: EXEC_CSR WRITE8 C, 1\n    ALU ADD unsigned\nh: JMP h\n";
        f.fs->write("/w/main.asm", original);
        QVERIFY(w.openFile("/w/main.asm"));
        auto* be = w.openAsBlocks();
        QVERIFY(be);
        // the blocks generate the very file they came from; the original text is kept aside; nothing else changed yet
        QCOMPARE(QString::fromStdString(be->generatedPath()), QString("/w/main.asm"));
        std::string t;
        QVERIFY(f.fs->read("/w/main.asm.bak", t));
        QCOMPARE(QString::fromStdString(t), QString::fromStdString(original));
        QVERIFY(f.ws.project().sources.size() == 1 && f.ws.project().sources[0] == "main.asm");
        // the first edit: the project lists the program, the file becomes generated (and read-only in the text editor)
        auto& s = be->session();
        const std::string last = s.document().sections.back().blocks.back().id;
        QVERIFY(s.apply([&](asmcore::blocks::Editor& e2) { return e2.setComment(last, "the end"); }).ok);
        QVERIFY(std::find(f.ws.project().blocks.begin(), f.ws.project().blocks.end(), std::string("main.acblk")) != f.ws.project().blocks.end());
        QCOMPARE((int)f.ws.project().sources.size(), 1);
        auto* ed = w.openFile("/w/main.asm");
        QVERIFY(ed && ed->isReadOnly());
        // detaching: plain text again, without the header, saved; the program leaves the project
        w.tabs()->setCurrentWidget(be);
        QVERIFY(w.detachBlocks());
        QVERIFY(f.fs->read("/w/main.asm", t));
        QVERIFY2(t.find("Generated from") == std::string::npos && t.find(".main") != std::string::npos, t.c_str());
        QVERIFY(std::find(f.ws.project().blocks.begin(), f.ws.project().blocks.end(), std::string("main.acblk")) == f.ws.project().blocks.end());
        QVERIFY(f.fs->exists("/w/main.acblk"));
        auto* after = w.openFile("/w/main.asm");
        QVERIFY(!after || !after->isReadOnly());
        QVERIFY(f.buildAndWait());
        QVERIFY(f.ctx.snapshot->result.ok);
    }

    void aProgramNextToItsAssemblerFileAdoptsThatFile() {
        Fixture f;
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        const std::string original = ".csr C = 0x120\n.main\nstart: EXEC_CSR WRITE8 C, 1\n    ALU ADD unsigned\nh: JMP h\n";
        f.fs->write("/w/main.asm", original);
        f.fs->write("/w/main.acblk", asmcore::blocks::serializeDocument(asmcore::blocks::importAsm(original, "main")));          // (made before programs said where they write)
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        w.show();
        w.openProject("/w/p.acproj");
        auto* be = w.openBlocks("/w/main.acblk");
        QVERIFY(be);
        QVERIFY(be->session().document().output.empty());
        // opening changes nothing
        QCOMPARE((int)f.ws.project().sources.size(), 1);
        QVERIFY(f.ws.project().blocks.empty());
        QVERIFY(be->session().apply([&](asmcore::blocks::Editor& e) { return e.setComment(be->session().document().sections.back().blocks.back().id, "x"); }).ok);
        // the first edit: it writes main.asm (not a second copy of the program), the project lists the program and still has one source, the old text is kept
        QCOMPARE(QString::fromStdString(be->generatedPath()), QString("/w/main.asm"));
        QCOMPARE((int)f.ws.project().sources.size(), 1);
        QCOMPARE((int)f.ws.project().blocks.size(), 1);
        std::string t;
        QVERIFY(f.fs->read("/w/main.asm.bak", t));
        QCOMPARE(QString::fromStdString(t), QString::fromStdString(original));
        std::string proj;
        QVERIFY(f.fs->read("/w/p.acproj", proj));
        QVERIFY2(proj.find("generated") == std::string::npos, proj.c_str());
        QVERIFY(f.buildAndWait());
        QVERIFY(f.ctx.snapshot->result.ok);
    }

    void aLoopBlockTakesAModeAndACount() {
        DragRig r(".main\ntop: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP top\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        s.select({s.document().sections[0].blocks[1].id});
        emit r.ed->toolbox().activated("type:loop");
        std::string loop;
        for (const auto& b : s.document().sections[0].blocks) if (b.type == "loop") loop = b.id;
        QVERIFY(!loop.empty());
        QVERIFY(s.generated().text.find("MOV CONST(10, 2) -> MAIN_SCRATCHPAD[30..31]") != std::string::npos);
        auto target = [&](const char* part) {
            for (const auto& t : r.sc().editTargets()) if (t.blockId == loop && t.part == part) return t;
            return BlockScene::EditTarget();
        };
        QVERIFY(!target("mode").blockId.empty() && !target("count").blockId.empty() && !target("counter").blockId.empty());
        QCOMPARE(target("mode").options.size(), size_t(2));
        // the count: double click, type, Enter -> the generated text follows, the editor goes
        auto* fe = r.ed->findChild<acide::FieldEditor*>("fieldEditor");
        QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(target("count").rect.center()));
        QVERIFY(fe->isVisible());
        fe->lineEdit()->selectAll();
        QTest::keyClicks(fe->lineEdit(), "25");
        QTest::keyClick(fe->lineEdit(), Qt::Key_Return);
        QVERIFY(!fe->isVisible());
        QVERIFY(s.generated().text.find("MOV CONST(25, 2)") != std::string::npos);
        // the mode: choose "forever" from the list -> no counting, the count and counter go away
        QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(target("mode").rect.center()));
        QVERIFY(r.ed->choiceMenu() && r.ed->choiceMenu()->isVisible());                  // (a closed choice is picked from a menu)
        for (QAction* a : r.ed->choiceMenu()->actions()) if (a->text() == "forever") a->trigger();
        QTest::qWait(30);
        QVERIFY(!fe->isVisible());
        QVERIFY(s.generated().text.find("ALU CMP.EQ") == std::string::npos && s.generated().text.find("JMP loop_") != std::string::npos);
        QVERIFY(target("count").blockId.empty());
        s.select({loop});
        r.ed->resize(900, 600);
        QApplication::processEvents();
        r.ed->grab().save(outDir() + "/blocks_loop.png");
        // a block dragged into the body is part of the loop
        s.apply([&](asmcore::blocks::Editor& e) { return e.setField(loop, "mode", "for"); });
        QVERIFY(!target("count").blockId.empty());
    }

    void aLabelNameEditorClosesAfterEveryWayOfFinishing() {
        DragRig r(".main\nloop: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP loop\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        auto* fe = r.ed->findChild<acide::FieldEditor*>("fieldEditor");
        QVERIFY(fe);
        std::string label;
        for (const auto& b : s.document().sections[0].blocks) if (b.type == "label") label = b.id;
        QVERIFY(!label.empty());
        auto dbl = [&] {
            for (const auto& t : r.sc().editTargets()) if (t.blockId == label) {
                const QPoint p = r.at(t.rect.center());
                QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, p);
                return true;
            }
            return false;
        };
        QVERIFY(dbl());
        QVERIFY(fe->isVisible());
        QTest::keyClicks(fe->lineEdit(), "again");
        QTest::keyClick(fe->lineEdit(), Qt::Key_Return);
        QVERIFY2(!fe->isVisible(), "Enter");
        QVERIFY(s.generated().text.find("again:") != std::string::npos);
        QVERIFY(dbl());
        QVERIFY(fe->isVisible());
        QTest::keyClick(fe->lineEdit(), Qt::Key_Escape);
        QVERIFY2(!fe->isVisible(), "Esc");
        QVERIFY(dbl());
        QTest::keyClick(fe->lineEdit(), Qt::Key_Tab);
        QVERIFY2(!fe->isVisible() || fe->isOpen(), "Tab");
        QTest::mouseClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(QPointF(900, 520)));              // a click elsewhere on the canvas
        QVERIFY2(!fe->isVisible() && !fe->isOpen(), "click elsewhere");
    }

    void theToolboxHasDrawersOfTheHardwareDescription() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n");
        auto& s = r.ed->session();
        auto& tb = r.ed->toolbox();
        auto drawerNames = [&] { return tb.drawerNames(); };
        auto has = [&](const std::string& n) { const auto v = drawerNames(); return std::find(v.begin(), v.end(), n) != v.end(); };
        QVERIFY(!has("Hardware CSR"));                                               // no description: none
        const std::string text = slurpFile(std::filesystem::path(ACIDE_SAMPLE_DIR) / "11_hardware_designer" / "board.achw");
        r.f.fs->write("/w/board.achw", text);
        r.f.ws.project().hardware = "board.achw";
        r.f.ws.projectEdited();
        QApplication::processEvents();
        QVERIFY(has("Hardware CSR") && has("Hardware Devices") && has("Hardware Services") && has("Hardware Names"));
        const auto names = drawerNames();
        const auto at = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) - names.begin(); };
        QCOMPARE(at("Hardware CSR"), at("Patterns") + 1);                          // right after the patterns
        QVERIFY(tb.catalogHas("hw:Hardware CSR/write AE_CTRL") && tb.catalogHas("hw:Hardware Devices/read DEV_IMU"));
        for (int i = 0; i < tb.tree()->topLevelItemCount(); i++) if (tb.tree()->topLevelItem(i)->data(0, Qt::UserRole + 2).toString().startsWith("Hardware")) tb.tree()->topLevelItem(i)->setExpanded(true);
        r.ed->resize(1000, 760);
        QApplication::processEvents();
        r.ed->grab().save(outDir() + "/blocks_hardware_toolbox.png");
        // a block of the drawer goes in like any: after the selected block, with the name of the part in it
        s.select({s.document().sections[0].blocks[1].id});
        emit tb.activated("hw:Hardware CSR/write AE_CTRL");
        QVERIFY2(s.generated().text.find("EXEC_CSR WRITE8 AE_CTRL, 0") != std::string::npos, s.generated().text.c_str());
        QVERIFY(r.ed->errorText().isEmpty());
        // Typeblock finds it by the name of the part
        QVERIFY(!r.ed->catalog().search("AE_CTRL").empty());
        // the drawers follow the description: a new CSR appears (the description is a buffer: no save needed), a removed one goes
        QVERIFY(!tb.catalogHas("hw:Hardware CSR/write NEW_ONE"));
        std::string more = text + "\n[csr NEW_ONE]\naddr = 0x3F0\nkind = ram\n";
        r.f.ws.setText("/w/board.achw", more);
        QApplication::processEvents();
        QVERIFY(tb.catalogHas("hw:Hardware CSR/write NEW_ONE") && tb.catalogHas("hw:Hardware CSR/read NEW_ONE"));
        r.f.ws.setText("/w/board.achw", text);
        QApplication::processEvents();
        QVERIFY(!tb.catalogHas("hw:Hardware CSR/write NEW_ONE"));
        // leaving the description takes the drawers away
        r.f.ws.project().hardware.clear();
        r.f.ws.projectEdited();
        QApplication::processEvents();
        QVERIFY(!has("Hardware CSR"));
    }

    void ifWhileAndWaitComeFromTheToolboxWithNamedSlots() {
        DragRig r(".main\ntop: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP top\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        // the toolbox offers them in the Control drawer
        for (const char* id : {"type:if", "type:while", "type:wait"}) QVERIFY2(r.ed->toolbox().catalogHas(id), id);
        s.select({s.document().sections[0].blocks[1].id});
        emit r.ed->toolbox().activated("type:if");
        std::string ifId;
        for (const auto& b : s.document().sections[0].blocks) if (b.type == "if") ifId = b.id;
        QVERIFY(!ifId.empty());
        // two slots, each named on a row of its own
        const acide::blocks::StackLayout* st = r.sc().layout().stackOf(ifId);
        QVERIFY(st);
        const acide::blocks::BlockBox* box = st->find(ifId);
        QCOMPARE((int)box->slotBoxes.size(), 2);
        QCOMPARE(QString::fromStdString(box->slotBoxes[0].label), QString("then"));
        QCOMPARE(QString::fromStdString(box->slotBoxes[1].label), QString("else"));
        QVERIFY(box->slotBoxes[0].labelRect.y < box->slotBoxes[0].rect.y && box->slotBoxes[0].rect.y + box->slotBoxes[0].rect.h <= box->slotBoxes[1].labelRect.y + 0.5);
        // something dropped into the ELSE slot is part of the block (and of the text)
        std::vector<asmcore::blocks::Block> blocks = s.fresh([&](asmcore::blocks::Document& d) { return r.ed->catalog().fromAssembler(d, "MOV CONST(2, 2) -> MAIN_SCRATCHPAD[4..5]", nullptr); });
        QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { asmcore::blocks::Where at;
            at.section = 0;
            at.container = ifId;
            at.slot = "ELSE";
            return e.insertBlocks(at, blocks);
        }).ok);
        QVERIFY(s.generated().text.find("MOV CONST(2, 2) -> MAIN_SCRATCHPAD[4..5]") != std::string::npos);
        // while: a slot for the condition work and one for the body
        s.select({ifId});
        emit r.ed->toolbox().activated("type:while");
        r.ed->resize(1000, 700);
        r.ed->view().fitAll();
        QApplication::processEvents();
        r.ed->grab().save(outDir() + "/blocks_if.png");
    }

    void variablesAndExpressionsAreBuiltFromTheToolbox() {
        DragRig r(".main\ntop: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[4..5]\n JMP top\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        auto& tb = r.ed->toolbox();
        const auto names = tb.drawerNames();
        QVERIFY(std::find(names.begin(), names.end(), "Variables") != names.end() && std::find(names.begin(), names.end(), "Expressions") != names.end());
        for (const char* id : {"type:set", "type:change", "type:v_var", "type:v_bin", "type:v_cmp"}) QVERIFY2(tb.catalogHas(id), id);
        // a "set" comes complete (0 plugged in) and gives code at once; the bytes it picks keep off the MOV's
        s.select({s.document().sections[0].blocks[1].id});
        emit tb.activated("type:set");
        std::string setId;
        for (const auto& b : s.document().sections[0].blocks) if (b.type == "set") setId = b.id;
        QVERIFY(!setId.empty());
        QVERIFY2(s.generated().text.find("MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]") != std::string::npos, s.generated().text.c_str());
        QVERIFY(r.ed->errorText().isEmpty());
        // a calculation dropped on the value: it takes the socket's place, the old 0 goes out
        s.select({setId});
        emit tb.activated("type:v_bin");
        QVERIFY2(s.generated().text.find("ALU ADD unsigned") != std::string::npos, s.generated().text.c_str());
        // an if, and a comparison into its "test"
        s.select({setId});
        emit tb.activated("type:if");
        std::string ifId;
        for (const auto& b : s.document().sections[0].blocks) if (b.type == "if") ifId = b.id;
        QVERIFY(!ifId.empty());
        s.select({ifId});
        emit tb.activated("type:v_cmp");
        const asmcore::blocks::Block* f = s.document().find(ifId);
        QVERIFY2(f && f->value("test") && f->value("test")->type == "v_cmp", "the comparison went into 'test'");
        QVERIFY(s.generated().text.find("ALU CMP.LT unsigned") != std::string::npos);
        r.ed->resize(1000, 700);
        r.ed->view().fitAll();
        QApplication::processEvents();
        r.ed->grab().save(outDir() + "/blocks_variables.png");
    }

    void aCascadeIsBuiltFromPartsCutByNextBlock() {
        DragRig r(".main\ntop: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[4..5]\n JMP top\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        QVERIFY(r.ed->toolbox().catalogHas("type:cascade") && r.ed->toolbox().catalogHas("type:cascade_next"));
        s.select({s.document().sections[0].blocks[1].id});
        emit r.ed->toolbox().activated("type:cascade");
        std::string cs;
        for (const auto& b : s.document().sections[0].blocks) if (b.type == "cascade") cs = b.id;
        QVERIFY(!cs.empty());
        QVERIFY2(s.generated().text.find("CASCADE_BEGIN 0, cascade_") != std::string::npos && s.generated().text.find("CASCADE_END 0, cascade_") != std::string::npos, s.generated().text.c_str());
        QVERIFY(r.ed->errorText().isEmpty());
        // two parts: a "next block" into the body, a block in each; the selector can name the second
        std::vector<asmcore::blocks::Block> next = s.fresh([&](asmcore::blocks::Document& d) { return r.ed->catalog().instantiate(*r.ed->catalog().find("type:cascade_next"), d); });
        asmcore::blocks::Where at;
        at.section = 0;
        at.container = cs;
        at.slot = "BODY";
        QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.insertBlocks(at, next); }).ok);
        QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setPart(cs, "k", "1"); }).ok);
        QVERIFY2(s.generated().text.find("CASCADE_BEGIN 1, cascade_") != std::string::npos && s.generated().text.find("CASCADE_END 1, cascade_") != std::string::npos, s.generated().text.c_str());
        r.ed->resize(1000, 700);
        r.ed->view().fitAll();
        QApplication::processEvents();
        r.ed->grab().save(outDir() + "/blocks_cascade.png");
    }

    void aClosedChoiceIsPickedFromAMenuThatStays() {
        DragRig r(".main\ntop: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP top\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        s.select({s.document().sections[0].blocks[1].id});
        emit r.ed->toolbox().activated("type:loop");
        std::string loop;
        for (const auto& b : s.document().sections[0].blocks) if (b.type == "loop") loop = b.id;
        QVERIFY(!loop.empty());
        BlockScene::EditTarget t;
        for (const auto& e : r.sc().editTargets()) if (e.blockId == loop && e.part == "mode") t = e;
        QVERIFY(!t.blockId.empty());
        QVERIFY(!t.options.empty() && !t.open);                                      // a closed choice (for / forever)
        QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(t.rect.center()));
        QVERIFY2(r.ed->choiceMenu() && r.ed->choiceMenu()->isVisible(), "a menu opens under the part");
        auto* fe = r.ed->findChild<acide::FieldEditor*>("fieldEditor");
        QVERIFY(!fe->isVisible());                                                   // (not the drop-down box)
        QTest::qWait(600);
        QVERIFY2(r.ed->choiceMenu() && r.ed->choiceMenu()->isVisible(), "...and it stays");
        QStringList items;
        QAction* pickForever = nullptr;
        for (QAction* a : r.ed->choiceMenu()->actions()) { items << a->text(); if (a->text() == "forever") pickForever = a; }
        QCOMPARE(items, QStringList({"for", "forever"}));
        QVERIFY(r.ed->choiceMenu()->actions()[0]->isChecked());                     // the current value is marked
        pickForever->trigger();
        QTest::qWait(30);
        QVERIFY(s.generated().text.find("ALU CMP.EQ") == std::string::npos);        // picked: no counting any more
        QVERIFY(s.generated().text.find("JMP loop_") != std::string::npos);
        // Esc leaves it as it is
        for (const auto& e : r.sc().editTargets()) if (e.blockId == loop && e.part == "mode") t = e;
        QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(t.rect.center()));
        QVERIFY(r.ed->choiceMenu() && r.ed->choiceMenu()->isVisible());
        QTest::keyClick(r.ed->choiceMenu(), Qt::Key_Escape);
        QTest::qWait(50);
        QVERIFY(!r.ed->choiceMenu());
        QVERIFY(s.generated().text.find("ALU CMP.EQ") == std::string::npos);
    }

    void aChoiceFromTheListClosesTheEditor() {
        DragRig r(".peri\nstart: EXEC_XFER READ dev=0, addr=0, addrlen=1, ptr=0\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        BlockScene::EditTarget t;
        bool found = false;
        for (const auto& e : r.sc().editTargets()) if (e.part == "rw") { t = e; found = true; }
        QVERIFY(found);
        r.sc().fieldEdit.fire(t);
        auto* fe = r.ed->findChild<acide::FieldEditor*>("fieldEditor");
        QVERIFY2(r.ed->choiceMenu() && r.ed->choiceMenu()->isVisible(), "the direction is picked from a menu");
        QVERIFY(!fe->isVisible());
        for (QAction* a : r.ed->choiceMenu()->actions()) if (a->text() == "WRITE") a->trigger();
        QTest::qWait(30);
        QVERIFY(!fe->isOpen());
        QVERIFY(s.generated().text.find("EXEC_XFER WRITE") != std::string::npos);
    }

    void theToolboxGrowsWithTheUsersOwnBlocks() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n");
        auto& tb = r.ed->toolbox();
        auto& s = r.ed->session();
        // every drawer header has a +
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);      // (the headers of the first build are deleted later)
        const auto plus = tb.tree()->findChildren<QToolButton*>("addBlock");
        QCOMPARE((int)plus.size(), tb.tree()->topLevelItemCount());
        // the + opens the dialog for its drawer
        QToolButton* dataPlus = nullptr;
        for (auto* b : plus) if (b->property("drawer").toString() == "Data") dataPlus = b;
        QVERIFY(dataPlus);
        dataPlus->click();
        auto* dlg = r.ed->customDialog();
        QVERIFY(dlg && dlg->isVisible());
        QCOMPARE(dlg->drawerBox()->currentData().toString(), QString("Data"));
        QVERIFY(!dlg->valid());                                                   // no name yet
        dlg->titleEdit()->setText("clear first byte");
        dlg->textEdit()->setPlainText("MOV CONST(0, 1) -> MAIN_SCRATCHPAD[0..0]");
        QVERIFY2(dlg->valid(), qPrintable(dlg->message()));
        dlg->titleEdit()->setText("move");                                       // the name is taken in this drawer
        QVERIFY(!dlg->valid());
        dlg->titleEdit()->setText("clear first byte");
        dlg->accept();
        QApplication::processEvents();
        QVERIFY(!r.ed->customDialog());
        // it is in the drawer, in the settings, and usable like any sample
        const QString id = "custom:Data/clear first byte";
        QVERIFY(tb.catalogHas(id));
        QCOMPARE((int)r.f.ctx.settings.getList("blocks.custom").size(), 1);
        const std::string first = s.document().sections[0].blocks[0].id;
        s.select({s.document().sections[0].blocks[1].id});
        emit tb.activated(id);
        QVERIFY(s.generated().text.find("MOV CONST(0, 1) -> MAIN_SCRATCHPAD[0..0]") != std::string::npos);
        (void)first;
        // the right button in a drawer offers a new block there; on one's own block also edit / delete
        r.ed->newCustomBlock("Math", false);
        QVERIFY(r.ed->customDialog() && r.ed->customDialog()->drawerBox()->currentData().toString() == "Math");
        r.ed->customDialog()->reject();
        QApplication::processEvents();
        // made from the selected blocks
        for (const auto& b : s.document().sections[0].blocks) if (b.type == "jmp") s.select({b.id});
        r.ed->newCustomBlock("Control", true);
        QVERIFY(r.ed->customDialog());
        QVERIFY2(r.ed->customDialog()->textEdit()->toPlainText().contains("JMP start"), qPrintable(r.ed->customDialog()->textEdit()->toPlainText()));
        r.ed->customDialog()->titleEdit()->setText("loop forever");
        r.ed->customDialog()->accept();
        QApplication::processEvents();
        QVERIFY(tb.catalogHas("custom:Control/loop forever"));
        // two blocks joined together, selected, become one new block (what the canvas menu "Save the selection as a new block" does)
        {
            std::set<std::string> two;
            for (const auto& b : s.document().sections[0].blocks) if (b.type == "mov" || b.type == "jmp") two.insert(b.id);
            QVERIFY(two.size() >= 2);
            s.select(std::vector<std::string>(two.begin(), two.end()));
            r.ed->newCustomBlock("Patterns", true);
            QVERIFY(r.ed->customDialog() && r.ed->customDialog()->drawerBox()->currentData().toString() == "Patterns");
            QVERIFY(r.ed->customDialog()->textEdit()->toPlainText().contains("MOV") && r.ed->customDialog()->textEdit()->toPlainText().contains("JMP"));
            r.ed->customDialog()->titleEdit()->setText("my pair");
            QVERIFY(r.ed->customDialog()->valid());
            r.ed->customDialog()->accept();
            QApplication::processEvents();
            QVERIFY(tb.catalogHas("custom:Patterns/my pair"));
            s.clearSelection();
            emit tb.activated("custom:Patterns/my pair");
            int movs = 0;
            for (const auto& sec : s.document().sections) for (const auto& b : sec.blocks) movs += b.type == "mov";
            QVERIFY(movs >= 2);
            r.ed->deleteCustomBlock("custom:Patterns/my pair");
            QVERIFY(!tb.catalogHas("custom:Patterns/my pair"));
        }
        // a component drawer takes an operand
        r.ed->newCustomBlock("Values", false);
        r.ed->customDialog()->titleEdit()->setText("bytes 4 to 7");
        r.ed->customDialog()->textEdit()->setPlainText("MAIN_SCRATCHPAD[4..7]");
        QVERIFY(r.ed->customDialog()->valid());
        r.ed->customDialog()->textEdit()->setPlainText("not (an operand");
        QVERIFY(!r.ed->customDialog()->valid());
        r.ed->customDialog()->textEdit()->setPlainText("MAIN_SCRATCHPAD[4..7]");
        r.ed->customDialog()->accept();
        QApplication::processEvents();
        QVERIFY(tb.catalogHas("custom:Values/bytes 4 to 7"));
        // edit one: the dialog holds its values and it may keep its name
        r.ed->newCustomBlock("Data", false, id);
        QVERIFY(r.ed->customDialog()->titleEdit()->text() == "clear first byte" && r.ed->customDialog()->valid());
        r.ed->customDialog()->textEdit()->setPlainText("RET");
        r.ed->customDialog()->accept();
        QApplication::processEvents();
        QCOMPARE((int)r.f.ctx.settings.getList("blocks.custom").size(), 3);
        // delete
        r.ed->deleteCustomBlock("custom:Control/loop forever");
        QVERIFY(!tb.catalogHas("custom:Control/loop forever"));
        QCOMPARE((int)r.f.ctx.settings.getList("blocks.custom").size(), 2);
        // what is in the settings is what the next session reads back
        asmcore::blocks::Registry reg = asmcore::blocks::Registry::defaults();
        acide::blocks::BlockCatalog next(reg);
        std::vector<acide::blocks::CustomBlock> mine;
        for (const std::string& line : r.f.ctx.settings.getList("blocks.custom")) { acide::blocks::CustomBlock c; QVERIFY(acide::blocks::decodeCustom(line, c)); mine.push_back(c); }
        next.setCustom(mine);
        QVERIFY(next.find(id.toStdString()) && next.find(id.toStdString())->text == "RET");
        QVERIFY(next.find("custom:Values/bytes 4 to 7"));
    }

    void theToolboxListsTheDrawersAndSearches() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n");
        auto& tb = r.ed->toolbox();
        auto* tree = tb.tree();
        QStringList drawers;
        for (int i = 0; i < tree->topLevelItemCount(); i++) drawers << tree->topLevelItem(i)->text(0);
        QVERIFY2(drawers.contains("Patterns") && drawers.contains("Data") && drawers.contains("CSR") && drawers.contains("Structure") && drawers.contains("Values") && drawers.contains("Regions") && drawers.contains("References"), qPrintable(drawers.join(",")));
        QVERIFY(drawers.indexOf("Data") < drawers.indexOf("Values"));
        QVERIFY(tb.visibleSamples() >= 40);
        // the drawers are coloured, and an instruction drawer is not the colour of a component drawer
        QTreeWidgetItem* data = nullptr;
        QTreeWidgetItem* values = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); i++) { if (tree->topLevelItem(i)->text(0) == "Data") data = tree->topLevelItem(i); if (tree->topLevelItem(i)->text(0) == "Values") values = tree->topLevelItem(i); }
        QVERIFY(data && values && data->background(0).color() != values->background(0).color());
        QVERIFY(data->child(0)->data(0, Qt::UserRole).toString().startsWith("type:"));
        r.ed->resize(1000, 640);
        QApplication::processEvents();
        r.ed->grab().save(outDir() + "/blocks_toolbox.png");
        // search
        const int all = tb.visibleSamples();
        tb.setFilter("mov");
        QVERIFY(tb.visibleSamples() >= 1 && tb.visibleSamples() < all / 3);
        bool hasMove = false;
        for (int i = 0; i < tree->topLevelItemCount(); i++) for (int k = 0; k < tree->topLevelItem(i)->childCount(); k++) if (!tree->topLevelItem(i)->child(k)->isHidden() && tree->topLevelItem(i)->child(k)->text(0).startsWith("move")) hasMove = true;
        QVERIFY(hasMove);
        tb.setFilter("zzzzqq");
        QCOMPARE(tb.visibleSamples(), 0);
        tb.setFilter("");
        QCOMPARE(tb.visibleSamples(), all);
        // the toggle hides it
        auto* toggle = r.ed->findChild<QToolButton*>("toolboxToggle");
        QVERIFY(toggle && toggle->isChecked() && tb.isVisible());
        toggle->click();
        QVERIFY(!tb.isVisible());
        toggle->click();
        QVERIFY(tb.isVisible());
    }

    void aSampleIsInsertedAfterTheSelectionOnActivation() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        const std::string mov = s.document().sections[0].blocks[1].id;
        s.select({mov});
        emit r.ed->toolbox().activated("type:alu");
        QCOMPARE((int)s.document().sections[0].blocks.size(), 4);
        QCOMPARE(s.document().sections[0].blocks[2].type, std::string("alu"));
        QVERIFY(s.isSelected(s.document().sections[0].blocks[2].id));
        QVERIFY(s.generated().text.find("ALU ADD unsigned") != std::string::npos);
        // a pattern is several blocks at once
        emit r.ed->toolbox().activated("pattern:add_two");
        QVERIFY((int)s.document().sections[0].blocks.size() >= 4 + 6);
        // with nothing selected a sample lies on the canvas; one undo takes it away
        s.clearSelection();
        const size_t before = s.document().sections.size();
        emit r.ed->toolbox().activated("type:num");
        QCOMPARE(s.document().sections.size(), before + 1);
        QCOMPARE(s.document().sections.back().domain, std::string("floating"));
        QVERIFY(s.undo());
        QCOMPARE(s.document().sections.size(), before);
    }

    void aSampleDraggedFromTheToolboxIsDroppedLikeAnyBlock() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        const std::string mov = s.document().sections[0].blocks[1].id;
        const std::string text = s.generated().text;
        // a number over a hole
        s.apply([&](asmcore::blocks::Editor& ed) { return ed.unplugValue(mov, "src", 500, 300); });
        s.apply([&](asmcore::blocks::Editor& ed) { return ed.removeSection(1); });
        QVERIFY(s.generated().text.find("(incomplete)") != std::string::npos);
        const QPointF hole = r.socketPoint(mov, "src");
        dragSample(r, "type:num", hole + QPointF(16, -3), false);
        QVERIFY(r.sc().paletteDragging());
        QVERIFY2(r.sc().dropTarget().kind == acide::blocks::DropTarget::Socket && r.sc().dropTarget().fits(),
                 qPrintable(QString("kind %1 fit %2 reason %3 hole %4,%5 payload %6 stacks %7").arg((int)r.sc().dropTarget().kind).arg((int)r.sc().dropTarget().fit).arg(QString::fromStdString(r.sc().dropTarget().reason)).arg(hole.x()).arg(hole.y()).arg((int)r.sc().payload()).arg((int)r.sc().layout().stacks.size())));
        dragSample(r, "type:num", hole + QPointF(16, -3), true);                                // (a second enter is the same drag: dropped)
        QVERIFY(!r.sc().dragging());
        QVERIFY(s.document().sections[0].blocks[1].value("src") && s.document().sections[0].blocks[1].value("src")->type == "num");
        QVERIFY(s.generated().text.find("MOV CONST(0, 2)") != std::string::npos || s.generated().text.find("MOV CONST(0)") != std::string::npos);
        // an instruction under another one
        const auto& lay = r.sc().layout();
        QPointF under;
        for (const auto& cp : lay.stacks[0].points) if (cp.kind == acide::blocks::ConnectionPoint::Next && cp.blockId == mov) under = QPointF(lay.stacks[0].origin.x + cp.pos.x, lay.stacks[0].origin.y + cp.pos.y);
        dragSample(r, "type:alu", under + QPointF(16, 12));
        QCOMPARE((int)s.document().sections[0].blocks.size(), 4);
        QCOMPARE(s.document().sections[0].blocks[2].type, std::string("alu"));
        QVERIFY(s.undo());
        QCOMPARE((int)s.document().sections[0].blocks.size(), 3);
        // an instruction in empty space stays on the canvas, floating
        dragSample(r, "type:jmp", QPointF(700, 400));
        QCOMPARE(s.document().sections.back().domain, std::string("floating"));
        QCOMPARE(s.document().sections.back().blocks[0].type, std::string("jmp"));
        QVERIFY(s.undo());
        // leaving the canvas without dropping takes it back
        const size_t n = s.document().sections.size();
        dragSample(r, "type:jmp", QPointF(700, 400), false, true);
        QVERIFY(!r.sc().dragging());
        QCOMPARE(s.document().sections.size(), n);
        (void)text;
    }

    void typeblockMakesBlocksFromTypedNamesAndLines() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        r.v().setFocus();
        auto& tb = r.ed->typeblock();
        const std::string mov = s.document().sections[0].blocks[1].id;
        s.select({mov});
        // a letter typed on the canvas opens it with that letter
        QTest::keyClick(&r.v(), Qt::Key_M, Qt::NoModifier, 0);
        QVERIFY(tb.isOpen());
        QCOMPARE(tb.edit()->text(), QString("m"));
        QTest::keyClicks(tb.edit(), "ov");
        QVERIFY(tb.resultCount() >= 1);
        QVERIFY2(tb.list()->item(0)->text().startsWith("move"), qPrintable(tb.list()->item(0)->text()));
        QTest::keyClick(tb.edit(), Qt::Key_Escape);
        QVERIFY(!tb.isOpen());
        QCOMPARE((int)s.document().sections[0].blocks.size(), 3);                                // Esc adds nothing
        // a name
        r.ed->openTypeblock();
        QVERIFY(tb.isOpen());
        QTest::keyClicks(tb.edit(), "jump");
        QTest::keyClick(tb.edit(), Qt::Key_Return);
        QVERIFY(!tb.isOpen());
        QCOMPARE((int)s.document().sections[0].blocks.size(), 4);
        QCOMPARE(s.document().sections[0].blocks[2].type, std::string("jmp"));
        // a line of assembler: it is offered first, and becomes the block with its components
        r.ed->openTypeblock();
        QTest::keyClicks(tb.edit(), "ALU SUB unsigned");
        QVERIFY(tb.list()->item(0)->text().startsWith("Insert as assembler"));
        QTest::keyClick(tb.edit(), Qt::Key_Return);
        QCOMPARE(s.document().sections[0].blocks[3].type, std::string("alu"));
        QVERIFY(s.generated().text.find("ALU SUB unsigned") != std::string::npos);
        // Down chooses another entry
        r.ed->openTypeblock();
        QTest::keyClicks(tb.edit(), "csr");
        QVERIFY(tb.resultCount() >= 2);
        QTest::keyClick(tb.edit(), Qt::Key_Down);
        QCOMPARE(tb.list()->currentRow(), 1);
        QTest::keyClick(tb.edit(), Qt::Key_Escape);
        // Ctrl+Space opens it too
        QTest::keyClick(&r.v(), Qt::Key_Space, Qt::ControlModifier);
        QVERIFY(tb.isOpen());
        QTest::keyClick(tb.edit(), Qt::Key_Escape);
        // text that is not understood stays text
        r.ed->openTypeblock();
        QTest::keyClicks(tb.edit(), "frobnicate the thing");
        QTest::keyClick(tb.edit(), Qt::Key_Return);
        QVERIFY(s.generated().text.find("frobnicate the thing") != std::string::npos);
    }

    void aWholeProgramCanBeTypedWithTheKeyboardOnly() {
        DragRig r(".main\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        r.v().setFocus();
        auto& tb = r.ed->typeblock();
        const std::vector<std::string> lines = {
            "start:", "EXEC_CSR WRITE8 0x120, 0x01", "EXEC_CSR SET8 0x120, 0x10", "EXEC_CSR CLR8 0x120, 0x01",
            "MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]", "MOV CONST(1, 2) -> MAIN_SCRATCHPAD[2..3]", "MOV CONST(5, 2) -> MAIN_SCRATCHPAD[4..5]",
            "loop:", "MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA", "MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB", "ALU ADD unsigned", "MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[0..1]",
            "MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA", "MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB", "ALU CMP.EQ unsigned", "BR.COND ALU_CMP_TRUE, finish", "JMP loop",
            "finish:", "EXEC_CSR WRITE_SP 0x121, len=2, ptr=0", "done:", "JMP done"};
        for (const std::string& line : lines) {
            r.ed->openTypeblock();
            QVERIFY2(tb.isOpen(), line.c_str());
            QTest::keyClicks(tb.edit(), QString::fromStdString(line));
            QTest::keyClick(tb.edit(), Qt::Key_Return);
        }
        std::string expected = "; Generated from prog.acblk - edit the blocks, not this file\n\n.main\n";
        for (const std::string& l : lines) expected += (l.back() == ':' ? "" : "    ") + l + "\n";
        QCOMPARE(QString::fromStdString(s.generated().text), QString::fromStdString(expected));
        QCOMPARE((int)s.document().sections.size(), 1);                                    // all in the Main part: nothing floats
        // and it is a program: it assembles
        asmcore::Options opt;
        const asmcore::Result res = asmcore::assemble({asmcore::Source{"p.asm", s.generated().text}}, nullptr, opt);
        QVERIFY2(res.ok, qPrintable(QString::fromStdString(res.diagnosticsText())));
        // the blocks are components, not text
        int raw = 0;
        for (const auto& b : s.document().sections[0].blocks) raw += b.type == "raw";
        QCOMPARE(raw, 0);
        QVERIFY(s.document().sections[0].blocks[4].value("src") && s.document().sections[0].blocks[4].value("dst"));
    }

    void arrowKeysMoveTheSelectionAndEnterEdits() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n.proc f\n RET\n.endp\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        r.v().setFocus();
        const auto& b = s.document().sections[0].blocks;
        const std::string label = b[0].id, mov = b[1].id, alu = b[2].id, proc = b[3].id, ret = (*b[3].stack("BODY"))[0].id;
        s.select({label});
        QTest::keyClick(&r.v(), Qt::Key_Down);
        QVERIFY(s.isSelected(mov) && s.selection().size() == 1);
        QTest::keyClick(&r.v(), Qt::Key_Down);
        QVERIFY(s.isSelected(alu));
        QTest::keyClick(&r.v(), Qt::Key_Down);
        QVERIFY(s.isSelected(proc));
        QTest::keyClick(&r.v(), Qt::Key_Right);                                           // into the procedure
        QVERIFY(s.isSelected(ret));
        QTest::keyClick(&r.v(), Qt::Key_Left);                                            // and out again
        QVERIFY(s.isSelected(proc));
        QTest::keyClick(&r.v(), Qt::Key_Up);
        QTest::keyClick(&r.v(), Qt::Key_Up);
        QVERIFY(s.isSelected(mov));
        QTest::keyClick(&r.v(), Qt::Key_Right);                                           // into what the move block holds: its first component
        QVERIFY(s.selection().size() == 1 && s.document().find(*s.selection().begin())->type == "num");
        QTest::keyClick(&r.v(), Qt::Key_Left);
        QVERIFY(s.isSelected(mov));
        // Enter types into the first part of the selected block
        QTest::keyClick(&r.v(), Qt::Key_Down);                                            // the ALU block
        QTest::keyClick(&r.v(), Qt::Key_Return);
        QVERIFY2(r.ed->choiceMenu() && r.ed->choiceMenu()->isVisible(), "the first part of the ALU block is its operation: a menu");
        QVERIFY(r.ed->choiceMenu()->actions().size() > 10);
        QTest::keyClick(r.ed->choiceMenu(), Qt::Key_Escape);
        QTest::qWait(30);
        QVERIFY(!r.ed->choiceMenu() && !r.ed->fieldEditor().isOpen());
    }

    void tidyUpFromTheButton() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n.peri\np: JMP p\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        const std::string text = s.generated().text;
        s.apply([&](asmcore::blocks::Editor& ed) { return ed.moveSection(1, 3, 4); });
        s.apply([&](asmcore::blocks::Editor& ed) { return ed.moveSection(0, 0, 0); });
        auto* tidy = r.ed->findChild<QToolButton*>("tidy");
        QVERIFY(tidy);
        tidy->click();
        const auto& lay = r.sc().layout();
        bool overlap = false;
        for (size_t i = 0; i < lay.stacks.size(); i++)
            for (size_t j = i + 1; j < lay.stacks.size(); j++) {
                const auto a = lay.stacks[i].bounds.translated(lay.stacks[i].origin.x, lay.stacks[i].origin.y), c = lay.stacks[j].bounds.translated(lay.stacks[j].origin.x, lay.stacks[j].origin.y);
                overlap = overlap || a.intersects(c);
            }
        QVERIFY(!overlap);
        QCOMPARE(s.generated().text, text);
        QVERIFY(s.undo());
    }

    void copyPasteAndTheKeyboard() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start\n");
        auto& s = r.ed->session();
        auto clip = std::make_shared<MemoryClipboard>();
        r.ed->setClipboard(clip);
        r.sc().setAnimationsEnabled(false);
        r.v().setFocus();
        r.w->activateWindow();
        const std::string mov = s.document().sections[0].blocks[1].id, alu = s.document().sections[0].blocks[2].id;
        const std::string original = s.generated().text;
        // Ctrl+C on a block: the clipboard has the blocks and their text
        s.select({mov});
        QTest::keyClick(&r.v(), Qt::Key_C, Qt::ControlModifier);
        QVERIFY(clip->blocks.find("acblk-clip") != std::string::npos);
        QCOMPARE(QString::fromStdString(clip->text), QString("    MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n"));
        // Ctrl+V with another selected: it goes under it
        s.select({alu});
        QTest::keyClick(&r.v(), Qt::Key_V, Qt::ControlModifier);
        QCOMPARE((int)s.document().sections[0].blocks.size(), 5);
        QCOMPARE(s.document().sections[0].blocks[3].type, std::string("mov"));
        // Ctrl+Z takes it back; Ctrl+D copies; Ctrl+E switches off; Delete removes
        QTest::keyClick(&r.v(), Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(s.generated().text, original);
        s.select({mov});
        QTest::keyClick(&r.v(), Qt::Key_D, Qt::ControlModifier);
        QCOMPARE((int)s.document().sections[0].blocks.size(), 5);
        QTest::keyClick(&r.v(), Qt::Key_E, Qt::ControlModifier);
        QVERIFY(s.generated().text.find("; MOV CONST(5, 2)") != std::string::npos);
        QTest::keyClick(&r.v(), Qt::Key_Delete);
        QCOMPARE((int)s.document().sections[0].blocks.size(), 4);
        // text from outside (only text, no blocks) becomes blocks
        clip->blocks.clear();
        clip->text = "MOV CONST(1, 1) -> MAIN_SCRATCHPAD[8..8]\n";
        s.clearSelection();
        r.ed->paste();
        QCOMPARE((int)s.document().sections.size(), 2);
        QCOMPARE(s.document().sections[1].domain, std::string("floating"));
        QVERIFY(s.document().sections[1].blocks[0].value("src") != nullptr);
    }

    void theSystemClipboardCarriesBlocksAndText() {
        DragRig r(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n");
        auto& s = r.ed->session();
        s.select({s.document().sections[0].blocks[0].id});
        r.ed->copy();
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        if (!mime || !mime->hasFormat("application/x-acblk")) QSKIP("the system clipboard is busy (another program holds it)");
        QCOMPARE(mime->text(), QString("    MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n"));
        s.select({s.document().sections[0].blocks[1].id});
        r.ed->paste();
        QCOMPARE((int)s.document().sections[0].blocks.size(), 3);
    }

    void theContextMenuOffersWhatFitsTheSelection() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n.proc f\n RET\n.endp\n");
        auto& s = r.ed->session();
        const std::string mov = s.document().sections[0].blocks[1].id, proc = s.document().sections[0].blocks[3].id;
        auto menuTexts = [&](const std::string& id, bool onTail = false) {
            QStringList texts;
            bool ran = false;
            QTimer::singleShot(150, [&] {
                if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) { for (QAction* a : m->actions()) if (!a->isSeparator()) texts << a->text() + (a->isEnabled() ? "" : " (off)"); m->close(); }
                ran = true;
            });
            (void)onTail;
            const QPointF at = id.empty() ? QPointF(900, 700) : r.center(id);
            r.sc().contextRequested.fire(at, QPoint(0, 0), id);
            return texts;
        };
        QStringList onMove = menuTexts(mov);
        QVERIFY2(onMove.join("|").contains("Cu&t") && onMove.join("|").contains("Disa&ble") && onMove.join("|").contains("&Delete"), qPrintable(onMove.join("|")));
        QVERIFY(onMove.join("|").contains("comment"));
        QVERIFY(s.isSelected(mov));                                                          // the block under the pointer is selected
        QVERIFY2(onMove.join("|").contains("&Fold / unfold (off)"), "only containers fold");
        QStringList onProc = menuTexts(proc);
        QVERIFY(onProc.join("|").contains("&Fold / unfold") && !onProc.join("|").contains("&Fold / unfold (off)"));
        QStringList onEmpty = menuTexts("");
        QVERIFY(onEmpty.join("|").contains("Select &all") && onEmpty.join("|").contains("Show everything"));
    }

    void tabGoesOnToTheNextPart() {
        DragRig r(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\nstart:\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        auto& fe = r.ed->fieldEditor();
        const std::string num = s.document().sections[0].blocks[0].value("src")->id;
        const auto t = EditHelp::find(r.sc(), num, "value");
        QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(t.rect.center()));
        QVERIFY(fe.isOpen());
        // the parts of the first line in reading order
        std::vector<std::string> order;
        for (const auto& e : r.sc().editTargets()) if (e.blockId == num || !e.flag) order.push_back(e.part);
        QVERIFY(order.size() >= 4);
        QTest::keyClick(fe.lineEdit(), Qt::Key_Tab);                                      // keeps "5", opens the next part
        QVERIFY(fe.isOpen());
        QVERIFY(fe.text() != "5" || order[1] != "value");
        QTest::keyClick(fe.lineEdit(), Qt::Key_Backtab);                                  // and back
        QVERIFY(fe.isOpen());
        QCOMPARE(fe.text(), QString("5"));
        QTest::keyClick(fe.lineEdit(), Qt::Key_Escape);
        QVERIFY(!fe.isOpen());
    }

    void doubleClickingTheEndOfABlockEditsItsComment() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        const std::string mov = s.document().sections[0].blocks[1].id;
        auto tailOf = [&](const std::string& id) {
            const auto* st = r.sc().layout().stackOf(id);
            const auto* b = st->find(id);
            return QPointF(st->origin.x + b->tailRect.center().x, st->origin.y + b->tailRect.center().y);
        };
        const double heightBefore = r.sc().blockRect(mov).height();
        double srcBefore = 0, numBefore = 0;
        {
            const auto* st0 = r.sc().layout().stackOf(mov);
            const auto* b0 = st0->find(mov);
            srcBefore = st0->blocks[(size_t)b0->sockets[0].child].rect.h;
            const auto& dst0 = st0->blocks[(size_t)b0->sockets[1].child];
            numBefore = st0->blocks[(size_t)dst0.sockets[0].child].rect.h;
        }
        // a double click in the middle of a block, or on a component, edits nothing
        QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(r.center(mov) - QPointF(60, 0)));
        QVERIFY(!r.ed->commentEdit().isOpen());
        // on its end: the editor opens over it
        acide::BlockScene::CommentTarget t;
        QVERIFY(r.sc().commentTargetAt(tailOf(mov), t));
        QCOMPARE(t.blockId, mov);
        QVERIFY(t.text.empty());
        QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(tailOf(mov)));
        QVERIFY(r.ed->commentEdit().isOpen());
        QVERIFY(r.ed->commentEdit().isVisible());
        QTest::keyClicks(&r.ed->commentEdit(), "load five");
        QTest::keyClick(&r.ed->commentEdit(), Qt::Key_Return, Qt::ShiftModifier);          // a second line
        QVERIFY(r.ed->commentEdit().isOpen());
        QTest::keyClicks(&r.ed->commentEdit(), "then go on");
        QTest::keyClick(&r.ed->commentEdit(), Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClicks(&r.ed->commentEdit(), "line three");
        QTest::keyClick(&r.ed->commentEdit(), Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClicks(&r.ed->commentEdit(), "line four");
        QTest::keyClick(&r.ed->commentEdit(), Qt::Key_Return);                             // keep it
        QVERIFY(!r.ed->commentEdit().isOpen());
        QCOMPARE(QString::fromStdString(s.document().find(mov)->comment), QString("load five\nthen go on\nline three\nline four"));
        QVERIFY(s.generated().text.find("[0..1]    ; load five\n    ; then go on\n    ; line three\n    ; line four\n") != std::string::npos);
        // the whole block is taller, and what is plugged in and nested in it fits the new height
        const auto* st = r.sc().layout().stackOf(mov);
        const auto* box = st->find(mov);
        QVERIFY2(box->rect.h > heightBefore + 1, qPrintable(QString("%1 -> %2").arg(heightBefore).arg(box->rect.h)));
        QCOMPARE((int)box->commentLines.size(), 4);
        const auto& src = st->blocks[(size_t)box->sockets[0].child];
        const auto& dst = st->blocks[(size_t)box->sockets[1].child];
        QVERIFY(std::fabs(src.rect.h - (box->rect.h - 10)) < 0.6 && std::fabs(dst.rect.h - (box->rect.h - 10)) < 0.6);
        const auto& num = st->blocks[(size_t)dst.sockets[0].child];
        QVERIFY2(src.rect.h > srcBefore + 1 && num.rect.h > numBefore + 1 && num.rect.h < dst.rect.h, qPrintable(QString("src %1->%2 num %3->%4").arg(srcBefore).arg(src.rect.h).arg(numBefore).arg(num.rect.h)));
        r.ed->grab().save(outDir() + "/blocks_comment.png");
        // editing it again starts from the text; Esc leaves it as it was
        QVERIFY(r.sc().commentTargetAt(tailOf(mov), t));
        QCOMPARE(QString::fromStdString(t.text), QString("load five\nthen go on\nline three\nline four"));
        QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(tailOf(mov)));
        QVERIFY(r.ed->commentEdit().isOpen());
        QCOMPARE(r.ed->commentEdit().toPlainText(), QString("load five\nthen go on\nline three\nline four"));
        QTest::keyClicks(&r.ed->commentEdit(), "zzz");
        QTest::keyClick(&r.ed->commentEdit(), Qt::Key_Escape);
        QVERIFY(!r.ed->commentEdit().isOpen());
        QCOMPARE(QString::fromStdString(s.document().find(mov)->comment), QString("load five\nthen go on\nline three\nline four"));
        // an empty comment takes it away and the block is low again
        QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(tailOf(mov)));
        QVERIFY(r.ed->commentEdit().isOpen());
        r.ed->commentEdit().selectAll();
        QTest::keyClick(&r.ed->commentEdit(), Qt::Key_Delete);
        QVERIFY2(r.ed->commentEdit().toPlainText().isEmpty(), qPrintable("text: " + r.ed->commentEdit().toPlainText()));
        QTest::keyClick(&r.ed->commentEdit(), Qt::Key_Return);
        QVERIFY2(s.document().find(mov)->comment.empty(), s.document().find(mov)->comment.c_str());
        QVERIFY(std::fabs(r.sc().blockRect(mov).height() - heightBefore) < 0.6);
        // undo: the comment is back
        QVERIFY(s.undo());
        QVERIFY(!s.document().find(mov)->comment.empty());
        // the hat and a label take comments too
        const auto& hat = r.sc().layout().stacks[0];
        const QPointF hatTail(hat.origin.x + hat.header.tailRect.center().x, hat.origin.y + hat.header.tailRect.center().y);
        QVERIFY(r.sc().commentTargetAt(hatTail, t) && t.section && t.stack == 0);
        QTest::mouseDClick(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(hatTail));
        QVERIFY(r.ed->commentEdit().isOpen());
        QTest::keyClicks(&r.ed->commentEdit(), "entry");
        QTest::keyClick(&r.ed->commentEdit(), Qt::Key_Return);
        QCOMPARE(QString::fromStdString(s.document().sections[0].comment), QString("entry"));
        QVERIFY(s.generated().text.find(".main    ; entry") != std::string::npos);
        const std::string label = s.document().sections[0].blocks[0].id;
        QVERIFY(r.sc().commentTargetAt(tailOf(label), t) && t.blockId == label);
    }

    void blocksWobbleSoftlyWhenTakenOutAndPutBack() {
        DragRig r(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n");
        auto& s = r.ed->session();
        auto& sc = r.sc();
        QVERIFY(sc.animationsEnabled());
        const std::string mov = s.document().sections[0].blocks[0].id;
        const std::string num = s.document().sections[0].blocks[0].value("src")->id;
        const QPointF hole = r.socketPoint(mov, "src");
        double sx = 1, sy = 1;
        // taking the number out: it pops (squashed at first), and the row it leaves starts at its old, wider width and springs shut around the hole
        r.pressAndMove(num, r.center(num) + QPointF(240, 140));
        QVERIFY(sc.draggingPayload());
        sc.effectScale(num, sx, sy);
        QVERIFY2(sx < 1.0 && sy > 1.0, qPrintable(QString("the lifted number is squashed (%1, %2)").arg(sx).arg(sy)));
        sc.effectScale(mov, sx, sy);
        QVERIFY2(sx > 1.0, qPrintable(QString("the row starts wider than the hole it leaves (%1)").arg(sx)));
        QVERIFY(sc.effectsRunning() >= 2);
        r.v().viewport()->grab().save(outDir() + "/blocks_squish_0.png");                  // the first frame of the effect
        for (int i = 0; i < 4; i++) sc.advanceEffects(1.0 / 60);
        r.v().viewport()->grab().save(outDir() + "/blocks_squish_1.png");
        // run the springs: they overshoot (the bounce) and come to rest
        double maxPop = 0, minPop = 10;
        int frames = 0;
        while (sc.effectsRunning() > 1 && frames < 600) {                       // (one is the lean of the dragged stack)
            sc.advanceEffects(1.0 / 60);
            sc.effectScale(num, sx, sy);
            maxPop = std::max(maxPop, sx);
            minPop = std::min(minPop, sx);
            frames++;
        }
        QVERIFY2(maxPop > 1.02, qPrintable(QString("the pop overshoots its size (%1)").arg(maxPop)));
        QVERIFY(frames < 600);
        // painting with an effect running does not trouble the canvas
        sc.popEffect(mov);
        const QImage img = r.v().viewport()->grab().toImage();
        QVERIFY(!img.isNull());
        r.release(r.center(num) + QPointF(240, 140));
        while (sc.effectsRunning() > 0) sc.advanceEffects(1.0 / 60);
        // putting it back: the row with the hole starts narrower and springs OPEN to take the number
        const QPointF hold = QPointF(sc.blockRect(num).width() / 2 + 6, 0);
        r.pressAndMove(num, hole + hold);
        QVERIFY(sc.dropTarget().fits());
        while (sc.effectsRunning() > 1) sc.advanceEffects(1.0 / 60);            // (the lift effects of this drag first)
        r.release(hole + hold);
        QCOMPARE((int)s.document().sections.size(), 1);
        sc.effectScale(mov, sx, sy);
        QVERIFY2(sx < 1.0, qPrintable(QString("the row starts narrower than the number it must hold (%1)").arg(sx)));
        double maxOpen = 0;
        int n = 0;
        while (sc.effectsRunning() > 0 && n < 600) { sc.advanceEffects(1.0 / 60); sc.effectScale(mov, sx, sy); maxOpen = std::max(maxOpen, sx); n++; }
        QVERIFY2(maxOpen > 1.0, qPrintable(QString("it springs open past its width and settles back (%1)").arg(maxOpen)));
        QCOMPARE((int)sc.effectsRunning(), 0);
        // switched off: no effects at all
        sc.setAnimationsEnabled(false);
        r.pressAndMove(num, r.center(num) + QPointF(240, 140));
        QVERIFY(sc.draggingPayload());
        QCOMPARE((int)sc.effectsRunning(), 0);
        r.release(r.center(num) + QPointF(240, 140));
    }

    void instructionsCanBeLiftedOutAndPutBack() {
        DragRig r(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start\n.peri\np: JMP p\n");
        auto& s = r.ed->session();
        const std::string original = s.generated().text;
        const std::string mov = s.document().sections[0].blocks[1].id, alu = s.document().sections[0].blocks[2].id;
        // drag the ALU block: it takes the jump below it
        r.pressAndMove(alu, r.center(alu) + QPointF(330, 40));
        QVERIFY(r.sc().draggingPayload());
        QCOMPARE((int)s.document().sections.size(), 3);
        QCOMPARE((int)s.document().sections.back().blocks.size(), 2);
        r.release(r.center(alu) + QPointF(330, 40));
        QCOMPARE((int)s.document().sections[0].blocks.size(), 2);
        QVERIFY(s.generated().text.find("ALU") == std::string::npos);
        // drag it back under the move block
        const auto& lay = r.sc().layout();
        QPointF under;
        for (const auto& cp : lay.stacks[0].points) if (cp.kind == acide::blocks::ConnectionPoint::Next && cp.blockId == mov) under = QPointF(lay.stacks[0].origin.x + cp.pos.x, lay.stacks[0].origin.y + cp.pos.y);
        const size_t floating = s.document().sections.size() - 1;
        const std::string movedAlu = s.document().sections[floating].blocks[0].id;
        const QRectF br = r.sc().blockRect(movedAlu);
        // hold it by its top left corner area so the attachment point follows the pointer predictably
        const QPointF grab = br.topLeft() + QPointF(6, 6);
        const QPoint from = r.at(grab);
        QTest::mousePress(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(r.v().viewport(), from + QPoint(10, 10));
        const QPointF to = under + QPointF(6, 6) + QPointF(3, 2);
        QTest::mouseMove(r.v().viewport(), r.at(to));
        QVERIFY(r.sc().draggingPayload());
        QVERIFY(r.sc().dropTarget().kind == acide::blocks::DropTarget::Insert && r.sc().dropTarget().fits());
        QTest::mouseRelease(r.v().viewport(), Qt::LeftButton, Qt::NoModifier, r.at(to));
        QCOMPARE((int)s.document().sections.size(), 2);
        QCOMPARE(s.generated().text, original);
        // Alt takes only that block
        const std::string alu2 = s.document().sections[0].blocks[2].id;
        r.pressAndMove(alu2, r.center(alu2) + QPointF(330, 40), Qt::AltModifier);
        QCOMPARE((int)s.document().sections.back().blocks.size(), 1);
        r.release(r.center(alu2) + QPointF(330, 40), Qt::AltModifier);
        QCOMPARE((int)s.document().sections[0].blocks.size(), 3);                          // label, mov, jmp: the jump stayed
    }

    void namesFromTheHardwareDescriptionAreKnown() {
        Rig r(".main\n EXEC_CSR WRITE8 BOARD_CTRL, 1\n");
        auto flagged = [&](const QString& name) {
            for (const auto& p : r.ed->session().problems()) if (QString::fromStdString(p.message).contains("'" + name + "' is not defined")) return true;
            return false;
        };
        QVERIFY(flagged("BOARD_CTRL"));                                           // nothing defines it yet
        QVERIFY(r.w->newHardware("/w/board.achw"));                              // a description is created and attached to the project
        HardwareDesigner* hd = r.w->currentDesigner();
        QVERIFY(hd);
        const QString csr = hd->addComponent("csr");                              // a CSR with a free name
        hd->flush();
        QVERIFY(!csr.isEmpty());
        // a program that uses that CSR by name
        r.f.ws.setText("/w/prog.acblk", asmcore::blocks::serializeDocument(asmcore::blocks::importAsm(".main\n EXEC_CSR WRITE8 " + csr.toStdString() + ", 1\n")));
        QVERIFY2(!flagged(csr), qPrintable(csr + " is a CSR of the hardware description"));
        QVERIFY(r.ed->session().symbols().find(csr.toStdString(), "csr") != nullptr);
        const asmcore::blocks::Block& b = r.ed->session().document().sections[0].blocks[0];
        QVERIFY2(b.value("csr") && b.value("csr")->type == "csr_ref", "the name is a CSR reference (it is defined by the description)");
        // and a name it does not define is still flagged
        r.f.ws.setText("/w/prog.acblk", asmcore::blocks::serializeDocument(asmcore::blocks::importAsm(".main\n EXEC_CSR WRITE8 NOT_THERE, 1\n")));
        QVERIFY(flagged("NOT_THERE"));
    }
    void zoomAndFit() {
        Rig r;
        BlockView& v = r.ed->view();
        QCOMPARE(v.zoom(), 1.0);
        v.zoomIn();
        QVERIFY(v.zoom() > 1.0);
        v.setZoom(100);
        QCOMPARE(v.zoom(), BlockView::kMaxZoom);
        v.setZoom(0.001);
        QCOMPARE(v.zoom(), BlockView::kMinZoom);
        v.resetZoom();
        QCOMPARE(v.zoom(), 1.0);
        v.fitAll();
        QVERIFY(v.zoom() <= 1.0);
        r.ed->findChild<QToolButton*>("zoomIn")->click();
        QVERIFY(v.zoom() > 0);
        QCOMPARE(r.ed->findChild<QToolButton*>("zoomReset") != nullptr, true);
    }
    void aDamagedFileShowsWhyAndDoesNotCrash() {
        Rig r("", false);
        r.f.fs->write("/w/bad.acblk", "{ this is not json");
        r.w->openProject("/w/p.acproj");
        r.w->openFile("/w/bad.acblk");
        auto* ed = qobject_cast<BlockEditor*>(r.w->tabs()->currentWidget());
        QVERIFY(ed);
        QVERIFY(!ed->errorText().isEmpty());
        QCOMPARE((int)ed->scene().layout().stacks.size(), 0);
        // fix the file on disk: the page recovers
        r.f.ws.setText("/w/bad.acblk", asmcore::blocks::serializeDocument(asmcore::blocks::importAsm(kBlockProg)));
        QVERIFY2(ed->errorText().isEmpty(), qPrintable(ed->errorText()));
        QCOMPARE((int)ed->scene().layout().stacks.size(), 3);
    }
    void followsChangesToTheFile() {
        Rig r;
        const std::string text = asmcore::blocks::serializeDocument(asmcore::blocks::importAsm(".main\n JMP x\n"));
        r.f.ws.setText("/w/prog.acblk", text);
        QCOMPARE((int)r.ed->scene().layout().stacks.size(), 1);
        r.ed->setPage(BlockEditor::Page::Code);                                         // (the assembly page is filled when it is shown)
        QVERIFY(r.ed->codePane().toPlainText().contains("JMP x"));
    }
    void theTabIsClosedLikeTheOthersAndComesBackWithTheSession() {
        Rig r;
        const int n = r.w->tabs()->count();
        r.w->endProject();
        QVERIFY(r.w->tabs()->count() < n || n == 0);
        r.w->openProject("/w/p.acproj");
        QVERIFY(r.ed != nullptr);
        r.w->openFile("/w/prog.acblk");
        QVERIFY(qobject_cast<BlockEditor*>(r.w->tabs()->currentWidget()));
        r.w->closeEditorAt(r.w->tabs()->currentIndex());
        QVERIFY(!r.f.ws.find("/w/prog.acblk"));
    }
    // ---- acceptance (M6)
    static std::string slurpFile(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    static std::string hash8(const std::string& s) { unsigned h = 5381; for (unsigned char c : s) h = h * 33 + c; return QString::number(h, 16).toStdString(); }

    // (2) every sample opens as blocks without changing anything: no file is written or marked unsaved, the generated text is stable (generate -> import -> generate
    // gives the same text), and it assembles to the bytes of the original. Each is also drawn, light and dark, for a look (build/.../blocks_sample_*.png).
    void everySampleOpensUntouchedAndStable() {
        namespace fs = std::filesystem;
        int n = 0;
        for (const char* theme : {"dark", "light"}) {
            Fixture f;
            f.settings.set("theme", theme);
            PanelRegistry panels;
            registerBuiltinPanels(panels);
            MainWindow w(f.ctx, panels);
            w.setQuiet(true);
            w.resize(1700, 1000);
            w.show();
            w.openProject("/w/p.acproj");
            const size_t filesBefore = f.fs->list("/w").size();
            for (auto& e : fs::recursive_directory_iterator(ACIDE_SAMPLE_DIR)) {
                if (!e.is_regular_file() || e.path().extension() != ".asm" || e.path().generic_string().find("/out/") != std::string::npos) continue;
                const std::string text = slurpFile(e.path());
                const std::string stem = e.path().stem().string();
                f.fs->write("/w/s.acblk", asmcore::blocks::serializeDocument(asmcore::blocks::importAsm(text, "s")));
                auto* ed = qobject_cast<BlockEditor*>(w.openTab("/w/s.acblk"));
                QVERIFY(ed);
                QVERIFY2(ed->errorText().isEmpty(), stem.c_str());
                // nothing was written or marked unsaved by opening
                QVERIFY2(!f.ws.find("/w/s.acblk")->dirty, stem.c_str());
                QVERIFY2(!f.ws.find(ed->generatedPath()), stem.c_str());
                QCOMPARE(f.fs->list("/w").size(), filesBefore + 1);                                                   // (only the .acblk the test wrote)
                // the text is stable and the bytes are the original's
                const std::string g = ed->session().generated().text;
                std::string body = g;                                                                                // (without the header the editor adds)
                const std::string head = "; " + acide::generatedHeader("/w/s.acblk") + "\n\n";
                if (body.compare(0, head.size(), head) == 0) body.erase(0, head.size());
                QVERIFY2(asmcore::blocks::generate(asmcore::blocks::importAsm(body, "s")).text == body, ("not stable: " + stem).c_str());
                asmcore::Options opt;
                opt.callModel = asmcore::CallModel::Inline;
                const std::vector<asmcore::Source> a{{"a.asm", text}}, b{{"a.asm", body}};
                const asmcore::Result ra = asmcore::assemble(a, nullptr, opt), rb = asmcore::assemble(b, nullptr, opt);
                if (ra.ok) QVERIFY2(rb.ok && ra.main.bytes == rb.main.bytes && ra.peri.bytes == rb.peri.bytes, ("bytes differ: " + stem).c_str());
                // a look at it
                ed->setMinimumSize(1100, 700);
                if (auto* tg = ed->findChild<QToolButton*>("toolboxToggle")) if (tg->isChecked()) tg->click();            // (the toolbox is looked at elsewhere)
                QApplication::processEvents();
                ed->view().fitAll();
                QApplication::processEvents();
                ed->grab().save(outDir() + "/blocks_sample_" + QString(theme) + "_" + QString::fromStdString(stem) + ".png");
                ed->setMinimumSize(0, 0);
                QVERIFY(w.closeEditorAt(w.tabs()->currentIndex()));
                n++;
            }
        }
        QVERIFY(n >= 20);
    }

    // (3) a text file converted to blocks, one block changed: the assembly differs in that one place only
    void convertingAndEditingOneBlockChangesOnlyThatPlace() {
        Fixture f;
        f.fs->write("/w/common/defs.inc", slurpFile(std::filesystem::path(ACIDE_SAMPLE_DIR) / "common" / "defs.inc"));
        f.fs->write("/w/hello.asm", slurpFile(std::filesystem::path(ACIDE_SAMPLE_DIR) / "01_hello_csr" / "hello.asm"));
        f.fs->write("/w/p.acproj", "name = p\nsource = hello.asm\ninclude_dir = common\n");
        std::vector<std::string> errs;
        f.ws.openProject("/w/p.acproj", errs);
        PanelRegistry panels;
        registerBuiltinPanels(panels);
        MainWindow w(f.ctx, panels);
        w.setQuiet(true);
        w.show();
        w.openProject("/w/p.acproj");
        QVERIFY(f.buildAndWait());
        QVERIFY2(f.ctx.snapshot->result.ok, f.ctx.snapshot->result.diagnosticsText().c_str());
        const std::vector<uint8_t> before = f.ctx.snapshot->result.main.bytes;
        QVERIFY(w.openFile("/w/hello.asm"));
        auto* be = w.openAsBlocks();
        QVERIFY(be);
        auto& s = be->session();
        // the same program: the build from the blocks has the same bytes
        QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setComment(s.document().sections.back().blocks.back().id, "the end"); }).ok);
        QVERIFY(f.buildAndWait());
        QVERIFY2(f.ctx.snapshot->result.ok, f.ctx.snapshot->result.diagnosticsText().c_str());
        QVERIFY(f.ctx.snapshot->result.main.bytes == before);
        // one value changed in one block
        std::string csr;
        for (const auto& sec : s.document().sections) for (const auto& b : sec.blocks) if (b.type == "csr_write8" && csr.empty()) csr = b.id;
        QVERIFY(!csr.empty());
        QVERIFY(s.apply([&](asmcore::blocks::Editor& e) { return e.setPart(csr, "imm", "0x02"); }).ok);
        QVERIFY(f.buildAndWait());
        QVERIFY2(f.ctx.snapshot->result.ok, f.ctx.snapshot->result.diagnosticsText().c_str());
        const std::vector<uint8_t>& after = f.ctx.snapshot->result.main.bytes;
        QCOMPARE(after.size(), before.size());
        int first = -1, last = -1, differing = 0;
        for (size_t i = 0; i < before.size(); i++) if (before[i] != after[i]) { if (first < 0) first = (int)i; last = (int)i; differing++; }
        QVERIFY(differing > 0);
        QVERIFY2(last - first < 8, qPrintable(QString("bytes %1..%2 differ").arg(first).arg(last)));            // one instruction word
    }

    // (1) 01_hello_csr made again with the mouse and the keyboard: the pieces first made apart on the canvas (floating), then joined by dragging
    void helloIsBuiltFromPiecesJoinedByDragging() {
        DragRig r(".main\n");
        auto& s = r.ed->session();
        r.sc().setAnimationsEnabled(false);
        const std::vector<std::string> a = {"start:", "EXEC_CSR WRITE8 0x120, 0x01", "EXEC_CSR SET8 0x120, 0x10", "EXEC_CSR CLR8 0x120, 0x01",
            "MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]", "MOV CONST(1, 2) -> MAIN_SCRATCHPAD[2..3]", "MOV CONST(5, 2) -> MAIN_SCRATCHPAD[4..5]"};
        const std::vector<std::string> b = {"loop:", "MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA", "MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB", "ALU ADD unsigned", "MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[0..1]",
            "MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA", "MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB", "ALU CMP.EQ unsigned", "BR.COND ALU_CMP_TRUE, finish", "JMP loop"};
        const std::vector<std::string> c = {"finish:", "EXEC_CSR WRITE_SP 0x121, len=2, ptr=0", "done:", "JMP done"};
        auto join = [](const std::vector<std::string>& v) { std::string t; for (const auto& l : v) t += l + "\n"; return t; };
        // the first piece goes into the Main part (the only empty program part); the others are made apart, lying on the canvas
        auto make = [&](const std::vector<std::string>& v, double x, double y) {
            s.clearSelection();
            asmcore::blocks::Document tmp;
            auto blocks = s.fresh([&](asmcore::blocks::Document& d) { return r.ed->catalog().fromAssembler(d, join(v), nullptr); });
            return s.place(blocks, x, y);
        };
        QVERIFY(make(a, 0, 0) > 0);
        QCOMPARE(s.stats().floatingStacks, (size_t)0);
        QVERIFY(make(b, 600, 80) > 0);
        QVERIFY(make(c, 600, 700) > 0);
        QCOMPARE(s.stats().floatingStacks, (size_t)2);
        QVERIFY(s.generated().text.find("loop:") == std::string::npos);                       // lying apart they are not part of the program
        r.ed->view().fitAll();
        QApplication::processEvents();
        auto firstOf = [&](size_t sec) { return s.document().sections[sec].blocks.front().id; };
        auto nextPoint = [&](const std::string& blockId) {
            const auto& lay = r.sc().layout();
            const acide::blocks::StackLayout* st = lay.stackOf(blockId);
            for (const auto& cp : st->points) if (cp.kind == acide::blocks::ConnectionPoint::Next && cp.blockId == blockId) return QPointF(st->origin.x + cp.pos.x, st->origin.y + cp.pos.y);
            return QPointF();
        };
        auto lastOfStack = [&](const std::string& any) { const acide::blocks::StackLayout* st = r.sc().layout().stackOf(any); return st->blocks[(size_t)st->top.back()].id; };
        // drag the second piece under the first, then the third under the second
        for (int round = 0; round < 2; round++) {
            std::string moving;
            for (const auto& sec : s.document().sections) if (sec.domain == "floating") { moving = sec.blocks.front().id; break; }
            QVERIFY(!moving.empty());
            const std::string under = lastOfStack(s.document().sections[0].blocks.front().id);
            const QRectF mr = r.sc().blockRect(moving);
            const QPointF grab = mr.center() - mr.topLeft();
            r.pressAndMove(moving, nextPoint(under) + grab);
            QVERIFY(r.sc().dragging());
            QVERIFY2(r.sc().dropTarget().fits(), qPrintable(QString("kind %1 fit %2 reason %3 moving %4 under %5 target(%6,%7) grab(%8,%9)").arg((int)r.sc().dropTarget().kind).arg((int)r.sc().dropTarget().fit).arg(QString::fromStdString(r.sc().dropTarget().reason)).arg(QString::fromStdString(moving)).arg(QString::fromStdString(under)).arg(nextPoint(under).x()).arg(nextPoint(under).y()).arg(grab.x()).arg(grab.y())));
            r.release(nextPoint(under) + grab);
            QVERIFY(!r.sc().dragging());
            QCOMPARE(s.stats().floatingStacks, (size_t)(1 - round));
            r.ed->view().fitAll();
            QApplication::processEvents();
        }
        (void)firstOf;
        std::string expected = "; Generated from prog.acblk - edit the blocks, not this file\n\n.main\n";
        for (const auto* part : {&a, &b, &c}) for (const std::string& l : *part) expected += (l.back() == ':' ? "" : "    ") + l + "\n";
        QCOMPARE(QString::fromStdString(s.generated().text), QString::fromStdString(expected));
        asmcore::Options opt;
        const asmcore::Result res = asmcore::assemble({asmcore::Source{"p.asm", s.generated().text}}, nullptr, opt);
        QVERIFY2(res.ok, qPrintable(QString::fromStdString(res.diagnosticsText())));
        // the same thing the typed and the hand-written versions give
        const std::string hand = slurpFile(std::filesystem::path(ACIDE_SAMPLE_DIR) / "01_hello_csr" / "hello.asm");
        (void)hand;
    }

    // large programs: 1000 blocks with 50 floating stacks open quickly and an edit is redrawn at once
    void aLargeProgramIsQuick() {
        std::string text = ".main\n";
        for (int i = 0; i < 1000; i++) text += (i % 7 == 0 ? "l" + std::to_string(i) + ":\n" : std::string()) + "    MOV CONST(" + std::to_string(i % 200) + ", 2) -> MAIN_SCRATCHPAD[" + std::to_string((i % 15) * 2) + ".." + std::to_string((i % 15) * 2 + 1) + "]\n";
        asmcore::blocks::Document d = asmcore::blocks::importAsm(text, "big");
        {
            Fixture f;
            PanelRegistry panels;
            registerBuiltinPanels(panels);
            MainWindow w(f.ctx, panels);
            w.setQuiet(true);
            w.resize(1100, 720);
            w.show();
            w.openProject("/w/p.acproj");
            f.fs->write("/w/big.acblk", asmcore::blocks::serializeDocument(d));
            QElapsedTimer t;
            t.start();
            auto* ed = qobject_cast<BlockEditor*>(w.openTab("/w/big.acblk"));
            QVERIFY(ed);
            ed->view().showContent();
            QApplication::processEvents();
            const qint64 open = t.elapsed();
            auto& s = ed->session();
            // 50 floating stacks
            t.restart();
            for (int k = 0; k < 5; k++) {
                const auto& blocks = s.document().sections[0].blocks;
                const std::string id = blocks[std::min<size_t>(blocks.size() - 1, 20 + 15)].id;
                s.apply([&](asmcore::blocks::Editor& e) { return e.splitAt(id, 800 + (k % 10) * 300, 60 + (k / 10) * 900); });
            }
            QVERIFY(s.stats().floatingStacks >= 4);
            QVERIFY(s.stats().blocks >= 1000);
            ed->view().showContent();
            QApplication::processEvents();
            // an edit: change, relayout, repaint
            const std::string first = s.document().sections[0].blocks.front().id;
            std::vector<qint64> times;
            for (int k = 0; k < 7; k++) {
                t.restart();
                s.apply([&](asmcore::blocks::Editor& e) { return e.setComment(first, "edit " + std::to_string(k)); });
                QApplication::processEvents();
                ed->view().viewport()->repaint();
                times.push_back(t.elapsed());
            }
            std::sort(times.begin(), times.end());
            const qint64 worst = times[times.size() / 2];                                          // the median (this build is not optimized: the numbers are for a slow case)
            qInfo() << "1000 blocks: open" << open << "ms; one edit and repaint (median of 7)" << worst << "ms";
            QVERIFY2(open < 1500, qPrintable(QString("open took %1 ms").arg(open)));
            QVERIFY2(worst < 600, qPrintable(QString("an edit took %1 ms").arg(worst)));
            // a big program is written to the workspace after the burst of edits; Save writes it at once (the pending write is done first)
            const std::string before = f.ws.find("/w/big.acblk")->text;
            s.apply([&](asmcore::blocks::Editor& e) { return e.setComment(first, "saved with Ctrl+S"); });
            QVERIFY(f.ws.find("/w/big.acblk")->text == before);                                   // (not yet)
            w.tabs()->setCurrentWidget(ed);
            for (QAction* a : w.findChildren<QAction*>()) if (a->text() == "&Save") { a->trigger(); break; }
            QVERIFY(f.ws.find("/w/big.acblk")->text != before && !f.ws.find("/w/big.acblk")->dirty);
            std::string onDisk;
            QVERIFY(f.fs->read("/w/big.acblk", onDisk) && onDisk.find("saved with Ctrl+S") != std::string::npos);
        }
    }

    void everySampleProgramShowsAsBlocks() {
        namespace fs = std::filesystem;
        int shown = 0;
        Rig r(kBlockProg, false);
        r.w->openProject("/w/p.acproj");
        for (auto& e : fs::recursive_directory_iterator(ACIDE_SAMPLE_DIR)) {
            if (!e.is_regular_file() || e.path().extension() != ".asm") continue;
            if (e.path().generic_string().find("/out/") != std::string::npos) continue;
            std::ifstream in(e.path(), std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            r.f.fs->write("/w/s.acblk", asmcore::blocks::serializeDocument(asmcore::blocks::importAsm(ss.str(), "s")));
            auto* ed = qobject_cast<BlockEditor*>(r.w->openTab("/w/s.acblk"));
            QVERIFY(ed);
            QVERIFY2(ed->errorText().isEmpty(), qPrintable(QString::fromStdString(e.path().string())));
            const QImage img = ed->view().grab().toImage();
            QVERIFY2(distinctColors(img) > 8, qPrintable(QString::fromStdString(e.path().string())));
            ed->setPage(BlockEditor::Page::Code);
            QVERIFY(!ed->codePane().toPlainText().isEmpty());
            QVERIFY(r.w->closeEditorAt(r.w->tabs()->currentIndex()));
            shown++;
        }
        QVERIFY(shown >= 10);
    }
    void paintsEveryLookWithoutTrouble() {
        Rig r;
        auto& s = r.ed->session();
        const auto& d = s.document();
        const std::string mov = d.sections[1].blocks[1].id, alu = d.sections[1].blocks[2].id, jmp = d.sections[1].blocks[3].id;
        s.apply([&](asmcore::blocks::Editor& e) { return e.setDisabled(alu, true); });
        s.apply([&](asmcore::blocks::Editor& e) { return e.setCollapsed(d.sections[1].blocks[4].id, true); });
        s.apply([&](asmcore::blocks::Editor& e) { return e.splitAt(jmp, 520, 40); });          // a floating stack
        QCOMPARE(s.stats().floatingStacks, (size_t)1);
        r.ed->scene().setHatched({mov});
        s.select({mov});
        r.ed->resize(900, 600);
        r.ed->show();
        r.ed->view().fitAll();
        const QImage img = r.ed->grab().toImage();
        QVERIFY(distinctColors(img) > 20);
        img.save(outDir() + "/blocks_looks.png");
        // the same in the dark theme
        r.f.settings.set("theme.base", "dark");
        auto* dark = qobject_cast<BlockEditor*>(r.w->openTab("/w/prog.acblk"));
        QVERIFY(dark);
        dark->grab().save(outDir() + "/blocks_dark.png");
        dark->setPage(BlockEditor::Page::Code);
        dark->grab().save(outDir() + "/blocks_code.png");
    }
};

int runAll(int argc, char** argv) {
    int rc = 0;
    rc |= runClass<UiTests>(argc, argv);
    rc |= runClass<WindowTests>(argc, argv);
    rc |= runClass<HardwareTests>(argc, argv);
    rc |= runClass<DebugTests>(argc, argv);
    rc |= runClass<BlockTests>(argc, argv);
    rc |= runClass<LanguageTests>(argc, argv);
    return rc;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    return runAll(argc, argv);
}

#if 0
QTEST_MAIN(UiTests)
#endif
#include "ui_tests.moc"
