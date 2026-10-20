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

#include <QMenuBar>
#include <QXmlStreamReader>

#include "app_shell.h"
#include "code_editor.h"
#include "i18n.h"
#include "editing.h"
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
            QString source, translation, comment;
            while (!(xml.isEndElement() && xml.name() == QLatin1String("message")) && !xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()) continue;
                if (xml.name() == QLatin1String("source")) source = xml.readElementText();
                else if (xml.name() == QLatin1String("translation")) translation = xml.readElementText();
                else if (xml.name() == QLatin1String("comment")) comment = xml.readElementText();
            }
            QString got = QCoreApplication::translate(context.toUtf8().constData(), source.toUtf8().constData(), comment.isEmpty() ? nullptr : comment.toUtf8().constData());
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

int runAll(int argc, char** argv) {
    int rc = 0;
    rc |= runClass<UiTests>(argc, argv);
    rc |= runClass<WindowTests>(argc, argv);
    rc |= runClass<DebugTests>(argc, argv);
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
