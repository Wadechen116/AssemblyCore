// Tests of the IDE service layer: no Qt, no window.
#include <cstdio>
#include <filesystem>
#include <chrono>
#include <functional>
#include <set>
#include <thread>
#include <string>

#include "artifacts.h"
#include "block_link.h"
#include "build.h"
#include "debugger.h"
#include "hardware_link.h"
#include "asmcore/profile_info.h"
#include "dispatcher.h"
#include "editing.h"
#include "fs.h"
#include "project.h"
#include "settings.h"
#include "sim_session.h"
#include "workspace.h"

using namespace acide;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = "") {
    if (ok) g_pass++;
    else { g_fail++; std::printf("  FAIL  %s\n        %s\n", name.c_str(), detail.c_str()); }
}

static const char* kProgram =
    ".main\n"
    "start: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n"
    "h: JMP h\n";

static void t_realfs() {
    std::printf("\nReal file system\n");
    RealFileSystem fs;
    std::string dir = "/tmp/acide_fs_test_" + std::to_string((long long)&dir % 100000);
    std::string f = dir + "/sub/dir/a.txt", back;
    check(fs.write(f, std::string("x\0y", 3)) && fs.read(f, back) && back.size() == 3 && back[1] == '\0', "write creates directories; binary data survives");
    check(fs.exists(f) && fs.modified(f) > 0 && fs.modified(dir + "/none") == -1, "exists / modified");
    auto names = fs.list(dir + "/sub/dir");
    check(names.size() == 1 && names[0] == "a.txt", "list");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

static void t_paths() {
    std::printf("\nPaths\n");
    check(dirName("/a/b/c.asm") == "/a/b" && baseName("/a/b/c.asm") == "c.asm", "dirName / baseName");
    check(joinPath("/a/b", "../c/./d.inc") == "/a/c/d.inc", "joinPath collapses . and ..", joinPath("/a/b", "../c/./d.inc"));
    check(joinPath("/a/b", "/x/y") == "/x/y", "an absolute second argument wins");
    check(joinPath("C:/p/q", "../r") == "C:/p/r", "drive letters survive", joinPath("C:/p/q", "../r"));
    check(relativeTo("/a/b", "/a/b/c/d") == "c/d" && relativeTo("/a/b", "/x") == "/x", "relativeTo");
}

static void t_project() {
    std::printf("\nProject file\n");
    Project p;
    std::vector<std::string> errs;
    std::string text = "name = imu\nsource = defs.inc\nsource = main.asm\ninclude_dir = inc\ncall_model = softret\ndataflow = error\nsuppress = W502\nsuppress = W510\nwcet = a:b\nwerror = 1\nfuture_key = 7\n";
    check(Project::parse(text, p, errs) && errs.empty(), "parse");
    check(p.name == "imu" && p.sources.size() == 2 && p.callModel == asmcore::CallModel::SoftRet && p.dataflow == asmcore::DataflowMode::Error && p.suppress.size() == 2 && p.werror, "fields");
    Project q;
    check(Project::parse(p.serialize(), q, errs) && q.serialize() == p.serialize(), "serialize round trip");
    check(q.extra.size() == 1 && q.extra[0].first == "future_key", "unknown keys are kept");
    Project bad;
    std::vector<std::string> e2;
    check(!Project::parse("call_model = fast\nnonsense\n", bad, e2) && e2.size() == 2, "errors are reported with line numbers", e2.empty() ? "" : e2[0]);
    p.file = "/w/proj/imu.acproj";
    check(p.absolute("../x/y.asm") == "/w/x/y.asm" && p.options().callModel == asmcore::CallModel::SoftRet && p.options().suppress.size() == 2, "absolute() and options()");
}

static std::shared_ptr<MemoryFileSystem> demoFs() {
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->write("/w/imu.acproj", "name = imu\nsource = main.asm\ninclude_dir = inc\n");
    fs->write("/w/main.asm", ".include \"defs.inc\"\n.main\nstart: EXEC_CSR WRITE8 CTRL, 1\nh: JMP h\n");
    fs->write("/w/inc/defs.inc", ".csr CTRL = 0x120\n");
    return fs;
}

static void t_workspace() {
    std::printf("\nWorkspace\n");
    auto fs = demoFs();
    Workspace ws(fs);
    std::vector<std::string> errs;
    check(ws.openProject("/w/imu.acproj", errs) && ws.project().name == "imu", "open a project");
    int changes = 0;
    ws.documentChanged.connect([&](const std::string&) { changes++; });
    Document* d = ws.open("/w/main.asm");
    check(d && !d->dirty && d->version == 0, "open a document");
    ws.setText("/w/main.asm", d->text + "; edit\n");
    check(ws.find("/w/main.asm")->dirty && ws.find("/w/main.asm")->version == 1 && changes == 1, "editing marks it dirty and notifies");
    ws.setText("/w/main.asm", ws.find("/w/main.asm")->text);
    check(changes == 1, "setting the same text changes nothing");
    BuildInput in = ws.snapshot();
    check(in.sources.size() == 1 && in.sources[0].name == "/w/main.asm" && in.sources[0].text.find("; edit") != std::string::npos, "the snapshot contains the unsaved text");
    std::string disk;
    fs->read("/w/main.asm", disk);
    check(disk.find("; edit") == std::string::npos, "...while the file is untouched");
    std::string err;
    check(ws.save("/w/main.asm", err) && !ws.anyDirty(), "save");
    fs->write("/w/main.asm", "changed outside\n");
    auto ext = ws.externallyChanged();
    check(ext.size() == 1 && ext[0] == "/w/main.asm", "an external change is noticed");
    check(ws.reload("/w/main.asm") && ws.find("/w/main.asm")->text == "changed outside\n", "reload");
    fs->write("/w/main.asm", "again outside\n");
    check(ws.externallyChanged().size() == 1, "a second external change is noticed");
    ws.keepBuffer("/w/main.asm");
    check(ws.externallyChanged().empty() && ws.find("/w/main.asm")->dirty && ws.find("/w/main.asm")->text == "changed outside\n", "keepBuffer: the edits stay, the change is acknowledged");
    ws.setText("/w/new.asm", "x\n");
    check(ws.find("/w/new.asm") && ws.find("/w/new.asm")->dirty, "a new file is a dirty buffer");
    ws.close("/w/new.asm");
    check(!ws.find("/w/new.asm"), "close");
    check(!ws.openProject("/w/none.acproj", errs), "a missing project fails");
    {   // closing a project forgets it and its documents
        Workspace w2(fs);
        std::vector<std::string> e2;
        w2.openProject("/w/imu.acproj", e2);
        w2.open("/w/main.asm");
        int notified = 0;
        auto c = w2.projectChanged.connect([&] { notified++; });
        (void)c;
        w2.closeProject();
        check(!w2.hasProject() && w2.openPaths().empty() && w2.project().sources.empty() && notified == 1, "closeProject: no project, no documents, listeners told once", std::to_string(notified));
        check(w2.snapshot().sources.empty(), "...and a build of it would be empty");
        check(fs->exists("/w/imu.acproj"), "...the project file is untouched");
    }
}

// The sample projects (sample/) opened and built exactly the way the IDE does it: a Workspace over the real file system, buildNow().
static void t_samples() {
#ifdef ACIDE_SAMPLE_DIR
    std::printf("\nSample projects (%s)\n", ACIDE_SAMPLE_DIR);
    auto lang = std::make_shared<const asmcore::lang::Language>(asmcore::lang::Language::defaults());
    RealFileSystem rfs;
    auto fs = std::make_shared<RealFileSystem>();
    int n = 0;
    const std::string root = ACIDE_SAMPLE_DIR;
    for (const std::string& dname : rfs.list(root)) {
        const std::string dir = root + "/" + dname;
        for (const std::string& fname : rfs.list(dir)) {
            if (fname.size() < 7 || fname.compare(fname.size() - 7, 7, ".acproj") != 0) continue;
            const std::string f = dir + "/" + fname;
            Workspace ws(fs);
            std::vector<std::string> errs;
            if (!ws.openProject(f, errs)) { check(false, "opens " + f, errs.empty() ? "" : errs[0]); continue; }
            n++;
            std::string expect = "ok", want;
            for (auto& kv : ws.project().extra) { if (kv.first == "sample_expect") expect = kv.second; if (kv.first == "sample_errors") want = kv.second; }
            SnapshotPtr s = buildNow(ws.snapshot(), *lang);
            const std::string name = ws.project().name;
            if (expect == "fail") {
                bool all = !s->result.ok;
                for (size_t at = 0; at < want.size();) {
                    size_t sp = want.find(' ', at);
                    std::string code = want.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
                    bool found = false;
                    for (auto& d : s->result.diags) if (d.code == code) found = true;
                    all = all && found;
                    if (sp == std::string::npos) break;
                    at = sp + 1;
                }
                check(all, name + ": the build fails with the documented codes (" + want + ")", s->result.diagnosticsText().substr(0, 300));
            } else {
                check(s->result.ok && s->notes.empty(), name + ": builds in the IDE", s->result.diagnosticsText().substr(0, 300) + (s->notes.empty() ? "" : s->notes[0]));
                // the symbol index and the line map work on it (outline, go to definition, listing sync)
                check(s->map != nullptr && !s->index.symbols().empty(), name + ": index and source map exist");
            }
        }
    }
    check(n >= 10, "found the sample projects", std::to_string(n));
#endif
}

// The block programs' bridge: the project lists them, the generated file is a source and a workspace buffer.
static void t_block_link() {
    std::printf("\nBlock programs in the workspace\n");
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->write("/p/prog.acproj", "name = prog\nsource = defs.inc\nblock = old.acblk\n");
    Workspace ws(fs);
    std::vector<std::string> errs;
    ws.openProject("/p/prog.acproj", errs);
    check(ws.project().blocks.size() == 1 && ws.project().blocks[0] == "old.acblk", "the project reads `block =`");
    check(ws.project().serialize().find("block = old.acblk") != std::string::npos, "...and writes it back");
    asmcore::blocks::Document d;
    d.name = "hello";
    check(generatedPathFor("/p/hello.acblk", d) == "/p/hello.generated.asm", "the default name of the generated file");
    d.output = "hello.asm";
    check(generatedPathFor("/p/sub/hello.acblk", d) == "/p/sub/hello.asm" && generatedPathFor("/p/x.acblk", asmcore::blocks::Document()) == "/p/x.generated.asm", "`output` is relative to the program");
    check(generatedHeader("/p/hello.acblk").find("hello.acblk") != std::string::npos, "the header names the program");
    std::string err;
    bool changed = false;
    ws.setText("/p/hello.asm", ".main\nh: JMP h\n");                                            // (only a buffer so far)
    check(!fs->exists("/p/hello.asm"), "the generated file is not on disk yet");
    check(attachBlocks(ws, "/p/hello.acblk", "/p/hello.asm", err, &changed) && changed, "attach", err);
    check(fs->exists("/p/hello.asm") && !ws.find("/p/hello.asm")->dirty, "...and attaching writes it: the project never names a file that is not there");
    check(ws.project().blocks.size() == 2 && ws.project().blocks[1] == "hello.acblk" && ws.project().sources.back() == "hello.asm", "the program is listed and its generated file is a source");
    std::string t;
    check(fs->read("/p/prog.acproj", t) && t.find("block = hello.acblk") != std::string::npos, "...and the project file was saved");
    check(attachBlocks(ws, "/p/hello.acblk", "/p/hello.asm", err, &changed) && !changed && ws.project().sources.size() == 2, "attaching twice changes nothing");
    check(attachBlocks(ws, "/elsewhere/z.acblk", "/elsewhere/z.asm", err, &changed) && !changed && ws.project().blocks.size() == 2, "a program outside the project is left alone");
    // which files are generated
    d.output = "hello.asm";
    fs->write("/p/hello.acblk", asmcore::blocks::serializeDocument(d));
    auto gen = generatedFiles(ws);
    check(gen.size() == 1 && gen["/p/hello.asm"] == "/p/hello.acblk", "the generated files of the project (an unreadable program is skipped)");
    check(detachBlocks(ws, "/p/hello.acblk", err) && ws.project().blocks.size() == 1 && ws.project().sources.back() == "hello.asm" && generatedFiles(ws).empty(), "detach: the program goes, the file stays a source and is plain text again", err);
}

// The Hardware Designer's bridge: description, generated files and the project are kept in step, and the build uses them unsaved.
static void t_hardware_link() {
    std::printf("\nHardware description\n");
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->write("/p/prog.acproj", "name = prog\nsource = main.asm\n");
    fs->write("/p/main.asm", ".main\nstart: EXEC_CSR WRITE8 AE, 1\nh: JMP h\n");
    Workspace ws(fs);
    std::vector<std::string> errs;
    ws.openProject("/p/prog.acproj", errs);
    std::string err;
    check(createHardware(ws, "/p/board.achw", err), "create a description", err);
    std::string t;
    check(fs->read("/p/board.achw", t) && t.find("name = board") != std::string::npos && fs->exists("/p/board.gen.inc") && fs->exists("/p/board.gen.env.cfg"), "the description and both generated files are written");
    check(ws.project().hardware == "board.achw" && ws.project().sources.size() == 2 && ws.project().sources[0] == "board.gen.inc" && ws.project().environment == "board.gen.env.cfg", "the project uses them: hardware key, generated include FIRST, environment");
    check(fs->read("/p/prog.acproj", t) && t.find("hardware = board.achw") != std::string::npos, "...and the project file was saved");
    // an edit shows up in the build without saving anything
    asmcore::Hardware hw;
    check(loadHardware(ws, "/p/board.achw", hw, errs), "load");
    asmcore::HwCsr c;
    c.name = "AE";
    c.addr = 0x10;
    hw.csrs.push_back(c);
    applyHardware(ws, "/p/board.achw", hw);
    check(ws.find("/p/board.gen.inc") && ws.find("/p/board.gen.inc")->dirty && ws.find("/p/board.gen.inc")->text.find(".csr AE = 0x010") != std::string::npos, "an edit puts the new definitions into the workspace as an unsaved buffer");
    auto lang = std::make_shared<const asmcore::lang::Language>(asmcore::lang::Language::defaults());
    SnapshotPtr s = buildNow(ws.snapshot(), *lang);
    check(s->result.ok, "the program that uses the new CSR builds although nothing was saved", s->result.diagnosticsText());
    fs->read("/p/board.gen.inc", t);
    check(t.find("AE") == std::string::npos, "(the file on disk is still the old one)");
    std::string werr;
    check(ws.saveAll(werr) && fs->read("/p/board.gen.inc", t) && t.find(".csr AE = 0x010") != std::string::npos && fs->read("/p/board.achw", t) && t.find("[csr AE]") != std::string::npos, "Save All writes the description and the generated files", werr);
    // the environment of a debug session is the generated one, also before it exists on disk
    hw.csrs[0].addr = 0x11;
    applyHardware(ws, "/p/board.achw", hw);
    check(ws.find("/p/board.gen.env.cfg")->text.find("addr = 0x011") != std::string::npos, "the generated environment follows the edit");
    // the profile: created, edited without saving, used by the build and the debugger
    check(createProfile(ws, "/p/target.profile", err) && ws.project().profile == "target.profile" && fs->exists("/p/target.profile"), "create a profile for the project", err);
    std::string ptext, ppath;
    check(loadProjectProfile(ws, ptext, ppath) && ppath == "/p/target.profile" && ptext.find("profile_id = target") != std::string::npos && ptext.find("PLANNING") != std::string::npos, "...with the default values and a header");
    ws.setText("/p/target.profile", asmcore::profileUpdateKey(ptext, "main_image_base_word", "9999999"));
    SnapshotPtr sp = buildNow(ws.snapshot(), *lang);
    check((!sp->notes.empty() && sp->notes[0].find("target.profile") != std::string::npos) || !sp->result.ok, "an unsaved profile edit is what the build uses (here: a window that does not fit the SRAM)", sp->result.diagnosticsText());
    ws.setText("/p/target.profile", ptext);
    // adopt existing files: nothing in the project changes, the originals become generated files
    auto fs2 = std::make_shared<MemoryFileSystem>();
    fs2->write("/q/q.acproj", "name = q\nsource = defs.inc\nsource = main.asm\nenvironment = env.cfg\n");
    fs2->write("/q/defs.inc", ".csr AE = 0x010\n.service S = 1, len=2, big\n");
    fs2->write("/q/env.cfg", "[csr AE]\nkind = ram\nreset = 5\n");
    fs2->write("/q/main.asm", ".main\nstart: EXEC_CSR WRITE8 AE, 1\nh: JMP h\n");
    Workspace w2(fs2);
    w2.openProject("/q/q.acproj", errs);
    std::vector<std::string> msgs;
    check(adoptHardware(w2, "/q/q.achw", "/q/defs.inc", "/q/env.cfg", msgs, err), "adopt existing definitions", err);
    asmcore::Hardware h2;
    check(loadHardware(w2, "/q/q.achw", h2, errs) && h2.includeFile == "defs.inc" && h2.envFile == "env.cfg" && h2.csrs.size() == 1 && h2.csrs[0].reset == 5 && h2.services.size() == 1, "the imported description points at the same files", err);
    check(w2.project().sources.size() == 2 && w2.project().sources[0] == "defs.inc" && w2.project().environment == "env.cfg" && w2.project().hardware == "q.achw", "the project's sources and environment are unchanged");
    SnapshotPtr s2 = buildNow(w2.snapshot(), *lang);
    check(s2->result.ok, "and it still builds with the regenerated include in place of the original", s2->result.diagnosticsText());
    std::string orig;
    fs2->read("/q/defs.inc", orig);
    check(orig.find("Generated") == std::string::npos, "(the original file is only replaced when the designer saves)");
}

static void t_build() {
    std::printf("\nBuild\n");
    auto lang = std::make_shared<const asmcore::lang::Language>(asmcore::lang::Language::defaults());
    auto fs = demoFs();
    Workspace ws(fs);
    std::vector<std::string> errs;
    ws.openProject("/w/imu.acproj", errs);
    SnapshotPtr s = buildNow(ws.snapshot(), *lang);
    check(s->result.ok, "the demo project builds (an include found through the file system)", s->result.diagnosticsText());
    check(s->index.find("CTRL") && s->index.find("CTRL")->kind == "csr" && s->index.find("CTRL")->refs.size() == 1, "the index follows .include");
    check(s->map->instrsForLine("/w/main.asm", 3).size() == 1, "source map");
    {
        std::string werr;
        ArtifactOptions ao;
        ao.sources = s->sources;
        auto files = writeArtifacts(*fs, "/w/out/imu", s->result, ao, werr);
        std::string bin, layout, manifest;
        check(werr.empty() && fs->read("/w/out/imu.main.bin", bin) && bin.size() == s->result.main.bytes.size(), "artifacts are written next to each other", werr + std::to_string(files.size()));
        check(!fs->exists("/w/out/imu.peri.bin") && !fs->exists("/w/out/imu.peri.layout.json"), "...only for the domains present");
        check(fs->read("/w/out/imu.main.layout.json", layout) && layout.find("main_move_desc_offset") != std::string::npos, "the sideband ImageLayout is written next to the image (v31: the .bin has none)");
        check(fs->exists("/w/out/imu.sram.hex") && fs->exists("/w/out/imu.boot.json") && fs->exists("/w/out/imu.memory.json") && fs->exists("/w/out/imu.events.json") && fs->exists("/w/out/imu.validation.json"), "the v31 artifact set is written");
        check(fs->read("/w/out/imu.manifest.json", manifest) && manifest.find("\"sources\"") != std::string::npos, "the manifest hashes the sources that were built");
        ArtifactOptions plain;
        plain.v31 = false;
        std::string e2;
        auto few = writeArtifacts(*fs, "/w/out/few", s->result, plain, e2);
        check(e2.empty() && fs->exists("/w/out/few.main.layout.json") && !fs->exists("/w/out/few.sram.hex"), "with v31 off only the layout stays (it is not optional)");
    }
    // the unsaved buffer is what gets built, and the included buffer wins over the disk
    ws.setText("/w/main.asm", ".include \"defs.inc\"\n.main\nstart: EXEC_CSR WRITE8 NOPE, 1\nh: JMP h\n");
    s = buildNow(ws.snapshot(), *lang);
    check(!s->result.ok && s->diagnosticsFor("/w/main.asm").size() == 1 && s->diagnosticsFor("/w/main.asm")[0].loc.line == 3, "an error in an unsaved buffer is found, per file", s->result.diagnosticsText());
    check(s->diagnosticsFor("/w/other").empty(), "no diagnostics for another file");
    ws.setText("/w/main.asm", ".include \"defs.inc\"\n.main\nstart: EXEC_CSR WRITE8 CTRL, 1\nh: JMP h\n");
    ws.setText("/w/inc/defs.inc", ".csr CTRL = 0x7FFF\n");
    s = buildNow(ws.snapshot(), *lang);
    check(!s->result.ok, "an edited, unsaved include is used (CTRL out of range)", s->result.diagnosticsText());
    // a profile that does not exist
    ws.project().profile = "missing.ini";
    s = buildNow(ws.snapshot(), *lang);
    check(!s->notes.empty(), "a missing profile is reported as a note");
}

static void t_service() {
    std::printf("\nBuild service (background thread)\n");
    auto lang = std::make_shared<const asmcore::lang::Language>(asmcore::lang::Language::defaults());
    auto disp = std::make_shared<QueueDispatcher>();
    auto fs = demoFs();
    Workspace ws(fs);
    std::vector<std::string> errs;
    ws.openProject("/w/imu.acproj", errs);
    BuildService svc(disp, lang);
    std::vector<uint64_t> got;
    std::vector<bool> oks;
    svc.finished.connect([&](SnapshotPtr s) { got.push_back(s->id); oks.push_back(s->result.ok); });
    uint64_t a = svc.request(ws.snapshot());
    check(disp->pumpUntil([&] { return !got.empty(); }, 5000) && got.back() == a && oks.back(), "a request finishes and is delivered on the pumping thread");
    check(svc.latest() && svc.latest()->id == a, "latest()");
    // many quick requests: only the newest result is delivered
    got.clear();
    uint64_t last = 0;
    for (int i = 0; i < 20; i++) { ws.setText("/w/main.asm", std::string(kProgram) + "; v" + std::to_string(i) + "\n"); last = svc.request(ws.snapshot()); }
    check(disp->pumpUntil([&] { return !got.empty() && got.back() == last; }, 5000), "the newest of 20 requests is delivered");
    for (size_t i = 1; i < got.size(); i++) check(got[i] > got[i - 1], "results arrive in request order");
    check(got.size() < 20, "older requests were coalesced or dropped", std::to_string(got.size()) + " delivered");
    // destroying the service with work in flight must not touch freed memory
    for (int i = 0; i < 5; i++) svc.request(ws.snapshot());
    check(true, "(destruction with pending work follows)");
}

static void t_destroy() {
    auto lang = std::make_shared<const asmcore::lang::Language>(asmcore::lang::Language::defaults());
    auto disp = std::make_shared<QueueDispatcher>();
    auto fs = demoFs();
    Workspace ws(fs);
    std::vector<std::string> errs;
    ws.openProject("/w/imu.acproj", errs);
    int delivered = 0;
    {
        BuildService svc(disp, lang);
        svc.finished.connect([&](SnapshotPtr) { delivered++; });
        for (int i = 0; i < 10; i++) svc.request(ws.snapshot());
    }
    disp->runPending();                                         // posted lambdas outlive the service
    check(true, "posted results after the service is gone are ignored");
}

static void t_settings() {
    std::printf("\nSettings\n");
    Settings s;
    int ev = 0;
    s.changed.connect([&](const std::string&) { ev++; });
    s.set("font.size", "11");
    s.set("font.size", "11");
    check(ev == 1 && s.getInt("font.size") == 11 && s.getInt("none", 5) == 5, "typed access, no event for an unchanged value");
    s.setBool("ui.dark", true);
    check(s.getBool("ui.dark") && !s.getBool("x"), "bool");
    s.pushRecent("recent", "a"); s.pushRecent("recent", "b"); s.pushRecent("recent", "a", 2);
    auto r = s.getList("recent");
    check(r.size() == 2 && r[0] == "a" && r[1] == "b", "most-recently-used list");
    MemoryFileSystem fs;
    s.save(fs, "/s.ini");
    Settings t;
    check(t.load(fs, "/s.ini") && t.getInt("font.size") == 11 && t.getList("recent").size() == 2, "save and load");
}

static std::vector<std::string> labels(const std::vector<CompletionItem>& v) {
    std::vector<std::string> r;
    for (auto& i : v) r.push_back(i.label);
    return r;
}
static bool has(const std::vector<CompletionItem>& v, const std::string& l) {
    for (auto& i : v) if (i.label == l) return true;
    return false;
}

static void t_editing() {
    std::printf("\nCompletion, hover, rename\n");
    using asmcore::lang::Dom;
    auto L = asmcore::lang::Language::defaults();
    const char* src =
        ".csr AE_CTRL = 0x120\n.service SA = 1\n.devmap 0, I2C, 0x68, imu\n.equ LIMIT = 5\n"
        ".main\n.proc f\n  RET\n.endp\nstart: CALL f\n  BR.COND PERI_RESULT_VALID, start\nh: JMP h\n"
        ".peri\nstart: EXEC_PUB service=SA, src=0, len=1\n  JMP start\nonlyperi: JMP start\n";
    asmcore::Index ix = asmcore::Index::build({asmcore::Source{"a.asm", src}}, L);
    auto at = [&](const std::string& line, Dom d) { return complete(L, &ix, line, (int)line.size(), d); };
    auto mn = at("  M", Dom::Main);
    check(has(mn, "MOV") && !has(mn, "EXEC_PUB") && !has(mn, "JMP"), "statement start: only mnemonics with that prefix, for the domain", labels(mn).size() ? labels(mn)[0] : "none");
    check(has(at("  EXEC_P", Dom::Peri), "EXEC_PUB") && !has(at("  EXEC_P", Dom::Main), "EXEC_PUB"), "EXEC_PUB is offered in Peri only");
    check(has(at("  .a", Dom::Main), ".ASSUME"), "directives after a dot");
    check(has(at("  MOV ", Dom::Main), "MAIN_SCRATCHPAD") && has(at("  MOV A", Dom::Main), "ALU_OPA") && !has(at("  MOV A", Dom::Main), "MAIN_SCRATCHPAD"), "MOV offers regions, filtered by prefix");
    check(has(at("  BR.COND ", Dom::Main), "ALU_CMP_TRUE") && !has(at("  BR.COND ", Dom::Main), "MOV"), "BR.COND operand 0: conditions");
    auto tg = at("  BR.COND ALWAYS, ", Dom::Main);
    check(has(tg, "start") && has(tg, "h") && !has(tg, "onlyperi"), "BR.COND operand 1: the labels of this domain only", labels(tg).size() ? labels(tg)[0] : "none");
    check(has(at("  CALL ", Dom::Main), "f") && !has(at("  CALL ", Dom::Main), "start"), "CALL: procedures");
    check(has(at("  EXEC_CSR WRITE8 ", Dom::Main), "AE_CTRL"), "EXEC_CSR operand 1: csr symbols");
    check(has(at("  EXEC_CSR ", Dom::Main), "WRITE_SP") && !has(at("  EXEC_CSR ", Dom::Peri), "WRITE_SP"), "EXEC_CSR operand 0: the operations of the domain");
    check(has(at("  ALU ", Dom::Main), "CMP.EQ") && has(at("  ALU ADD ", Dom::Main), "UNSIGNED") && !has(at("  ALU ADD ", Dom::Main), "ADD"), "ALU: operations, then the sign modifier");
    check(has(at("  BR.COND ALWAYS, start, ", Dom::Main), "SAFE") && !has(at("  BR.COND ALWAYS, ", Dom::Main), "SAFE"), "BR.COND offers the safe attribute after the label (event checkpoint)");
    check(has(at("  JMP start, ", Dom::Main), "SAFE") && has(at("  BR.IF_READY p, ", Dom::Peri), "SAFE") && !has(at("  CASCADE_END 0, start, ", Dom::Main), "SAFE"), "JMP and the Peri branches offer safe, CASCADE_END never");
    check(has(at("  BR.E", Dom::Main), "BR.ERET") && has(at("  BR.E", Dom::Peri), "BR.ERET"), "BR.ERET is a mnemonic of both domains");
    check(has(at("  EXEC_PUB ", Dom::Peri), "SERVICE") && has(at("  EXEC_PUB service=", Dom::Peri), "SA"), "EXEC_PUB: argument names, then service symbols");
    check(has(at("  EXEC_XFER ", Dom::Peri), "READ") && has(at("  EXEC_XFER READ ", Dom::Peri), "DEV"), "EXEC_XFER");
    {
        auto it = at("  EXEC_PUB ", Dom::Peri);
        std::string ins;
        for (auto& i : it) if (i.label == "SERVICE") ins = i.insert;
        check(ins == "service=", "an argument name inserts 'name='", ins);
    }
    check(at("  .proc ", Dom::Main).empty(), "a new name: nothing to offer");
    check(has(complete(L, nullptr, "  MOV ", 6, Dom::Main), "MAIN_SCRATCHPAD"), "works without an index");
    check(domainAtLine(L, src, 5) == Dom::None && domainAtLine(L, src, 6) == Dom::Main && domainAtLine(L, src, 13) == Dom::Peri, "domainAtLine");

    std::string h = hoverText(L, &ix, nullptr, "a.asm", 4, ".equ LIMIT = 5", 7, Dom::None);
    check(h.find("equ LIMIT") == 0, "hover on a symbol", h);
    std::string h2 = hoverText(L, &ix, nullptr, "a.asm", 11, "h: JMP h", 4, Dom::Main);
    check(h2.find("JMP label") != std::string::npos, "hover on a mnemonic shows the signature", h2);
    check(hoverText(L, &ix, nullptr, "a.asm", 1, "   ", 1, Dom::Main).empty(), "nothing to say on blanks");

    std::map<std::string, std::string> texts{{"a.asm", src}};
    std::string problem;
    const asmcore::IndexSymbol* st = ix.find("start", "main");
    auto edits = renameEdits(ix, *st, "begin", texts, problem);
    check(problem.empty() && edits.size() == 1 && edits["a.asm"].find("begin: CALL f") != std::string::npos && edits["a.asm"].find("PERI_RESULT_VALID, begin") != std::string::npos,
          "rename changes the definition and the use", edits.empty() ? problem : edits["a.asm"]);
    check(edits["a.asm"].find("start: EXEC_PUB") != std::string::npos && edits["a.asm"].find("JMP start") != std::string::npos, "the label of the same name in the other domain is not renamed");
    check(renameEdits(ix, *st, "MOV", texts, problem).empty() && !problem.empty(), "a keyword is refused");
    std::string edited = src;
    edited.replace(edited.find("start: CALL"), 5, "stArt");   // the buffer changed since the index was built
    texts["a.asm"] = edited;
    check(renameEdits(ix, *st, "begin", texts, problem).empty() && problem.find("changed") != std::string::npos, "a stale index is detected, nothing is edited", problem);
}

// ---- simulator session ---------------------------------------------------------------------------------------------------------------
static SnapshotPtr buildSource(const std::string& src) {
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->write("/p/p.acproj", "name = p\nsource = a.asm\n");
    fs->write("/p/a.asm", src);
    Workspace ws(fs);
    std::vector<std::string> errs;
    ws.openProject("/p/p.acproj", errs);
    auto lang = asmcore::lang::Language::defaults();
    return buildNow(ws.snapshot(), lang);
}

struct SessionRig {
    std::shared_ptr<QueueDispatcher> disp = std::make_shared<QueueDispatcher>();
    SimSession ses;
    std::vector<SimUpdate> ups;
    int loadedCount = 0;
    bool loadedOk = false;
    std::vector<std::string> loadErrors;
    SessionRig() : ses(disp) {
        ses.updated.connect([this](const SimUpdate& u) { ups.push_back(u); });
        ses.loaded.connect([this](bool ok, const std::vector<std::string>& e) { loadedCount++; loadedOk = ok; loadErrors = e; });
    }
    // waits for an update that satisfies `pred`, returns it (or nullptr)
    const SimUpdate* waitFor(const std::function<bool(const SimUpdate&)>& pred, int ms = 5000) {
        size_t seen = 0;
        const SimUpdate* hit = nullptr;
        disp->pumpUntil([&] {
            for (; seen < ups.size(); seen++) if (pred(ups[seen])) { hit = &ups[seen]; return true; }
            return false;
        }, ms);
        return hit;
    }
    const SimUpdate* waitStop(int ms = 5000) { return waitFor([](const SimUpdate& u) { return u.stop.valid; }, ms); }
    void clear() { disp->runPending(); ups.clear(); }
    bool loadSource(const std::string& src, const std::string& env = "") {
        SnapshotPtr prog = buildSource(src);
        if (!prog->result.ok) { std::printf("        build failed: %s\n", prog->result.diagnosticsText().c_str()); return false; }
        ses.load(prog, env, asmcore::Profile());
        return disp->pumpUntil([&] { return loadedCount > 0; }, 5000);
    }
};

static const char* kEndless = ".csr MARK = 0x50\n.main\ns: EXEC_CSR WRITE8 MARK, 1\n EXEC_CSR WRITE8 MARK, 2\n JMP s\n";
static const char* kFinite = ".csr MARK = 0x50\n.main\ns: EXEC_CSR WRITE8 MARK, 7\n EXEC_CSR WRITE8 MARK, 8\nh: JMP h\n";

struct DebugRig {
    std::shared_ptr<MemoryFileSystem> fs = std::make_shared<MemoryFileSystem>();
    std::shared_ptr<QueueDispatcher> disp = std::make_shared<QueueDispatcher>();
    Workspace ws;
    Settings settings;
    SimSession ses;
    SnapshotPtr latest;
    Debugger dbg;
    std::vector<SimUpdate> ups;
    std::vector<DebugLocation> stops;
    std::vector<asmcore::sim::StopReason> reasons;
    int loadedCount = 0;
    bool loadedOk = false;
    explicit DebugRig(const std::string& src, const std::string& projectExtra = "")
        : ws(fs), ses(disp), dbg(ses, ws, &settings, [this] { return latest; }) {
        fs->write("/p/p.acproj", "name = p\nsource = a.asm\n" + projectExtra);
        fs->write("/p/a.asm", src);
        std::vector<std::string> errs;
        ws.openProject("/p/p.acproj", errs);
        rebuild();
        ses.loaded.connect([this](bool ok, const std::vector<std::string>&) { loadedCount++; loadedOk = ok; });
        dbg.stopped.connect([this](const SimUpdate& u, const DebugLocation& l) { stops.push_back(l); reasons.push_back(u.stop.reason); });
        dbg.updated.connect([this](const SimUpdate& u) { ups.push_back(u); });
    }
    void rebuild() { auto lang = asmcore::lang::Language::defaults(); latest = buildNow(ws.snapshot(), lang); }
    bool begin(std::string& why) {
        if (!dbg.start(why)) return false;
        return disp->pumpUntil([&] { return loadedCount > 0; }, 5000) && loadedOk;
    }
    bool waitStop(size_t n, int ms = 5000) { return disp->pumpUntil([&] { return stops.size() >= n; }, ms); }
};

static void t_debugger() {
    std::printf("\nDebugger (lines, locations, stale)\n");
    using namespace asmcore;
    // line numbers:  1 .csr  2 .main  3 .proc  4 write  5 RET  6 .endp  7 start: CALL  8 CALL  9 done
    const std::string prog =
        ".csr MARK = 0x50\n.main\n.proc leaf\n EXEC_CSR WRITE8 MARK, 1\n RET\n.endp\nstart: CALL leaf\n CALL leaf\ndone: JMP done\n";
    {
        DebugRig r(prog);
        check(r.latest->result.ok, "setup builds", r.latest->result.diagnosticsText());
        check(r.dbg.toggleLineBreakpoint("/p/a.asm", 4) == 4 && r.dbg.hasLineBreakpoint("/p/a.asm", 4), "a breakpoint on a code line");
        check(r.dbg.toggleLineBreakpoint("/p/a.asm", 4) == 4 && !r.dbg.hasLineBreakpoint("/p/a.asm", 4), "toggling again removes it");
        check(r.dbg.toggleLineBreakpoint("/p/a.asm", 6) == 7 && r.dbg.hasLineBreakpoint("/p/a.asm", 7), "a breakpoint on a line without code moves to the next line with code (.endp -> line 7)");
        check(r.dbg.toggleLineBreakpoint("/p/a.asm", 6) == 7 && !r.dbg.hasLineBreakpoint("/p/a.asm", 7), "...and toggling the same spot again removes that one");
        check(r.dbg.toggleLineBreakpoint("/p/a.asm", 99) == 0 && r.dbg.lineBreakpoints().empty(), "a line past the last code line gets none");
        std::string why;
        DebugRig bad(".main\nx: JMP nowhere\n");
        check(!bad.dbg.start(why) && !why.empty(), "start() refuses a program with errors", why);
        DebugRig none(prog);
        none.latest.reset();
        check(!none.dbg.start(why), "start() refuses when nothing has been built");
    }
    {   // an inline CALL expands line 4 at two places: one line, two addresses, two stops
        DebugRig r(prog);
        std::string why;
        r.dbg.toggleLineBreakpoint("/p/a.asm", 4);
        check(r.begin(why), "start", why);
        r.dbg.run();
        check(r.waitStop(1) && r.reasons.back() == asmcore::sim::StopReason::Breakpoint && r.stops.back().valid && r.stops.back().line == 4 && r.stops.back().file == "/p/a.asm", "the run stops at the breakpoint and reports the source line");
        check(!r.stops.back().frames.empty() && r.stops.back().frames.back().callSite.line == 7, "...with the call chain (called from line 7)");
        r.dbg.run();
        check(r.waitStop(2) && r.reasons.back() == asmcore::sim::StopReason::Breakpoint && r.stops.back().frames.back().callSite.line == 8, "the second expansion of the same line stops as well (called from line 8)");
        auto lb = r.dbg.lineBreakpoints();
        check(lb.size() == 1 && lb[0].hits == 2 && lb[0].resolved, "the hits of both addresses are added up for the line", lb.empty() ? "" : std::to_string(lb[0].hits));
        check(r.dbg.location(Domain::Main).valid && r.dbg.location(Domain::Main).line == 4, "location() maps the current pc to a line");
        check(!r.dbg.stale(), "the program is not stale right after a build");
        r.ws.setText("/p/a.asm", prog + "; edit\n");
        check(r.dbg.stale(), "an edit makes the program being debugged stale");
        r.dbg.stepOut();
        check(r.waitStop(3) && r.reasons.back() == asmcore::sim::StopReason::InstrLimit && r.stops.back().line == 9, "stepOut returns to the caller (line 9 after the last CALL)", std::to_string(r.stops.empty() ? -1 : r.stops.back().line));
        r.dbg.stepInto();
        check(r.waitStop(4) && r.stops.back().line == 9, "stepInto on the final loop stays on its line");
    }
    {   // run to line, restart, conditions, enable/disable
        DebugRig r(prog);
        std::string why;
        check(r.begin(why), "start", why);
        check(!r.dbg.runToLine("/p/a.asm", 6, why) && !why.empty(), "runToLine refuses a line without code", why);
        check(r.dbg.runToLine("/p/a.asm", 8, why), "runToLine accepts a line with code");
        bool got = r.waitStop(1);
        check(got && r.stops.back().line == 8, "...and stops there (line 8)", "got=" + std::to_string(got) + " stops=" + std::to_string(r.stops.size()) + (r.stops.empty() ? "" : " line=" + std::to_string(r.stops.back().line) + " pc=" + std::to_string(r.stops.back().pc) + " reason=" + std::to_string((int)r.reasons.back())));
        r.dbg.restart();
        r.disp->pumpUntil([&] { return r.dbg.location(Domain::Main).valid && r.dbg.location(Domain::Main).pc == 0 && r.ses.view() && r.ses.view()->cycle == 0; }, 3000);
        check(r.ses.view() && r.ses.view()->cycle == 0, "restart goes back to power-on");
        r.dbg.toggleLineBreakpoint("/p/a.asm", 4);
        r.dbg.setCondition("/p/a.asm", 4, "csr[0x50] == 99");
        r.stops.clear();
        r.dbg.run();
        check(r.waitStop(1) && r.reasons.back() == asmcore::sim::StopReason::Idle, "a breakpoint with a false condition does not stop");
        r.dbg.restart();
        r.dbg.setCondition("/p/a.asm", 4, "");
        r.dbg.setEnabled("/p/a.asm", 4, false);
        r.stops.clear();
        r.dbg.run();
        check(r.waitStop(1) && r.reasons.back() == asmcore::sim::StopReason::Idle && r.dbg.hasLineBreakpoint("/p/a.asm", 4), "a disabled breakpoint stays in the list but does not stop");
    }
    {   // lines shifting, persistence
        DebugRig r(prog);
        r.dbg.toggleLineBreakpoint("/p/a.asm", 4);
        r.dbg.toggleLineBreakpoint("/p/a.asm", 7);
        r.dbg.setCondition("/p/a.asm", 7, "pc(main) > 1");
        r.dbg.linesShifted("/p/a.asm", 5, 2);
        check(r.dbg.hasLineBreakpoint("/p/a.asm", 4) && r.dbg.hasLineBreakpoint("/p/a.asm", 9) && !r.dbg.hasLineBreakpoint("/p/a.asm", 7), "inserting two lines above line 7 moves that breakpoint to 9; the one above stays");
        r.dbg.linesShifted("/p/a.asm", 1, -3);
        check(!r.dbg.hasLineBreakpoint("/p/a.asm", 4) && r.dbg.hasLineBreakpoint("/p/a.asm", 1), "deleting lines moves breakpoints up (and one that falls above line 1 is dropped)", std::to_string(r.dbg.lineBreakpoints().size()));
        r.dbg.linesShifted("/p/a.asm", 0, 3);
        r.dbg.saveBreakpoints();
        Debugger& d = r.dbg;
        d.clearLineBreakpoints();
        check(d.lineBreakpoints().empty(), "clear");
        d.loadBreakpoints();
        auto lb = d.lineBreakpoints();
        check(lb.size() == 2 && lb[0].line == 4 && lb[1].line == 9 && lb[1].condition == "pc(main) > 1", "breakpoints, with conditions, are saved per project and loaded again", std::to_string(lb.size()));
    }
    {   // environment and profile files of the project are used; a missing one is reported
        DebugRig r(prog, "environment = e.env\n");
        std::string why;
        check(!r.dbg.start(why) && why.find("environment") != std::string::npos, "a missing environment file is reported", why);
        r.fs->write("/p/e.env", "[csr X]\nkind = banana\n");
        check(r.dbg.start(why), "start with an environment file");
        r.disp->pumpUntil([&] { return r.ses.state() == SimState::Failed; }, 3000);
        check(r.ses.state() == SimState::Failed, "an environment file with errors fails the session");
        r.fs->write("/p/e.env", "");
        DebugRig q(prog, "environment = e.env\n");
        q.fs->write("/p/e.env", "");
        check(q.begin(why), "an empty environment file is fine", why);
        q.ws.setText("/p/e.env", "[csr X]\nkind = banana\n");
        q.dbg.stop();
        check(q.dbg.start(why), "an unsaved edit of the environment file is what gets used");
        q.disp->pumpUntil([&] { return q.ses.state() == SimState::Failed; }, 3000);
        check(q.ses.state() == SimState::Failed, "...(the unsaved text has the error)");
    }
}

static void t_session() {
    std::printf("\nSimulator session\n");
    using namespace asmcore;
    using namespace asmcore::sim;
    {
        SessionRig r;
        check(r.loadSource(kFinite) && r.loadedOk, "load a program: the simulator starts on the worker thread", r.loadErrors.empty() ? "" : r.loadErrors[0]);
        const SimUpdate* u = r.waitFor([](const SimUpdate& x) { return x.view && x.state == SimState::Paused; });
        check(u && u->view->pc[0] == 0 && u->view->cycle == 0 && u->view->regions.count("scratch") && !u->view->regions.count("const") && u->eventsReset, "the first view: pc 0, regions present, events reset");
        check(u && u->view->csrs.size() == 1 && u->view->csrs[0].first == "MARK", "the program's .csr names are listed", u ? std::to_string(u->view->csrs.size()) : "");
        r.clear();
        r.ses.step(Domain::Main);
        u = r.waitStop();
        check(u && u->stop.reason == StopReason::InstrLimit && u->view->pc[0] == 2 && u->view->csrs[0].second == 7, "step: one instruction, state updated", u ? std::to_string(u->view->pc[0]) : "none");
        check(u && !u->events.empty(), "events of the step arrive with the update");
        r.clear();
        r.ses.stepOver(Domain::Main);
        u = r.waitStop();
        check(u && u->stop.reason == StopReason::InstrLimit && u->view->csrs[0].second == 8, "stepOver");
        r.clear();
        r.ses.run();
        u = r.waitStop();
        check(u && u->stop.reason == StopReason::Idle && u->state == SimState::Paused && u->view->halted[0], "run to the end of a finite program: Idle, halted");
        check(r.ses.state() == SimState::Paused && r.ses.view() && r.ses.view()->halted[0], "the owner-thread mirror follows the updates");
        r.clear();
        r.ses.reset();
        u = r.waitFor([](const SimUpdate& x) { return x.eventsReset; });
        check(u && u->view->pc[0] == 0 && u->view->cycle == 0 && u->view->csrs[0].second == 0, "reset: power-on state and the event list starts afresh");
    }
    {   // breakpoints
        SessionRig r;
        r.loadSource(kFinite);
        r.waitFor([](const SimUpdate& x) { return x.view != nullptr; });
        r.ses.setBreakpoints({BreakpointSpec{Domain::Main, 2, ""}});
        r.clear();
        r.ses.run();
        const SimUpdate* u = r.waitStop();
        check(u && u->stop.reason == StopReason::Breakpoint && u->view->pc[0] == 2 && u->view->breakpoints.size() == 1 && u->view->breakpoints[0].hits == 1 && u->stop.id == u->view->breakpoints[0].id,
              "a breakpoint stops the run; its id and hit count are in the view");
        r.ses.setBreakpoints({});
        r.clear();
        r.ses.run();
        u = r.waitStop();
        check(u && u->stop.reason == StopReason::Idle, "after removing it the run goes on");
        r.ses.reset();
        r.ses.setBreakpoints({BreakpointSpec{Domain::Main, 2, "csr[0x50] == 99"}});
        r.clear();
        r.ses.run();
        u = r.waitStop();
        check(u && u->stop.reason == StopReason::Idle, "a conditional breakpoint whose condition is false does not stop");
        r.ses.reset();
        r.ses.runToPc(Domain::Main, 2);
        r.clear();
        u = r.waitStop();
        check(u && u->stop.reason == StopReason::Breakpoint && u->view->pc[0] == 2 && u->view->breakpoints.size() == 1, "runToPc stops there and leaves only the user's breakpoints", u ? std::to_string(u->view->breakpoints.size()) : "");
    }
    {   // run-away protection
        SessionRig r;
        r.loadSource(kEndless);
        r.waitFor([](const SimUpdate& x) { return x.view != nullptr; });
        RunSpec cyc;
        cyc.maxCycles = 5000;
        cyc.stopOnIdle = false;
        r.clear();
        r.ses.run(cyc);
        const SimUpdate* u = r.waitStop();
        check(u && u->stop.reason == StopReason::CycleLimit, "the cycle budget ends an endless program", u ? std::to_string((int)u->stop.reason) : "none");
        RunSpec wall;
        wall.maxCycles = 0;
        wall.maxMillis = 150;
        wall.stopOnIdle = false;
        r.clear();
        auto t0 = std::chrono::steady_clock::now();
        r.ses.run(wall);
        u = r.waitStop();
        long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(u && u->stop.reason == StopReason::TimeLimit && ms < 3000, "the wall-clock budget ends it too", std::to_string(ms) + " ms");
        // pause
        RunSpec forever;
        forever.maxCycles = 0;
        forever.maxMillis = 0;
        forever.stopOnIdle = false;
        r.clear();
        r.ses.run(forever);
        const SimUpdate* running = r.waitFor([](const SimUpdate& x) { return x.state == SimState::Running && x.view && x.view->cycle > 0; });
        check(running != nullptr, "while running, fresh states keep arriving (the UI can show progress)");
        size_t before = r.ups.size();
        r.disp->pumpUntil([&] { return r.ups.size() > before + 2; }, 3000);
        check(r.ups.size() > before + 2, "...several of them");
        r.ses.pause();
        u = r.waitStop(3000);
        check(u && u->stop.reason == StopReason::Cancelled && u->state == SimState::Paused, "pause() interrupts an unlimited run");
        r.clear();
        r.ses.step(Domain::Main);
        u = r.waitStop();
        check(u && u->stop.reason == StopReason::InstrLimit, "the session is usable again after a pause");
    }
    {   // watch expressions, pokes, injection, signals
        SessionRig r;
        r.loadSource(kFinite);
        r.waitFor([](const SimUpdate& x) { return x.view != nullptr; });
        r.ses.setWatchExprs({"csr[0x50]", "scratch[0] +", "pc(main)"});
        const SimUpdate* u = r.waitFor([](const SimUpdate& x) { return x.view && x.view->watches.size() == 3; });
        check(u && u->view->watches[0].ok && u->view->watches[0].value == 0 && !u->view->watches[1].ok && !u->view->watches[1].error.empty(), "watch expressions: a value, and a bad expression with its error");
        r.clear();
        r.ses.writeRegion("scratch", 4, {0xAB, 0xCD});
        u = r.waitFor([](const SimUpdate& x) { return x.view && x.view->regions.count("scratch") && x.view->regions.at("scratch")[4] == 0xAB; });
        check(u && u->view->regions.at("scratch")[5] == 0xCD, "poke a region");
        r.clear();
        r.ses.writeRegion("nonsense", 0, {1});
        u = r.waitFor([](const SimUpdate& x) { return !x.messages.empty(); });
        check(u != nullptr, "a bad poke is reported as a message");
        r.clear();
        r.ses.command("csr 0x50=0x42");
        u = r.waitFor([](const SimUpdate& x) { return x.view && x.view->csrs.size() == 1 && x.view->csrs[0].second == 0x42; });
        check(u != nullptr, "an injected CSR write takes effect at once");
        r.clear();
        r.ses.command("frobnicate");
        u = r.waitFor([](const SimUpdate& x) { return !x.messages.empty(); });
        check(u != nullptr, "a bad command is reported");
        r.clear();
        r.ses.setDataWatches({"scratch[8..9]", "not an expression ("});
        u = r.waitFor([](const SimUpdate& x) { return !x.messages.empty(); });
        check(u != nullptr, "a bad data watch is reported while a good one is kept");
    }
    {   // trace export and rewind
        SessionRig r;
        r.loadSource(kEndless);
        r.waitFor([](const SimUpdate& x) { return x.view != nullptr; });
        RunSpec spec;
        spec.maxCycles = 60000;
        spec.stopOnIdle = false;
        r.clear();
        r.ses.run(spec);
        r.waitStop();
        std::string text, vcd, csv;
        bool d1 = false, d2 = false, d3 = false;
        r.ses.exportTrace("text", [&](const std::string& s) { text = s; d1 = true; });
        r.ses.exportTrace("vcd", [&](const std::string& s) { vcd = s; d2 = true; });
        r.ses.exportTrace("csv", [&](const std::string& s) { csv = s; d3 = true; });
        r.disp->pumpUntil([&] { return d1 && d2 && d3; }, 5000);
        check(d1 && !text.empty() && d3 && !csv.empty(), "trace export: text and csv");
        check(d2 && vcd.find("$timescale") != std::string::npos, "trace export: vcd", vcd.substr(0, 40));
        r.clear();
        r.ses.rewindTo(20000);
        const SimUpdate* u = r.waitFor([](const SimUpdate& x) { return x.eventsReset; });
        check(u && u->view->cycle == 20000, "rewind to an earlier cycle", u ? std::to_string(u->view->cycle) : "none");
    }
    {   // bad inputs
        SessionRig r;
        r.ses.load(buildSource(".main\nx: JMP nowhere\n"), "", asmcore::Profile());
        r.disp->pumpUntil([&] { return r.loadedCount > 0; }, 3000);
        check(r.loadedCount == 1 && !r.loadedOk && !r.loadErrors.empty() && r.ses.state() == SimState::Failed, "a program with errors cannot be simulated");
        SessionRig e;
        e.ses.load(buildSource(kFinite), "[csr X]\nkind = banana\n", asmcore::Profile());
        e.disp->pumpUntil([&] { return e.loadedCount > 0; }, 3000);
        check(e.loadedCount == 1 && !e.loadedOk && !e.loadErrors.empty(), "an environment file with errors is reported", e.loadErrors.empty() ? "" : e.loadErrors[0]);
        e.ses.run();                                            // commands without a simulator are ignored
        e.ses.step(asmcore::Domain::Main);
        e.disp->runPending();
        check(true, "(commands without a simulator are ignored)");
    }
    {   // destroying a session in the middle of an unlimited run
        auto disp = std::make_shared<QueueDispatcher>();
        {
            SimSession ses(disp);
            ses.load(buildSource(kEndless), "", asmcore::Profile());
            RunSpec forever;
            forever.maxCycles = 0;
            forever.maxMillis = 0;
            forever.stopOnIdle = false;
            ses.run(forever);
            disp->pumpUntil([&] { return ses.state() == SimState::Running; }, 2000);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        disp->runPending();
        check(true, "destroying a session that is running returns promptly and leaves nothing behind");
    }
}

int main() {
    t_realfs();
    t_paths();
    t_project();
    t_workspace();
    t_build();
    t_block_link();
    t_hardware_link();
    t_samples();
    t_service();
    t_destroy();
    t_settings();
    t_editing();
    t_session();
    t_debugger();
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
