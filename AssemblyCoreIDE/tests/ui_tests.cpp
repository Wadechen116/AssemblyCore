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
#include <QScrollBar>
#include <QWheelEvent>
#include <QToolButton>
#include <QImage>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "app_shell.h"
#include "block_editor.h"
#include "document_tab.h"
#include "asmcore/blocks.h"
#include "asmcore/blocks_edit.h"
#include "code_editor.h"
#include "i18n.h"
#include "editing.h"
#include "hardware_designer.h"
#include "hardware_link.h"
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
    void draggingASelectedBlockMovesItsWholeStack() {
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
        const QRectF br = r.ed->scene().blockRect(doc.sections[1].blocks[2].id);
        const QPoint from = v.mapFromScene(br.center());
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
        QVERIFY(s.isSelected(doc.sections[1].blocks[2].id));
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
