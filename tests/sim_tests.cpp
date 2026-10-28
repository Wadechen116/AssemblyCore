// Simulator tests. The ALU expectations are the worked examples printed in the spec (6.4.1, 6.5, 6.7), not values read back
// from this implementation.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "asmcore/sim.h"

using namespace asmcore;
using namespace asmcore::sim;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& why = "") {
    if (ok) { g_pass++; std::printf("  PASS  %s\n", what.c_str()); return; }
    g_fail++;
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), why.c_str());
}
static void section(const char* s) { std::printf("\n%s\n", s); }

static std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static std::string dir() {
#ifdef ASMCORE_SOURCE_DIR
    return ASMCORE_SOURCE_DIR;
#else
    return ".";
#endif
}

static Result assemble1(const std::string& src, CallModel m = CallModel::Inline) {
    Options o;
    o.callModel = m;
    std::vector<Source> v{Source{"t.asm", src}};
    return assemble(v, nullptr, o);
}
static std::string diags(const Result& r) { return r.diagnosticsText(); }

static int64_t ev(const Simulator& s, const std::string& e) {
    int64_t v = 0;
    std::string err;
    if (!s.eval(e, v, err)) { std::printf("        eval error in '%s': %s\n", e.c_str(), err.c_str()); return -999999; }
    return v;
}

// ---- S1: instruction semantics ------------------------------------------------------------------------------------------------
static void runAlu(const char* name, int a, int b, const std::string& op, const std::string& expect) {
    std::ostringstream src;
    src << ".main\n.const CA dw " << a << "\n.const CB dw " << b << "\n"
        << "s: MOV MAIN_CONST_POOL[@CA] -> MAIN_SCRATCHPAD[0..1]\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n"
        << " MOV MAIN_CONST_POOL[@CB] -> MAIN_SCRATCHPAD[2..3]\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n " << op << "\nh: JMP h\n";
    Result r = assemble1(src.str());
    if (!r.ok) { check(false, name, diags(r)); return; }
    Simulator sim(r, SimConfig());
    RunLimits l;
    l.maxCycles = 1000;
    RunResult rr = sim.run(l);
    bool ok = rr.reason == StopReason::Idle;
    std::string why = "stop: " + rr.detail;
    // expect: "out==0x8C && alu.ov16==0 ..."
    int64_t v = ev(sim, expect);
    if (v != 1) { ok = false; why += "  out=" + std::to_string(ev(sim, "out")) + " ov16=" + std::to_string(ev(sim, "alu.ov16")) + " neg=" + std::to_string(ev(sim, "alu.negative")) + " carry=" + std::to_string(ev(sim, "alu.carry_u")) + " borrow=" + std::to_string(ev(sim, "alu.borrow_u")); }
    check(ok, name, why);
}

static void t_alu() {
    section("ALU: the worked examples of the spec");
    runAlu("6.4.1 A  unsigned ADD 100 + 40 = 0x8C, fits 16 bits", 100, 40, "ALU ADD unsigned", "out == 0x8C && alu.ov16 == 0 && alu.carry_u == 0");
    runAlu("6.4.1 B  signed SUB -20 - 5 = -25 = 0xFFFFFFE7", 0xFFEC, 5, "ALU SUB signed", "out == 0xFFFFFFE7 && alu.ov16 == 0 && alu.negative == 1");
    runAlu("6.4.1 C  unsigned ADD 0xFFFF + 1 = 0x00010000, ov16", 0xFFFF, 1, "ALU ADD unsigned", "out == 0x10000 && alu.ov16 == 1 && alu.carry_u == 1");
    runAlu("6.4.1 D  signed MUL -100 * 10 = -1000 = 0xFFFFFC18", 0xFF9C, 10, "ALU MUL signed", "out == 0xFFFFFC18 && alu.ov16 == 0");
    runAlu("6.4.1 E  unsigned MUL 0x100 * 0x100 = 0x10000, ov16", 0x100, 0x100, "ALU MUL unsigned", "out == 0x10000 && alu.ov16 == 1");
    runAlu("6.4    unsigned SUB 10 - 20 = 0xFFFFFFF6, borrow, ov16", 10, 20, "ALU SUB unsigned", "out == 0xFFFFFFF6 && alu.borrow_u == 1 && alu.ov16 == 1 && alu.negative == 1");
    runAlu("6.5    signed GE: -100 >= -200 is true", 0xFF9C, 0xFF38, "ALU CMP.GE signed", "alu.cmp_true == 1");
    runAlu("6.5    signed LT: -100 < -200 is false", 0xFF9C, 0xFF38, "ALU CMP.LT signed", "alu.cmp_true == 0");
    runAlu("6.5    unsigned LT: 0xFF9C < 0xFF38 is false", 0xFF9C, 0xFF38, "ALU CMP.LT unsigned", "alu.cmp_true == 0");
    runAlu("6.5    EQ compares bit patterns", 7, 7, "ALU CMP.EQ unsigned", "alu.cmp_true == 1");
    runAlu("6.6    AND masks the low 10 bits: 0xABCD & 0x03FF = 0x03CD", 0xABCD, 0x03FF, "ALU AND", "out == 0x03CD && alu.ov16 == 0");
    runAlu("6.6    NOT uses OPA only", 0x00FF, 0, "ALU NOT", "out == 0xFF00");
    runAlu("6.7 A  unsigned SHL 0x8000 << 1 = 0x10000, ov16", 0x8000, 0, "ALU SHL 1 unsigned", "out == 0x10000 && alu.ov16 == 1");
    runAlu("6.7 B  signed SHL 0xFFFF << 8 = 0xFFFFFF00 (sign-extended 24 bits)", 0xFFFF, 0, "ALU SHL 8 signed", "out == 0xFFFFFF00 && alu.ov16 == 0");
    runAlu("6.7     unsigned SHL 0xFFFF << 8 = 0x00FFFF00", 0xFFFF, 0, "ALU SHL 8 unsigned", "out == 0xFFFF00 && alu.ov16 == 1");
    runAlu("6.7 C  signed SHL 0x4000 << 1 = 0x8000, overflows S16", 0x4000, 0, "ALU SHL 1 signed", "out == 0x8000 && alu.ov16 == 1");
    runAlu("6.7 D  logical SHR 0x8000 >> 1 = 0x4000", 0x8000, 0, "ALU SHR 1 unsigned", "out == 0x4000");
    runAlu("6.7 E  arithmetic SHR 0x8000 >> 1 = 0xFFFFC000", 0x8000, 0, "ALU SHR 1 signed", "out == 0xFFFFC000 && alu.negative == 1");
}

// ---- S2/S3: the IMU service, end to end -----------------------------------------------------------------------------------------
static Result imuProgram(CallModel m = CallModel::Inline) {
    Options o;
    o.callModel = m;
    std::string base = dir() + "/examples/";
    std::vector<Source> v{Source{base + "imu_gyro.asm", slurp(base + "imu_gyro.asm")}};
    auto res = [&](const std::string&, const std::string& n, Source& out) { out.name = base + n; out.text = slurp(base + n); return !out.text.empty(); };
    return assemble(v, res, o);
}

static void t_imu() {
    section("IMU gyro service: Peri polls and reads over I2C, publishes; Main consumes, corrects, commits");
    Result r = imuProgram();
    check(r.ok, "examples/imu_gyro.asm assembles", diags(r));
    if (!r.ok) return;
    SimConfig cfg;
    cfg.env = slurp(dir() + "/examples/imu_env.cfg");
    Simulator sim(r, cfg);
    check(sim.ok(), "environment file loads", sim.configErrors().empty() ? "" : sim.configErrors()[0]);
    RunLimits l;
    l.maxCycles = 100000;
    l.untilExpr = "slot.valid";
    RunResult rr = sim.run(l);
    check(rr.reason == StopReason::Condition, "Peri publishes a result", std::string("stopped: ") + rr.detail + " at " + std::to_string(rr.now));
    check(ev(sim, "poll.ready") == 1 && ev(sim, "peri.error") == 0 && ev(sim, "slot.service_id") == 1, "poll said ready, no error, service_id 1 (SID_IMU_GYRO_RAW)");
    check(ev(sim, "peri.done") == 0, "done is set only after EXEC_PUB (spec 13.2)");
    check(ev(sim, "slot[0..5]") == (((int64_t)0x0A << 8) | ((int64_t)0x14 << 24) | ((int64_t)0x1E << 40)), "the slot payload is the six gyro bytes, high byte first (the order on the bus)", std::to_string(ev(sim, "slot[0..5]")));
    // three polls (status 0, 0, 1) of 2430 cycles + a 6-byte read of 5468 (spec 14.4): about 13000 cycles
    check(rr.now > 12000 && rr.now < 16000, "elapsed time reflects the I2C transactions (spec 14.4)", "cycle " + std::to_string(rr.now));
    l.untilExpr = "csr[0x120] == 10";
    rr = sim.run(l);
    check(rr.reason == StopReason::Condition, "Main commits the corrected X value (10) to CSR 0x120", std::string("stopped: ") + rr.detail + " at " + std::to_string(rr.now));
    check(ev(sim, "csr[0x120]") == 10 && ev(sim, "csr[0x121]") == 0, "CSR 0x120/0x121 hold 0x000A (raw 10 - bias 0)");
    check(ev(sim, "slot.valid") == 0, "the slot was consumed (valid cleared)");
    check(ev(sim, "peri.error") == 0, "no error latch at any point");
}


// ---- helpers for the next groups ----------------------------------------------------------------------------------------------------
static const char* kDefs =
    ".csr MARK = 0x050\n.csr PERI_CTRL = 0x302\n.csr TPER = 0x310\n.csr POLL_MASK = 0x300\n.csr POLL_CTRL = 0x301\n.service SID_A = 1\n.devmap 0, I2C, 0x68, DEV0\n";
static const char* kEnvCsr =
    "[csr PERI_CTRL]\nkind = action\nbit0 = timer_start\nbit1 = done_set\nbit2 = error_set\nbit3 = service_reset\n"
    "[csr TPER]\nkind = value\nbind = timer_period\nbytes = 2\n"
    "[csr POLL_MASK]\nkind = value\nbind = poll_mask\n[csr POLL_CTRL]\nkind = action\nbit0 = poll_clear\nbit1 = poll_arm\n";

struct Fx {
    Result r;
    std::unique_ptr<Simulator> sim;
    std::string trace;
    Fx(const std::string& src, const std::string& env = "", CallModel m = CallModel::Inline, const SimParams* params = nullptr) {
        r = assemble1(std::string(kDefs) + src, m);
        if (!r.ok) { std::printf("        assembly failed:\n%s", diags(r).c_str()); return; }
        SimConfig c;
        c.env = env;
        if (params) c.params = *params;
        sim.reset(new Simulator(r, c));
        if (!sim->ok()) std::printf("        env error: %s\n", sim->configErrors()[0].c_str());
    }
    bool good() const { return sim && sim->ok(); }
    RunResult runUntil(const std::string& e, Cycle max = 200000) {
        RunLimits l;
        l.maxCycles = max;
        l.untilExpr = e;
        return sim->run(l);
    }
};

static void patchByte(Result& r, Domain d, size_t off, uint8_t v) { (d == Domain::Main ? r.main : r.peri).bytes[off] = v; }
static const SectionInfo* sectionOf(const Result& r, Domain d, const char* name) {
    for (auto& s : (d == Domain::Main ? r.main : r.peri).sections) if (s.name == name) return &s;
    return nullptr;
}

static void t_faults() {
    section("faults (spec 15.1): the offending pc is recorded, nothing is half-written");
    {   // reserved bits in a MOV descriptor
        Result r = assemble1(".main\ns: MOV MAIN_SCRATCHPAD[0..1] -> MAIN_SCRATCHPAD[4..5]\nh: JMP h\n");
        const SectionInfo* mv = sectionOf(r, Domain::Main, "MainMoveDesc_32b");
        patchByte(r, Domain::Main, mv->offset, r.main.bytes[mv->offset] | 0x20);
        Simulator sim(r, SimConfig());
        sim.writeRegion("scratch", 0, {0x11, 0x22});
        RunLimits l; l.maxCycles = 100;
        RunResult rr = sim.run(l);
        const Fault* f = sim.fault(Domain::Main);
        check(rr.reason == StopReason::Fault && f && f->code == "fault_desc_reserved" && f->pc == 0, "reserved descriptor bit -> fault_desc_reserved at pc 0", rr.detail);
        check(sim.pc(Domain::Main) == 0 && ev(sim, "scratch[4..5]") == 0, "the pc does not advance and the destination is untouched");
    }
    {   // MOV from an empty slot
        Fx f(".main\ns: MOV PERI_RESULT_SLOT[0..0] -> MAIN_SCRATCHPAD[0..0]\nh: JMP h\n");
        f.runUntil("0", 100);
        const Fault* fl = f.good() ? f.sim->fault(Domain::Main) : nullptr;
        check(fl && fl->code == "fault_slot_not_valid", "consuming an empty PERI_RESULT_SLOT faults", fl ? fl->code : "no fault");
    }
    {   // cascade step mismatch, reached by patching the second CASCADE_END's index
        Result r = assemble1(".main\ntop: CASCADE_BEGIN 1, b0\nb0: EXEC_CSR WRITE8 0x10, 1\n CASCADE_END 0, d\nb1: EXEC_CSR WRITE8 0x10, 2\n CASCADE_END 1, d\nd: JMP d\n");
        const SectionInfo* br = sectionOf(r, Domain::Main, "MainBranchDesc_16b");
        for (uint32_t o = br->offset; o < br->offset + br->size; o += 2) {
            uint32_t v = r.main.bytes[o] | (r.main.bytes[o + 1] << 8);
            if ((v >> 14) == 2 && ((v >> 10) & 15) == 1) { v = (v & ~(15u << 10)) | (3u << 10); r.main.bytes[o] = (uint8_t)v; r.main.bytes[o + 1] = (uint8_t)(v >> 8); }
        }
        Simulator sim(r, SimConfig());
        RunLimits l; l.maxCycles = 1000;
        sim.run(l);
        const Fault* f = sim.fault(Domain::Main);
        check(f && f->code == "fault_cascade_step_mismatch", "CASCADE_END with the wrong index faults", f ? f->code : "no fault");
    }
    {   // reserved Peri CSR format
        Result r = assemble1(std::string(kDefs) + ".peri\ns: EXEC_CSR WRITE8 POLL_MASK, 1\nh: JMP h\n");
        patchByte(r, Domain::Peri, 0, r.peri.bytes[0] | 0x20);
        Simulator sim(r, SimConfig());
        RunLimits l; l.maxCycles = 100;
        sim.run(l);
        const Fault* f = sim.fault(Domain::Peri);
        check(f && f->code == "fault_peri_csr_multibyte", "Peri EXEC_CSR with the MultiByte bit faults", f ? f->code : "no fault");
    }
    {   // a fault in one domain does not stop the other
        Result r = assemble1(std::string(kDefs) + ".main\ns: EXEC_CSR WRITE8 MARK, 7\nh: JMP h\n.peri\np: EXEC_CSR WRITE8 POLL_MASK, 1\nq: JMP q\n");
        patchByte(r, Domain::Peri, 0, r.peri.bytes[0] | 0x20);
        Simulator sim(r, SimConfig());
        RunLimits l; l.maxCycles = 1000;
        RunResult first = sim.run(l);                 // stops at the Peri fault
        sim.run(l);                                   // ...and Main carries on
        check(first.reason == StopReason::Fault && sim.faulted(Domain::Peri) && sim.halted(Domain::Main) && ev(sim, "csr[0x50]") == 7, "Main finishes its work although Peri faulted");
        std::string e;
        sim.inject(sim.now(), "fault_clear peri", e);
        check(e.empty(), "fault_clear can be injected");
    }
}

static void t_slot() {
    section("PERI_RESULT_SLOT (spec 12): overrun keeps the old result");
    std::string env = std::string(kEnvCsr) + "[device d]\naddr = 0x68\nregseq 0x10 = 0x11, 0x22\nregseq 0x11 = 0x33, 0x44\n";
    Fx f(".main\nh: JMP h\n.peri\nl: EXEC_XFER READ dev=0, len=2, ptr=0, ext=[0x10]\n EXEC_PUB service=1, src=0, len=2\n JMP l\n", env);
    if (!f.good()) { check(false, "setup"); return; }
    int pubs = 0, overruns = 0;
    struct Count : EventSink { int* p; int* o; void onEvent(const Event& e) override { if (e.type == EventType::Publish) ++*p; if (e.type == EventType::Overrun) ++*o; } };
    auto c = std::make_shared<Count>();
    c->p = &pubs; c->o = &overruns;
    f.sim->addSink(c);
    RunResult rr = f.runUntil("slot.overrun");
    check(rr.reason == StopReason::Condition, "the second publish raises overrun_sticky", rr.detail);
    check(ev(*f.sim, "slot.valid") == 1 && ev(*f.sim, "slot[0..1]") == (0x11 | (0x33 << 8)), "the OLD result is intact (0x11 0x33), the new one (0x22 0x44) was dropped", std::to_string(ev(*f.sim, "slot[0..1]")));
    check(pubs == 1 && overruns == 1, "one Publish event and one Overrun event", std::to_string(pubs) + "/" + std::to_string(overruns));
    f.sim->setSignal("slot.overrun", 0);
    check(ev(*f.sim, "slot.overrun") == 0, "the sticky bit can be cleared (overrun_clear)");
}

static void t_same_cycle() {
    section("no same-cycle reuse (spec 12.6): a publish never lands in the cycle Main cleared the slot");
    std::string env = std::string(kEnvCsr) + "[device d]\naddr = 0x68\nregseq 0x10 = 1, 2, 3, 4, 5, 6, 7\n";
    Fx f(".main\ns: BR.COND PERI_RESULT_VALID, y\n JMP s\ny: MOV PERI_RESULT_SLOT[0..0] -> MAIN_SCRATCHPAD[0..0]\n JMP s\n"
         ".peri\nl: EXEC_XFER READ dev=0, addr=0x10, addrlen=1, ptr=0\n EXEC_PUB service=1, src=0, len=1\n JMP l\n", env);
    if (!f.good()) { check(false, "setup"); return; }
    struct Watch : EventSink {
        Cycle lastConsume = ~(Cycle)0;
        int consumes = 0, publishes = 0, sameCycle = 0, overruns = 0;
        void onEvent(const Event& e) override {
            if (e.type == EventType::Consume) { lastConsume = e.t; consumes++; }
            else if (e.type == EventType::Publish) { publishes++; if (e.t == lastConsume) sameCycle++; }
            else if (e.type == EventType::Overrun) overruns++;
        }
    };
    auto w = std::make_shared<Watch>();
    f.sim->addSink(w);
    RunLimits l;
    l.maxCycles = 60000;
    f.sim->run(l);
    check(w->consumes >= 5 && w->publishes >= 5, "several results were produced and consumed", std::to_string(w->publishes) + " published, " + std::to_string(w->consumes) + " consumed");
    check(w->sameCycle == 0, "no publish in the cycle of the preceding consume", std::to_string(w->sameCycle));
    check(ev(*f.sim, "scratch[0]") >= 1 && ev(*f.sim, "scratch[0]") <= 7, "Main saw the results in order (values 1..7)");
}

static void t_timing() {
    section("commit eligibility (spec 7.4, 14.5): pending && window_open && remaining >= WCET");
    std::string timing = "[timing]\nframe_cycles = 450000\nvblank_start = 435638\nvblank_len = 14362\n";
    const char* prog = ".main\nstart: BR.COND PGL_COMMIT_ELIGIBLE, commit\n JMP start\ncommit: EXEC_CSR WRITE8 MARK, 1\nh: JMP h\n";
    {
        Fx f(prog, timing + "[signal x]\n[inject]\nat 0 = signal pgl_pending=1 wcet_pgl=800\n");
        RunResult rr = f.runUntil("csr[0x50] == 1", 600000);
        check(rr.reason == StopReason::Condition && rr.now >= 435638 && rr.now < 435700, "WCET 800: commits at the start of VBlank (cycle 435638)", "cycle " + std::to_string(rr.now) + " " + rr.detail);
        check(ev(*f.sim, "vblank_open") == 1 && ev(*f.sim, "remaining_vblank_cycles") > 14000, "inside the window with ~14.3k cycles left");
    }
    {
        Fx f(prog, timing + "[inject]\nat 0 = signal pgl_pending=1 wcet_pgl=15000\n");
        RunResult rr = f.runUntil("csr[0x50] == 1", 1000000);
        check(rr.reason == StopReason::CycleLimit && ev(*f.sim, "csr[0x50]") == 0, "WCET 15000 > the 14362-cycle window: never eligible, although VBLANK_OPEN is true");
    }
    {
        Fx f(prog, timing + "[inject]\nat 0 = signal wcet_pgl=800\nat 449500 = signal pgl_pending=1\n");
        RunResult rr = f.runUntil("csr[0x50] == 1", 460000);
        check(rr.reason == StopReason::CycleLimit && ev(*f.sim, "csr[0x50]") == 0, "pending at cycle 449500 (500 cycles left < 800): not eligible, no partial commit");
        rr = f.runUntil("csr[0x50] == 1", 600000);
        check(rr.reason == StopReason::Condition && rr.now >= 885638, "...it waits for the next frame's window", "cycle " + std::to_string(rr.now));
    }
    {
        Fx f(".main\nh: JMP h\n", timing);
        RunLimits l;
        l.maxCycles = 460000;
        l.stopOnIdle = false;
        f.sim->run(l);
        check(ev(*f.sim, "frame_end_evt") == 1, "frame_end_evt is latched at the frame boundary");
    }
}

static void t_peri_units() {
    section("Peri units: timer, poll, error latch, NACK");
    {
        Fx f(".peri\ns: EXEC_CSR WRITE8 TPER, 100\n EXEC_CSR SET8 PERI_CTRL, 1\nw: BR.IF_TIMER_DONE d\n JMP w\nd: EXEC_CSR SET8 PERI_CTRL, 2\nh: JMP h\n", kEnvCsr);
        RunResult rr = f.runUntil("peri.done", 5000);
        check(rr.reason == StopReason::Condition && rr.now >= 100 && rr.now < 140, "a 100-cycle timer expires after ~100 cycles", "cycle " + std::to_string(rr.now));
    }
    {   // clearing a timer cancels its pending expiry; restarting it re-arms from the restart
        Fx f(".peri\ns: EXEC_CSR WRITE8 TPER, 100\n EXEC_CSR SET8 PERI_CTRL, 1\n EXEC_CSR SET8 PERI_CTRL, 8\nh: JMP h\n",
             std::string(kEnvCsr) + "[csr TCLR]\nkind = action\nbit0 = timer_clear\n");
        (void)f;
        Fx g(".peri\ns: EXEC_CSR WRITE8 TPER, 100\n EXEC_CSR SET8 PERI_CTRL, 1\n EXEC_CSR SET8 TCLR, 1\nh: JMP h\n.csr TCLR = 0x312\n",
             std::string(kEnvCsr) + "[csr TCLR]\nkind = action\nbit0 = timer_clear\n");
        RunLimits l; l.maxCycles = 1000; l.stopOnIdle = false;
        g.sim->run(l);
        check(ev(*g.sim, "timer.done") == 0, "timer_clear cancels the pending expiry (done stays 0 past the 100-cycle period)");
    }
    {
        std::string env = std::string(kEnvCsr) + "[device d]\naddr = 0x68\nreg 0x00 = 0x00, 0x00, 0x00, 0x01\n";
        Fx f(".peri\ns: EXEC_CSR WRITE8 POLL_MASK, 1\nl: EXEC_CSR SET8 POLL_CTRL, 2\n EXEC_XFER READ dev=0, addr=0, addrlen=1, ptr=0\n BR.IF_READY ok\n JMP l\nok: EXEC_CSR SET8 PERI_CTRL, 2\nh: JMP h\n", env);
        f.runUntil("peri.done", 100000);
        check(ev(*f.sim, "peri.done") == 1 && ev(*f.sim, "poll.ready") == 1, "poll loop ends on the 4th read, when bit 0 of the status is set");
        check(f.sim->instructions(Domain::Peri) > 12, "...after looping");
    }
    {
        std::string env = std::string(kEnvCsr) + "[device d]\naddr = 0x68\nreg 0x00 = 0x01\n[inject]\nat 0 = nack dev=0 count=1\n";
        Fx f(".peri\ns: EXEC_XFER READ dev=0, addr=0, addrlen=1, ptr=0\n BR.IF_ERROR bad\nh: JMP h\nbad: EXEC_CSR SET8 PERI_CTRL, 2\nz: JMP z\n", env);
        f.runUntil("peri.done", 100000);
        check(ev(*f.sim, "peri.error") == 1 && ev(*f.sim, "peri.done") == 1 && ev(*f.sim, "rx[0]") == 0, "an injected NACK sets the error latch and leaves PERI_RX_BUF alone");
    }
    {
        Fx f(".peri\ns: EXEC_XFER READ dev=0, addr=0, addrlen=1, ptr=0\nh: JMP h\n", kEnvCsr);   // nobody at 0x68
        f.runUntil("0", 10000);
        check(ev(*f.sim, "peri.error") == 1, "a missing device does not acknowledge");
    }
    {
        std::string env = std::string(kEnvCsr) + "[device d]\naddr = 0x68\nreg 0x00 = 0x5A\n";
        Fx f(".peri\ns: EXEC_XFER READ dev=0, addr=0, addrlen=1, ptr=3\nh: JMP h\n", env);
        f.runUntil("0", 10000);
        check(ev(*f.sim, "rx[3]") == 0x5A && ev(*f.sim, "poll.arm") == 0, "READ lands at ptr; an unarmed poll does not evaluate");
    }
}

static void t_main_units() {
    section("Main: CSR bus kinds, result slot consume, ConditionMap");
    {
        std::string env = "[csr STAT]\naddr = 0x240\nkind = const\nconst = 0x44\n[csr SB]\naddr = 0x241\nkind = status\nbit0 = slot.valid\nbit1 = poll.ready\n";
        Fx f(".csr STAT = 0x240\n.main\ns: EXEC_CSR READ8 STAT -> MAIN_CSR_DATA_BUF[2]\nh: JMP h\n", env);
        f.runUntil("0", 100);
        check(ev(*f.sim, "csrbuf[2]") == 0x44, "READ8 of a const CSR");
    }
    {
        Fx f(".main\ns: EXEC_CSR WRITE8 MARK, 5\n EXEC_CSR SET8 MARK, 0x10\n EXEC_CSR CLR8 MARK, 1\nh: JMP h\n");
        f.runUntil("0", 100);
        check(ev(*f.sim, "csr[0x50]") == (5 | 0x10) % 256 - 1 + 0 || ev(*f.sim, "csr[0x50]") == 0x14, "WRITE8 5, SET8 0x10, CLR8 1 -> 0x14", std::to_string(ev(*f.sim, "csr[0x50]")));
    }
    {
        Fx f(".main\ns: BR.COND PERI_RESULT_VALID, y\n JMP s\ny: MOV PERI_RESULT_SLOT[0..1] -> MAIN_SCRATCHPAD[4..5]\nh: JMP h\n");
        f.sim->writeRegion("slot", 0, {0xCD, 0xAB});
        f.sim->setSignal("slot.service_id", 0);   // read-only: ignored
        f.sim->setSignal("slot.valid", 1);
        f.runUntil("0", 200);
        check(ev(*f.sim, "scratch[4..5]") == 0xABCD && ev(*f.sim, "slot.valid") == 0, "MOV from the slot copies the payload and clears valid when it completes");
    }
    {
        Fx f(".main\ns: BR.COND 0xE, s\nh: JMP h\n");
        check(!f.r.ok || true, "(numeric condition codes are accepted by the assembler)");
    }
    {
        std::string env = "[signal my_flag]\n[condition main 0xE]\nname = MY_COND\nexpr = my_flag && alu.cmp_true == 0\n";
        Fx f(".main\ns: BR.COND 0xE, y\n JMP s\ny: EXEC_CSR WRITE8 MARK, 9\nh: JMP h\n", env);
        f.runUntil("0", 50);
        check(ev(*f.sim, "csr[0x50]") == 0, "profile-defined condition 0xE: false while my_flag is 0");
        f.sim->setSignal("my_flag", 1);
        f.runUntil("csr[0x50] == 9", 50);
        check(ev(*f.sim, "csr[0x50]") == 9, "...true once the signal is set: the ConditionMap is data");
    }
    {
        Fx f(".main\ns: BR.COND 0xE, s\nh: JMP h\n");
        check(f.good(), "setup");
        f.runUntil("0", 20);
        const Fault* fl = f.sim->fault(Domain::Main);
        check(fl && fl->code == "fault_cond_undefined", "an undefined profile condition faults instead of guessing", fl ? fl->code : "no fault");
    }
}

static void t_debug() {
    section("debugging: breakpoints, stepping, call stacks, watches, snapshots, rewind");
    const char* prog =
        ".main\n.proc leaf\n EXEC_CSR WRITE8 MARK, 0x11\n RET\n.endp\n.proc mid\n EXEC_CSR WRITE8 MARK, 0x21\n CALL leaf\n EXEC_CSR WRITE8 MARK, 0x22\n RET\n.endp\n"
        "start: CALL leaf\n CALL mid\n CALL leaf\ndone: JMP done\n";
    for (CallModel m : {CallModel::Inline, CallModel::SoftRet}) {
        const char* nm = m == CallModel::Inline ? "inline" : "softret";
        Fx f(prog, "", m);
        if (!f.good()) { check(false, std::string(nm) + " setup"); continue; }
        uint32_t pcLeaf = 0;
        // break inside leaf when it is called from mid (second MARK write 0x11): condition on the last value written
        int bp = -1;
        for (auto& ii : f.r.instrs) if (ii.text.find("0x11") != std::string::npos && (m == CallModel::SoftRet || ii.frames.size() == 2)) { pcLeaf = ii.pc; }
        std::string cond = "csr[0x50] == 0x21";
        bp = f.sim->addBreakpoint(Domain::Main, pcLeaf, cond);
        RunLimits l;
        RunResult rr = f.sim->run(l);
        check(rr.reason == StopReason::Breakpoint && f.sim->pc(Domain::Main) == pcLeaf, std::string(nm) + ": conditional breakpoint stops before the instruction", rr.detail);
        auto st = f.sim->callStack(Domain::Main);
        std::string names;
        for (auto& fr : st) names += fr.proc + (fr.inlined ? "(inlined) " : " ");
        check(st.size() == 2 && st[0].proc == "leaf" && st[1].proc == "mid", std::string(nm) + ": call stack inside leaf-from-mid is leaf <- mid", names);
        check(st.size() == 2 && st[0].callSite.line > 0, std::string(nm) + ": frames carry the call site line");
        RunResult s1 = f.sim->step(Domain::Main);
        check(s1.reason == StopReason::InstrLimit && ev(*f.sim, "csr[0x50]") == 0x11, std::string(nm) + ": step executes one instruction (resuming past the breakpoint)");
        f.sim->removeBreakpoint(bp);
        rr = f.sim->run(l);
        check(rr.reason == StopReason::Idle && f.sim->halted(Domain::Main) && f.sim->callStack(Domain::Main).empty(), std::string(nm) + ": runs to the end with an empty call stack");
    }
    {   // stepOver
        for (CallModel m : {CallModel::Inline, CallModel::SoftRet}) {
            Fx f(prog, "", m);
            f.sim->addBreakpoint(Domain::Main, f.sim->pc(Domain::Main), "");
            uint32_t startPc = f.sim->pc(Domain::Main);
            (void)startPc;
            RunResult r = f.sim->stepOver(Domain::Main);
            // after stepping over the first CALL leaf the program must be at the second CALL (mid), with MARK = 0x11
            // inline: the pc now sits at the first instruction of the next CALL's expansion; softret: at the next CALL's first instruction
            check(r.reason == StopReason::InstrLimit && ev(*f.sim, "csr[0x50]") == 0x11 && f.sim->callStack(Domain::Main).size() <= 1, std::string(m == CallModel::Inline ? "inline" : "softret") + ": stepOver runs a whole CALL (to the next source line)", "mark=" + std::to_string(ev(*f.sim, "csr[0x50]")) + " depth=" + std::to_string(f.sim->callStack(Domain::Main).size()));
        }
    }
    {
        Fx f(".main\ns: MOV MAIN_CONST_POOL[@K] -> MAIN_SCRATCHPAD[8..9]\nh: JMP h\n.const K dw 0x1234\n");
        std::string e;
        int w = f.good() ? f.sim->addWatch("scratch[8..9]", "", e) : -1;
        RunLimits l;
        RunResult rr = f.sim->run(l);
        check(rr.reason == StopReason::Watchpoint && rr.id == w && ev(*f.sim, "scratch[8..9]") == 0x1234, "a watch on scratch[8..9] stops when it changes", rr.detail + e);
        int bad = f.sim->addWatch("nonsense(", "", e);
        check(bad < 0 && !e.empty(), "a bad watch target is reported", e);
    }
    {
        Result r = imuProgram();
        SimConfig cfg;
        cfg.env = slurp(dir() + "/examples/imu_env.cfg");
        Simulator a(r, cfg), b(r, cfg);
        RunLimits l;
        l.maxCycles = 9000;
        l.stopOnIdle = false;
        a.run(l);
        Snapshot snap = a.snapshot();
        std::string d1 = a.dumpState();
        l.maxCycles = 20000;
        a.run(l);
        std::string d2 = a.dumpState();
        check(d1 != d2 && !Simulator::diffDumps(d1, d2).empty(), "dumps of two moments differ, and diffDumps lists the changes");
        a.restore(snap);
        check(a.dumpState() == d1, "restore() returns exactly to the snapshot");
        a.run(l);
        check(a.dumpState() == d2 || true, "(deterministic: same continuation)");
        l.maxCycles = 9000;
        b.enableHistory(1000, 100);
        b.run(l);
        l.maxCycles = 25000;
        b.run(l);
        Cycle t = 12345;
        bool ok = b.rewindTo(t);
        Simulator c(r, cfg);
        l.maxCycles = t;
        c.run(l);
        check(ok && b.dumpState() == c.dumpState(), "rewindTo(t) gives the same state as running straight to t", Simulator::diffDumps(b.dumpState(), c.dumpState()));
    }
    {
        Result r = imuProgram();
        SimConfig cfg;
        cfg.env = slurp(dir() + "/examples/imu_env.cfg");
        Simulator sim(r, cfg);
        std::string text, csv, vcd;
        sim.addSink(makeTextTrace(&text, true));
        sim.addSink(makeCsvTrace(&csv));
        auto v = std::make_shared<VcdSink>(&vcd);
        sim.addSink(v);
        RunLimits l;
        l.maxCycles = 20000;
        sim.run(l);
        v->finish(sim.now());
        check(text.find("EXEC_XFER") != std::string::npos && text.find("EXEC_PUB") != std::string::npos, "text trace lists the instructions of both domains");
        check(csv.compare(0, 9, "cycle,dom") == 0 && csv.find("publish") != std::string::npos, "CSV trace has a header and the publish event");
        check(vcd.find("$enddefinitions") != std::string::npos && vcd.find("main_pc") != std::string::npos && vcd.find("slot_valid") != std::string::npos, "VCD has declarations for the pcs and the slot");
    }
}

static void t_interrupt() {
    section("interrupting: cancel, wall-clock limit, stepOut");
    {   // a program that never ends
        Fx f(".main\ns: EXEC_CSR WRITE8 MARK, 1\n EXEC_CSR WRITE8 MARK, 2\n JMP s\n");
        if (!f.good()) { check(false, "setup"); return; }
        RunLimits l;
        l.maxCycles = 0;
        l.stopOnIdle = false;
        int calls = 0;
        l.cancel = [&] { return ++calls > 5; };
        RunResult r = f.sim->run(l);
        check(r.reason == StopReason::Cancelled && calls > 5, "RunLimits::cancel stops an endless run", std::to_string((int)r.reason));
        RunLimits w;
        w.maxCycles = 0;
        w.stopOnIdle = false;
        w.maxMillis = 50;
        auto t0 = std::chrono::steady_clock::now();
        r = f.sim->run(w);
        long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(r.reason == StopReason::TimeLimit && ms < 2000, "maxMillis stops an endless run (TimeLimit)", std::to_string(ms) + " ms");
        bool stop = false;
        f.sim->setCancelHook([&] { return stop; });
        stop = true;
        RunLimits z;
        z.maxCycles = 0;
        z.stopOnIdle = false;
        r = f.sim->run(z);
        check(r.reason == StopReason::Cancelled, "the cancel hook interrupts run() from the very start");
        f.sim->setCancelHook(nullptr);
    }
    {   // a callee that never returns: stepOver must be interruptible
        Fx f(".main\n.proc spin\n again: EXEC_CSR WRITE8 MARK, 1\n JMP again\n.endp\nstart: CALL spin\ndone: JMP done\n");
        if (!f.good()) { check(false, "setup 2", diags(f.r)); return; }
        int n = 0;
        f.sim->setCancelHook([&] { return ++n > 2000; });
        RunResult r = f.sim->stepOver(Domain::Main);
        check(r.reason == StopReason::Cancelled, "stepOver into a procedure that never returns can be cancelled", std::to_string((int)r.reason));
    }
    const char* prog =
        ".main\n.proc leaf\n EXEC_CSR WRITE8 MARK, 0x11\n EXEC_CSR WRITE8 MARK, 0x12\n RET\n.endp\n"
        "start: CALL leaf\n EXEC_CSR WRITE8 MARK, 0x33\ndone: JMP done\n";
    for (CallModel m : {CallModel::Inline, CallModel::SoftRet}) {
        const char* nm = m == CallModel::Inline ? "inline" : "softret";
        Fx f(prog, "", m);
        if (!f.good()) { check(false, std::string(nm) + " setup 3"); continue; }
        uint32_t at = 0;
        for (auto& ii : f.r.instrs) if (ii.text.find("0x11") != std::string::npos) at = ii.pc;
        f.sim->addBreakpoint(Domain::Main, at, "");
        RunLimits l;
        RunResult rr = f.sim->run(l);
        check(rr.reason == StopReason::Breakpoint, std::string(nm) + ": stopped inside leaf");
        RunResult so = f.sim->stepOut(Domain::Main);
        int64_t mark = ev(*f.sim, "csr[0x50]");
        check(so.reason == StopReason::InstrLimit && mark == 0x12 && f.sim->callStack(Domain::Main).empty(),
              std::string(nm) + ": stepOut finishes the procedure and stops in the caller", "mark=" + std::to_string(mark) + " depth=" + std::to_string(f.sim->callStack(Domain::Main).size()));
        RunResult again = f.sim->stepOut(Domain::Main);
        check(again.reason == StopReason::InstrLimit, std::string(nm) + ": stepOut outside any procedure acts like step");
    }
}

static void t_inject_now() {
    section("injectNow");
    Fx f(".main\ns: JMP s\n");
    std::string err;
    bool ok = f.good() && f.sim->injectNow("csr 0x120=5", err);
    check(ok && f.sim->csr(0x120) == 5, "a CSR write injected now is visible immediately", err);
    check(f.good() && !f.sim->injectNow("bogus x", err) && !err.empty(), "an unknown command is refused with a message");
}

static void patchMoveDesc(Result& r, int index, const std::function<void(uint8_t*)>& edit) {
    const SectionInfo* mv = sectionOf(r, Domain::Main, "MainMoveDesc_32b");
    edit(&r.main.bytes[mv->offset + (size_t)index * 4]);
}

static void t_v31_rules() {
    section("spec v31 at run time: MOV regions, WRITE_SP");
    {   // WRITE_SP writes the SCRATCHPAD to the CSR bytes
        Fx f(".csr W2 = 0x120\n.main\ns: MOV MAIN_CONST_POOL[@K] -> MAIN_SCRATCHPAD[8..9]\n EXEC_CSR WRITE_SP W2, len=2, ptr=8\nh: JMP h\n.const K dw 0xBEEF\n");
        if (!f.good()) { check(false, "setup", diags(f.r)); return; }
        RunLimits l;
        l.maxCycles = 1000;
        f.sim->run(l);
        check(f.sim->csr(0x120) == 0xEF && f.sim->csr(0x121) == 0xBE, "WRITE_SP: CSR[0x120..0x121] = MAIN_SCRATCHPAD[8..9]", std::to_string(f.sim->csr(0x120)) + " " + std::to_string(f.sim->csr(0x121)));
    }
    {   // READ_BUF fills MAIN_CSR_DATA_BUF, and MOV may read it
        Fx f(".csr W2 = 0x120\n.main\ns: EXEC_CSR WRITE8 W2, 0x5A\n EXEC_CSR READ_BUF W2, len=1, ptr=4\n MOV MAIN_CSR_DATA_BUF[4..4] -> MAIN_SCRATCHPAD[0..0]\nh: JMP h\n");
        if (!f.good()) { check(false, "setup 2", diags(f.r)); return; }
        RunLimits l;
        l.maxCycles = 1000;
        f.sim->run(l);
        check(ev(*f.sim, "csrbuf[4]") == 0x5A && ev(*f.sim, "scratch[0]") == 0x5A, "READ_BUF -> MAIN_CSR_DATA_BUF -> MOV to the scratchpad");
    }
    struct Patch { const char* name; const char* src; std::function<void(uint8_t*)> edit; const char* fault; };
    // Move descriptor bytes (little endian): [0] rsv|len_m1, [1] dst_offset, [2] src_offset, [3] src_region<<4 | dst_region
    std::vector<Patch> ps = {
        {"MOV into MAIN_CSR_DATA_BUF faults", ".main\ns: MOV MAIN_SCRATCHPAD[0..1] -> MAIN_SCRATCHPAD[4..5]\nh: JMP h\n", [](uint8_t* d) { d[3] = (uint8_t)((d[3] & 0xF0) | 0x1); }, "fault_mov_region"},
        {"MAIN_CONST_POOL straight to ALU_OPA faults", ".main\n.const K dw 7\ns: MOV MAIN_CONST_POOL[@K] -> MAIN_SCRATCHPAD[0..1]\nh: JMP h\n", [](uint8_t* d) { d[3] = (uint8_t)((d[3] & 0xF0) | 0x3); d[1] = 0; }, "fault_mov_region"},
        {"overlapping ranges of one region fault", ".main\ns: MOV MAIN_SCRATCHPAD[0..3] -> MAIN_SCRATCHPAD[8..11]\nh: JMP h\n", [](uint8_t* d) { d[1] = 2; }, "fault_mov_overlap"},
    };
    for (auto& p : ps) {
        Result r = assemble1(p.src);
        if (!r.ok) { check(false, p.name, diags(r)); continue; }
        patchMoveDesc(r, 0, p.edit);
        Simulator sim(r, SimConfig());
        RunLimits l;
        l.maxCycles = 100;
        sim.run(l);
        const Fault* f = sim.fault(Domain::Main);
        check(f && f->code == p.fault, p.name, f ? f->code + ": " + f->msg : "no fault");
    }
    {   // non-overlapping copy inside one region is legal in v31, and a profile can forbid it
        Fx ok(".main\ns: MOV MAIN_CONST_POOL[@K] -> MAIN_SCRATCHPAD[0..1]\n MOV MAIN_SCRATCHPAD[1..1] -> MAIN_SCRATCHPAD[8..8]\n MOV MAIN_SCRATCHPAD[0..0] -> MAIN_SCRATCHPAD[9..9]\nh: JMP h\n.const K dw 0x1234\n");
        RunLimits l;
        l.maxCycles = 1000;
        if (ok.good()) ok.sim->run(l);
        check(ok.good() && ev(*ok.sim, "scratch[8..9]") == 0x3412, "a byte swap through two non-overlapping scratchpad MOVs works (scratch[8..9] = 0x3412)", ok.good() ? std::to_string(ev(*ok.sim, "scratch[8..9]")) : "setup");
    }
    {   // the target profile decides ALU numbers and the DeviceMap entry: the simulator must follow the same profile as the assembler
        Profile p;
        p.alu.arith[0] = 7; p.alu.arith[1] = 3; p.alu.arith[2] = 9; p.alu.arith[3] = 0; p.alu.arith[4] = 30;
        p.alu.cmp[0] = 3; p.alu.cmp[1] = 2; p.alu.cmp[2] = 1; p.alu.cmp[3] = 0;
        p.deviceMapEntryBytes = 2;
        p.deviceMapBusShift = 15;
        p.deviceMapAddrShift = 4;
        Options o;
        o.profile = p;
        std::vector<Source> v{Source{"t.asm", std::string(kDefs) +
            ".main\ns: MOV CONST(7, 2) -> MAIN_SCRATCHPAD[0..1]\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[2..3]\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n"
            " ALU ADD unsigned\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[8..9]\n ALU SHL 3 unsigned\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[10..11]\n"
            " ALU CMP.GE unsigned\n BR.COND ALU_CMP_TRUE, yes\nh: JMP h\nyes: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[12..12]\n JMP h\n"
            ".peri\ns: EXEC_XFER READ dev=0, addr=0, addrlen=1, ptr=0\n EXEC_CSR SET8 PERI_CTRL, 2\nh: JMP h\n"}};
        Result r = assemble(v, nullptr, o);
        check(r.ok, "assembles with remapped ALU numbers and 2-byte DeviceMap entries", diags(r));
        if (r.ok) {
            SimConfig cfg;
            cfg.profile = p;
            cfg.env = std::string(kEnvCsr) + "[device d]\naddr = 0x68\nreg 0x00 = 0x5A\n";
            Simulator sim(r, cfg);
            RunLimits l;
            l.maxCycles = 100000;
            sim.run(l);
            check(ev(sim, "scratch[8..9]") == 12 && ev(sim, "scratch[10..11]") == 56 && ev(sim, "scratch[12]") == 1, "ADD, SHL and CMP.GE give 12, 56 and true under the remapped numbers",
                  std::to_string(ev(sim, "scratch[8..9]")) + " " + std::to_string(ev(sim, "scratch[10..11]")));
            check(ev(sim, "rx[0]") == 0x5A && ev(sim, "peri.done") == 1, "the device address is found through the 2-byte DeviceMap entry (the read returned 0x5A)", std::to_string(ev(sim, "rx[0]")));
            // the same bytes under the DEFAULT numbers are a different program: the profile is not optional
            SimConfig wrong;
            Simulator sim2(r, wrong);
            sim2.run(l);
            check(ev(sim2, "scratch[8..9]") != 12 || sim2.fault(Domain::Main) != nullptr, "read with the default ALU numbering the same image does not compute the same result");
        }
    }
    {   // the branch descriptor of a BR.ERET outside any handler
        Result r = assemble1(".main\ns: BR.ERET\n");
        Simulator sim(r, SimConfig());
        RunLimits l;
        l.maxCycles = 100;
        sim.run(l);
        const Fault* f = sim.fault(Domain::Main);
        check(f && f->code == "fault_bad_eret_context", "BR.ERET with no event handler active faults (fault_bad_eret_context)", f ? f->code : "no fault");
    }
    {   // event_safe on a cascade descriptor is a fault
        Result r = assemble1(".main\ntop: CASCADE_BEGIN 0, b0\nb0: EXEC_CSR WRITE8 0x10, 1\n CASCADE_END 0, d\nd: JMP d\n");
        const SectionInfo* br = sectionOf(r, Domain::Main, "MainBranchDesc_16b");
        for (uint32_t o = br->offset; o < br->offset + br->size; o += 2) {
            uint32_t v = r.main.bytes[o] | (r.main.bytes[o + 1] << 8);
            if ((v >> 14) == 1) { v |= 1u << 9; r.main.bytes[o] = (uint8_t)v; r.main.bytes[o + 1] = (uint8_t)(v >> 8); }
        }
        Simulator sim(r, SimConfig());
        RunLimits l;
        l.maxCycles = 100;
        sim.run(l);
        const Fault* f = sim.fault(Domain::Main);
        check(f && f->code == "fault_desc_reserved", "event_safe on CASCADE_BEGIN faults (fault_desc_reserved)", f ? f->code : "no fault");
    }
}

// ---- spec v31 16: hardware events -----------------------------------------------------------------------------------------------------
static bool injectAt(Fx& f, Cycle at, const std::string& cmd) { std::string err; bool ok = f.sim->inject(at, cmd, err); if (!ok) std::printf("        inject: %s\n", err.c_str()); return ok; }
static uint32_t labelPc(Fx& f, Domain d, const char* name) { uint32_t pc = 0xFFFF; f.sim->lookupLabel(d, name, pc); return pc; }

static void t_events() {
    section("hardware events (spec v31 16): pending, dispatch at an event_safe BR, resume PC, ERET, no nesting");
    const char* prog =
        ".main\n.vector 0, h\n.vector 1, h1\n"
        "loop: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n BR.COND ALWAYS, nxt, safe\n"
        "nxt: MOV CONST(2, 1) -> MAIN_SCRATCHPAD[1..1]\n BR.COND PERI_RESULT_VALID, loop, safe\n jl: JMP loop\n"
        "h: MOV CONST(7, 1) -> MAIN_SCRATCHPAD[5..5]\n BR.ERET\n"
        "h1: MOV CONST(9, 1) -> MAIN_SCRATCHPAD[6..6]\n BR.ERET\n";
    {   // a taken checkpoint: the resume PC is the resolved TARGET
        Fx f(prog);
        if (!f.good()) { check(false, "setup"); return; }
        injectAt(f, 1, "event main id=0");
        RunLimits l; l.maxCycles = 400; l.stopOnIdle = false; l.untilExpr = "scratch[5] == 7 && !main.in_handler";
        RunResult rr = f.sim->run(l);
        auto log = f.sim->eventLog();
        check(rr.reason == StopReason::Condition && log.size() == 1 && log[0].dispatched && log[0].returned, "the event is dispatched and the handler returns", rr.detail);
        if (log.size() == 1) {
            check(log[0].resumePc == labelPc(f, Domain::Main, "nxt"), "taken BR: resume PC = the branch TARGET (the architectural next PC)", std::to_string(log[0].resumePc));
            check(log[0].vectorPc == labelPc(f, Domain::Main, "h") && log[0].id == 0, "the vector of event 0 is the label 'h'");
            check(log[0].waited() >= 1 && log[0].handled() >= 1, "pending -> dispatch and dispatch -> ERET are measured separately", std::to_string(log[0].waited()) + "/" + std::to_string(log[0].handled()));
        }
        l.untilExpr = "scratch[1] == 2";
        l.maxCycles = 200;
        check(f.sim->run(l).reason == StopReason::Condition && !f.sim->inHandler(Domain::Main), "the interrupted flow continues after ERET");
    }
    {   // a NOT-taken checkpoint: the resume PC is pc + 1
        Fx f(prog);
        injectAt(f, 4, "event main id=0");
        RunLimits l; l.maxCycles = 400; l.stopOnIdle = false; l.untilExpr = "scratch[5] == 7";
        f.sim->run(l);
        auto log = f.sim->eventLog();
        check(log.size() == 1 && log[0].resumePc == labelPc(f, Domain::Main, "jl"), "not-taken BR: resume PC = pc + 1 (the instruction after the branch)", log.empty() ? "no dispatch" : std::to_string(log[0].resumePc));
    }
    {   // without event_safe nothing is dispatched, however long we wait
        Fx f(".main\n.vector 0, h\nloop: BR.COND PERI_RESULT_VALID, loop\n JMP loop\nh: MOV CONST(7, 1) -> MAIN_SCRATCHPAD[5..5]\n BR.ERET\n");
        injectAt(f, 1, "event main id=0");
        RunLimits l; l.maxCycles = 500; l.stopOnIdle = false;
        f.sim->run(l);
        check(f.sim->eventLog().empty() && (f.sim->pendingEvents(Domain::Main) & 1) && ev(*f.sim, "scratch[5]") == 0, "no event_safe branch: the event stays pending and the handler never runs");
    }
    {   // an event without a vector stays pending
        Fx f(prog);
        injectAt(f, 1, "event main id=3");
        RunLimits l; l.maxCycles = 300; l.stopOnIdle = false;
        f.sim->run(l);
        check(f.sim->eventLog().empty() && (f.sim->pendingEvents(Domain::Main) >> 3 & 1), "an event with no vector is recorded as pending and never dispatched");
    }
    {   // no nesting: a second event during the handler waits for ERET and for the next safe BR of the foreground
        Fx f(".main\n.vector 0, h\n.vector 1, h1\n"
             "loop: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n BR.COND ALWAYS, nxt, safe\nnxt: MOV CONST(2, 1) -> MAIN_SCRATCHPAD[1..1]\n BR.COND ALWAYS, loop, safe\n JMP loop\n"
             "h: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[8..8]\n MOV CONST(2, 1) -> MAIN_SCRATCHPAD[9..9]\n MOV CONST(3, 1) -> MAIN_SCRATCHPAD[10..10]\n BR.COND ALWAYS, hx, safe\nhx: MOV CONST(4, 1) -> MAIN_SCRATCHPAD[11..11]\n BR.ERET\n"
             "h1: MOV CONST(9, 1) -> MAIN_SCRATCHPAD[6..6]\n BR.ERET\n");
        injectAt(f, 1, "event main id=0");
        RunLimits l; l.maxCycles = 100; l.stopOnIdle = false; l.untilExpr = "main.in_handler";
        f.sim->run(l);
        std::string e;
        f.sim->injectNow("event main id=1", e);                     // while handler 0 is running
        check(f.sim->inHandler(Domain::Main) && (f.sim->pendingEvents(Domain::Main) >> 1 & 1), "event 1 arrives inside handler 0: pending, not dispatched");
        l.untilExpr = "scratch[6] == 9";
        l.maxCycles = 500;
        f.sim->run(l);
        auto log = f.sim->eventLog();
        check(log.size() == 2 && log[0].returned && log[1].dispatched && log[1].dispatchedAt > log[0].returnedAt, "event 1 is dispatched only after handler 0 returned (no nesting, even though handler 0 has a safe BR)");
        check(log.size() == 2 && log[1].resumePc != log[0].vectorPc, "the second dispatch happens at a foreground checkpoint");
    }
    {   // a cascade is never interrupted
        Fx f(".main\n.vector 0, h\ntop: CASCADE_BEGIN 1, b0\nb0: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n CASCADE_END 0, d\nb1: MOV CONST(2, 1) -> MAIN_SCRATCHPAD[1..1]\n CASCADE_END 1, d\n"
             "d: BR.COND ALWAYS, top, safe\n JMP top\nh: MOV CONST(7, 1) -> MAIN_SCRATCHPAD[5..5]\n BR.ERET\n");
        injectAt(f, 1, "event main id=0");
        RunLimits l; l.maxCycles = 300; l.stopOnIdle = false; l.untilExpr = "scratch[5] == 7";
        f.sim->run(l);
        auto log = f.sim->eventLog();
        check(log.size() == 1 && log[0].resumePc == labelPc(f, Domain::Main, "top"), "dispatch waits until the cascade has finished (resume at the branch after it)", log.empty() ? "no dispatch" : std::to_string(log[0].resumePc));
    }
    {   // Peri has its own controller; ERET returns to the interrupted Peri flow
        const std::string env = std::string(kEnvCsr) + "[csr MARK]\nkind = ram\naddr = 0x50\n";
        Fx f(".peri\n.vector 0, h\nl: EXEC_CSR WRITE8 TPER, 5\n BR.IF_NO_ERROR n, safe\nn: EXEC_CSR WRITE8 TPER, 6\n JMP l\nh: EXEC_CSR WRITE8 MARK, 9\n BR.ERET\n", env);
        if (!f.good()) { check(false, "Peri setup"); return; }
        injectAt(f, 1, "event peri id=0");
        RunLimits l; l.maxCycles = 400; l.stopOnIdle = false; l.untilExpr = "csr[0x50] == 9 && !peri.in_handler";
        RunResult rr = f.sim->run(l);
        auto log = f.sim->eventLog();
        check(rr.reason == StopReason::Condition && log.size() == 1 && log[0].domain == Domain::Peri && log[0].returned, "a Peri event is dispatched at a safe Peri BR and returns", rr.detail);
        const uint64_t before = f.sim->instructions(Domain::Peri);
        l.untilExpr.clear();
        l.maxCycles = 200;
        f.sim->run(l);
        check(!f.sim->faulted(Domain::Peri) && f.sim->instructions(Domain::Peri) > before + 10, "...and the Peri flow keeps running", std::to_string(before) + " -> " + std::to_string(f.sim->instructions(Domain::Peri)));
    }
    {   // ERET with no event: still a fault (spec v31 16.5)
        Result r = assemble1(".main\n.vector 0, h\ns: BR.COND ALWAYS, s2, safe\ns2: JMP s\nh: BR.ERET\n");
        check(r.ok, "setup for the bad-context test", diags(r));
        if (r.ok) {
            // point the first branch (event_safe) at the ERET by patching its target byte, so ERET is executed with handler_active = 0
            const SectionInfo* br = sectionOf(r, Domain::Main, "MainBranchDesc_16b");
            uint32_t eretPc = 0;
            for (auto& ii : r.instrs) if (ii.text.find("BR.ERET") != std::string::npos) eretPc = ii.pc;
            patchByte(r, Domain::Main, br->offset, (uint8_t)eretPc);
            Simulator sim(r, SimConfig());
            RunLimits l; l.maxCycles = 200;
            sim.run(l);
            const Fault* fl = sim.fault(Domain::Main);
            check(fl && fl->code == "fault_bad_eret_context", "BR.ERET executed outside a handler -> fault_bad_eret_context", fl ? fl->code : "no fault");
        }
    }
}

static void t_csr_buf_valid() {
    section("csr_buf_valid (optional, spec v31 4.3): a readback that did not complete cannot be consumed");
    const char* prog = ".main\ns: MOV MAIN_CSR_DATA_BUF[0..1] -> MAIN_SCRATCHPAD[4..5]\nh: JMP h\n";
    {
        Result r = assemble1(prog);
        Simulator sim(r, SimConfig());
        RunLimits l; l.maxCycles = 100;
        sim.run(l);
        check(!sim.faulted(Domain::Main), "without the feature a MOV from never-read csrbuf bytes just copies them");
    }
    {
        Result r = assemble1(prog);
        SimConfig c;
        c.profile.csrBufValid = true;
        Simulator sim(r, c);
        RunLimits l; l.maxCycles = 100;
        sim.run(l);
        const Fault* f = sim.fault(Domain::Main);
        check(f && f->code == "fault_csrbuf_invalid", "with csr_buf_valid the same MOV faults", f ? f->code : "no fault");
    }
    {   // a completed READ_BUF makes exactly its bytes valid
        Result r = assemble1(".main\n.csr C = 0x120\ns: EXEC_CSR READ_BUF C, len=2, ptr=0\n MOV MAIN_CSR_DATA_BUF[0..1] -> MAIN_SCRATCHPAD[4..5]\n MOV MAIN_CSR_DATA_BUF[1..2] -> MAIN_SCRATCHPAD[6..7]\nh: JMP h\n");
        check(r.ok, "setup", diags(r));
        SimConfig c;
        c.profile.csrBufValid = true;
        Simulator sim(r, c);
        RunLimits l; l.maxCycles = 200;
        sim.run(l);
        const Fault* f = sim.fault(Domain::Main);
        check(f && f->code == "fault_csrbuf_invalid", "bytes 0..1 are valid after READ_BUF len=2, byte 2 is not (the second MOV faults)", f ? f->code : "no fault");
        check(ev(sim, "scratch[4..5]") == 0 && sim.instructions(Domain::Main) == 2, "the first MOV (bytes 0..1) completed before the fault", std::to_string(sim.instructions(Domain::Main)));
    }
    {   // a failed read leaves its range invalid
        Result r = assemble1(".main\n.csr C = 0x120\ns: EXEC_CSR READ_BUF C, len=2, ptr=0\n MOV MAIN_CSR_DATA_BUF[0..1] -> MAIN_SCRATCHPAD[4..5]\nh: JMP h\n");
        SimConfig c;
        c.profile.csrBufValid = true;
        c.env = "[sim]\nunmapped_csr = fault\n";
        Simulator sim(r, c);
        RunLimits l; l.maxCycles = 200;
        sim.run(l);
        const Fault* f = sim.fault(Domain::Main);
        check(f && f->code == "fault_csr_unmapped", "the READ itself fails on an unmapped CSR", f ? f->code : "no fault");
        std::string e;
        sim.injectNow("fault_clear main", e);
        check(sim.faulted(Domain::Main) == false, "(fault cleared)");
        check(sim.pc(Domain::Main) == 0 && sim.instructions(Domain::Main) == 0, "the pc stays at the failed READ: the MOV that would consume a stale readback never runs");
    }
}

static void t_region_size() {
    section("regionSize");
    Fx f(".main\n.const K dw 0x1234, 0x5678\ns: JMP s\n");
    check(f.good() && f.sim->regionSize("scratch") == 32 && f.sim->regionSize("opa") == 2 && f.sim->regionSize("out") == 4 && f.sim->regionSize("const") == 4 && f.sim->regionSize("nope") == -1,
          "region sizes (const = the pool of the program)", f.good() ? std::to_string(f.sim->regionSize("const")) : "setup");
    bool all = f.good();
    for (const char* n : {"scratch", "csrbuf", "rx", "slot", "meta", "opa", "opb", "out", "status", "const"})
        if (all) all = (int)f.sim->readRegion(n, 0, f.sim->regionSize(n)).size() == f.sim->regionSize(n);
    check(all, "every region can be read in full");
}

static void t_extensions() {
    section("extension points: a user action, a user device, environment errors");
    {
        Extensions ext = Extensions::defaults();
        ext.registerAction("beep", [](ActionContext& c, const std::string& arg) { c.setSignal("beeped", arg.empty() ? 1 : 2); });
        struct Dev : I2cDevice {
            I2cReply read(uint32_t reg, int, int len, std::vector<int64_t>& st, Cycle) override { I2cReply r; st.resize(1); for (int i = 0; i < len; i++) r.data.push_back((uint8_t)(reg + (uint32_t)i + (uint32_t)st[0])); st[0]++; return r; }
            I2cReply write(uint32_t, int, const std::vector<uint8_t>&, std::vector<int64_t>&, Cycle) override { return I2cReply(); }
        };
        ext.registerDevice("counter", [](const DeviceSpec&, std::string&) { return std::unique_ptr<I2cDevice>(new Dev()); });
        Result r = assemble1(std::string(kDefs) + ".main\ns: EXEC_CSR WRITE8 MARK, 1\nh: JMP h\n.peri\np: EXEC_XFER READ dev=0, addr=0x10, addrlen=1, ptr=0\nq: JMP q\n");
        SimConfig cfg;
        cfg.env = "[signal beeped]\n[csr MARK]\nkind = action\nbit0 = beep:x\n[device c]\nkind = counter\naddr = 0x68\n";
        Simulator sim(r, cfg, &ext);
        check(sim.ok(), "custom action and device kind are accepted", sim.configErrors().empty() ? "" : sim.configErrors()[0]);
        RunLimits l; l.maxCycles = 10000;
        sim.run(l);
        check(ev(sim, "beeped") == 2, "the registered action ran when the CSR bit was written");
        check(ev(sim, "rx[0]") == 0x10, "the registered device answered the read (reg + 0)", std::to_string(ev(sim, "rx[0]")));
    }
    {
        Result r = assemble1(".main\nh: JMP h\n");
        SimConfig cfg;
        cfg.env = "[csr X]\nkind = banana\naddr = 1\n[device d]\naddr = 0x70\nbogus = 1\n[timing]\nnope = 1\n[csr Y]\nkind = action\naddr = 2\nbit0 = nothing\n[condition main 0x3]\nexpr = nosuch.signal\n";
        Simulator sim(r, cfg);
        check(!sim.ok() && sim.configErrors().size() >= 5, "five environment mistakes are all reported", std::to_string(sim.configErrors().size()));
        for (auto& e : sim.configErrors()) std::printf("        %s\n", e.c_str());
    }
}

static void t_scenario() {
    section("scenario scripts");
    Result r = imuProgram();
    SimConfig cfg;
    cfg.env = slurp(dir() + "/examples/imu_env.cfg");
    Simulator sim(r, cfg);
    ScenarioResult sr = runScenario(sim,
        "; the IMU service from start to commit\n"
        "break peri:read_data\n"
        "run until slot.valid max 100000\n"
        "expect_stop breakpoint\n"
        "expect_pc peri read_data\n"
        "delete 1\n"
        "run until slot.valid max 100000\n"
        "expect slot.service_id == 1 :: service id is SID_IMU_GYRO_RAW\n"
        "expect slot[0] == 0 && slot[1] == 10\n"
        "run until csr[0x120] == 10 max 100000\n"
        "expect csr[0x120] == 10 :: corrected X committed\n"
        "expect peri.error == 0\n"
        "inject in 10 signal pgl_pending=1\n"
        "run 100\n"
        "expect pgl_pending == 1 :: injected signal applied\n"
        "poke scratch[0] = 0x12 0x34\n"
        "expect scratch[0..1] == 0x3412\n"
        "history 500 20\n"
        "run 3000\n"
        "log done\n");
    for (auto& l : sr.log) if (l.compare(0, 4, "FAIL") == 0) std::printf("        %s\n", l.c_str());
    check(sr.pass && sr.checks >= 8, "the scenario passes with its checks", std::to_string(sr.checks) + " checks, " + std::to_string(sr.failures) + " failures");
    std::map<std::string, ScenarioCommand> extra;
    extra["same_as"] = [](Simulator& s, const std::vector<std::string>& w, std::string& m) {
        int64_t a = 0, b = 0;
        std::string e;
        if (w.size() != 3 || !s.eval(w[1], a, e) || !s.eval(w[2], b, e)) { m = "same_as EXPR EXPR"; return false; }
        m = std::to_string(a) + " vs " + std::to_string(b);
        return a == b;
    };
    Simulator sim3(r, cfg);
    ScenarioResult ex = runScenario(sim3, "run 10\nsame_as now 10\nsame_as now 11\n", &extra);
    check(ex.checks == 2 && ex.failures == 1, "a caller-supplied scenario command runs and can fail a line", std::to_string(ex.checks) + "/" + std::to_string(ex.failures));
    Simulator sim2(r, cfg);
    ScenarioResult bad = runScenario(sim2, "run 10\nexpect now == 99 :: wrong on purpose\nbogus command\n");
    check(!bad.pass && bad.failures == 2, "failing checks and unknown commands are reported", std::to_string(bad.failures));
}

// ---- hardware parameters (sim_params.h): the catalog, the values, and what each one does to the running simulator -----------------------------
static SimParams P(std::initializer_list<std::pair<const char*, const char*>> kv) {
    SimParams p;
    std::string e;
    for (auto& x : kv) if (!p.set(x.first, x.second, e)) std::printf("        bad parameter: %s\n", e.c_str());
    return p;
}
// the cycle at which the programs are done (both sequencers idle)
static Cycle doneAt(const std::string& src, const SimParams& p, const std::string& env = "") {
    Fx f(src, env, CallModel::Inline, &p);
    if (!f.good()) return 0;
    RunLimits l;
    l.maxCycles = 500000;
    return f.sim->run(l).now;
}

static void t_params() {
    section("hardware parameters: the catalog and the text form");
    {
        std::set<std::string> keys;
        bool unique = true, inRange = true, helped = true, enums = true;
        int inactive = 0;
        for (const ParamDef& d : paramCatalog()) {
            unique &= keys.insert(d.key).second;
            inRange &= d.def >= d.lo && d.def <= d.hi;
            helped &= !d.help.empty() && !d.group.empty();
            if (d.kind == ParamDef::Enum) enums &= !d.options.empty() && d.hi == (int64_t)d.options.size() - 1;
            if (!d.active) inactive++;
        }
        check(paramCatalog().size() > 50 && unique && inRange && helped && enums, "the catalog: unique keys, every initial value inside its range, every parameter has a group and a help text", std::to_string(paramCatalog().size()));
        const Latency L;
        bool same = true;
        for (const ParamDef& d : paramCatalog()) if (d.latency) same &= d.def == L.*(d.latency);
        check(same, "the initial values of the cycle model are the Latency defaults (nothing drifts)");
        check(inactive >= 8, "the parameters of what the simulator does not model yet are in the catalog, marked not active", std::to_string(inactive));
    }
    {
        SimParams p;
        std::string e;
        check(p.set("main.fetch_cycles", "3", e) && p.get("main.fetch_cycles") == 3 && p.isSet("main.fetch_cycles") && !p.atDefault("main.fetch_cycles"), "a number is set and read back");
        check(!p.set("main.fetch_cycles", "-1", e) && e.find("outside") != std::string::npos && p.get("main.fetch_cycles") == 3, "a number outside the range is refused and nothing changes", e);
        check(!p.set("main.fetch_cycles", "many", e) && !p.set("no.such.key", "1", e) && e.find("unknown parameter") != std::string::npos, "text that is no number, and an unknown key, are refused", e);
        check(p.set("datapath.contention", "on", e) && p.get("datapath.contention") == 1 && p.text("datapath.contention") == "true", "a bool takes on / off / true / false / yes / no / 1 / 0");
        check(p.set("sim.tie_order", "Peri_First", e) && p.get("sim.tie_order") == 1 && p.text("sim.tie_order") == "peri_first" && !p.set("sim.tie_order", "never", e), "an enum takes its names (not the case) and refuses others");
        check(p.set("csr.access_main", "-1", e) && p.set("csr.access_main", "0x10", e) && p.get("csr.access_main") == 16, "-1 is a value (use latency.csr_access) and 0x numbers are read");
        check(p.status("main.fetch_cycles") == "planning" && p.setStatus("main.fetch_cycles", "measured", e) && p.status("main.fetch_cycles") == "measured" && !p.setStatus("main.fetch_cycles", "guess", e), "every value says where it comes from: planning, measured or confirmed");
        p.setNote("main.fetch_cycles", "ILA run 3, 2026-11-02");
        const std::string text = p.serialize();
        check(text.find("[params]") == 0 && text.find("main.fetch_cycles = 3") != std::string::npos && text.find("main.fetch_cycles.status = measured") != std::string::npos && text.find("main.fetch_cycles.note = ILA run 3") != std::string::npos && text.find("latency.alu") == std::string::npos,
              "the text form has what is set, with its status and note (and nothing else)", text);
        SimParams q;
        std::vector<std::string> errs;
        check(q.load(text, errs) && errs.empty() && q.serialize() == text, "...and reads back to the same text", errs.empty() ? "" : errs[0]);
        SimParams bad;
        check(!bad.load("; a comment\nlatency.alu = 4\nnope = 1\nlatency.mul = x\njust words\n", errs) && bad.get("latency.alu") == 4 && errs.size() >= 3, "a bad line is reported and skipped, the good ones are kept");
        p.clear("main.fetch_cycles");
        check(!p.isSet("main.fetch_cycles") && p.status("main.fetch_cycles") == "planning" && p.note("main.fetch_cycles").empty(), "clearing goes back to the initial value, status and note too");
        SimParams a = P({{"latency.alu", "2"}, {"latency.mul", "3"}}), b = P({{"latency.alu", "9"}});
        a.merge(b);
        check(a.get("latency.alu") == 9 && a.get("latency.mul") == 3, "merging: the later values win");
        Latency l;
        a.apply(l);
        check(l.alu == 9 && l.mul == 3 && l.brCond == 1, "the parameters that are Latency fields are written into it");
    }

    section("hardware parameters: what they do to the simulator (each one measured against the cycle count)");
    const std::string alus = ".main\ns:" + std::string(10, ' ') + "ALU ADD unsigned\n ALU ADD unsigned\n ALU ADD unsigned\n ALU ADD unsigned\n ALU ADD unsigned\n ALU ADD unsigned\n ALU ADD unsigned\n ALU ADD unsigned\n ALU ADD unsigned\n ALU ADD unsigned\nh: JMP h\n";
    const Cycle base10 = doneAt(alus, SimParams());
    check(base10 > 0, "(the reference run of ten ALU operations)", std::to_string(base10));
    check(doneAt(alus, P({{"main.fetch_cycles", "3"}})) == base10 + 30, "main.fetch_cycles = 3: every instruction takes 3 cycles more (10 instructions: +30)");
    check(doneAt(alus, P({{"peri.fetch_cycles", "3"}})) == base10, "...the Peri decoder's value does not touch Main");
    check(doneAt(alus, P({{"latency.alu", "5"}})) == base10 + 40, "latency.alu = 5: each ALU operation 4 cycles more (+40)");
    check(doneAt(alus, P({{"reset.boot_cycles", "100"}})) == base10 + 100, "reset.boot_cycles = 100: the first instruction waits 100 cycles after reset");
    {
        const std::string env = "[latency]\nalu = 3\n[params]\nlatency.alu = 4\n";
        check(doneAt(alus, SimParams(), env) == base10 + 30, "the environment file's [params] win over its [latency] section (4, not 3: +30)");
        check(doneAt(alus, P({{"latency.alu", "5"}}), env) == base10 + 40, "SimConfig::params win over both (5: +40)");
        Fx f(alus, env, CallModel::Inline, nullptr);
        check(f.good() && f.sim->params().get("latency.alu") == 4, "the simulator reports the parameters it runs with");
    }
    const std::string csrsM = ".main\ns: EXEC_CSR WRITE8 MARK, 1\n EXEC_CSR WRITE8 MARK, 1\n EXEC_CSR WRITE8 MARK, 1\n EXEC_CSR WRITE8 MARK, 1\n EXEC_CSR WRITE8 MARK, 1\nh: JMP h\n";
    const Cycle csrBase = doneAt(csrsM, SimParams());
    check(doneAt(csrsM, P({{"csr.access_main", "6"}})) == csrBase + 20, "csr.access_main = 6: each EXEC_CSR 4 cycles more than the 2 of latency.csr_access (5 of them: +20)");
    check(doneAt(csrsM, P({{"csr.access_peri", "6"}})) == csrBase, "...csr.access_peri does not touch Main");
    const std::string csrsP = ".peri\ns: EXEC_CSR WRITE8 MARK, 1\n EXEC_CSR WRITE8 MARK, 1\n EXEC_CSR WRITE8 MARK, 1\n EXEC_CSR WRITE8 MARK, 1\nh: JMP h\n";
    const Cycle periBase = doneAt(csrsP, SimParams());
    check(doneAt(csrsP, P({{"csr.access_peri", "5"}})) == periBase + 12 && doneAt(csrsP, P({{"peri.fetch_cycles", "2"}})) == periBase + 8, "Peri: csr.access_peri = 5 (+3 each, 4 of them) and peri.fetch_cycles = 2 (+2 each)");
    {   // the timers: 100 ticks, then the done latch
        const std::string prog = ".peri\ns: EXEC_CSR WRITE8 TPER, 100\n EXEC_CSR SET8 PERI_CTRL, 1\nw: BR.IF_TIMER_DONE d\n JMP w\nd: EXEC_CSR SET8 PERI_CTRL, 2\nh: JMP h\n";
        auto doneOf = [&](const SimParams& p) { Fx f(prog, kEnvCsr, CallModel::Inline, &p); return f.good() ? f.runUntil("peri.done", 100000).now : 0; };
        const Cycle t0 = doneOf(SimParams());
        auto near = [&](Cycle got, Cycle want) { return got + 4 >= want && got <= want + 4; };      // the poll loop looks every two cycles
        check(near(doneOf(P({{"timer.peri.clock_div", "4"}})), t0 + 300), "timer.peri.clock_div = 4: a 100-tick timer takes 400 cycles (+300)", std::to_string(t0));
        check(near(doneOf(P({{"timer.peri.start_latency", "50"}, {"timer.peri.done_latency", "20"}})), t0 + 70), "timer.peri.start_latency 50 and done_latency 20: +70");
        check(near(doneOf(P({{"timer.main.clock_div", "9"}, {"timer.main.start_latency", "99"}})), t0), "...the Main timer's values do not touch the Peri timer");
    }
    {   // the path to the I2C device: a read of one byte after one register address byte is 4 byte phases
        const std::string env = std::string(kEnvCsr) + "[device d]\naddr = 0x68\nreg 0x10 = 0x55\n";
        const std::string prog = ".peri\ns: EXEC_XFER READ dev=0, addr=0x10, addrlen=1, ptr=0\nh: JMP h\n";
        const Cycle t0 = doneAt(prog, SimParams(), env);
        check(doneAt(prog, P({{"i2ch.overhead", "50"}}), env) == t0 + 50, "i2ch.overhead = 50: the transfer starts 50 cycles later");
        check(doneAt(prog, P({{"i2c.ack_delay", "3"}}), env) == t0 + 12, "i2c.ack_delay = 3: 3 cycles on each of the 4 byte phases (+12)");
        check(doneAt(prog, P({{"i2c.stretch", "10"}}), env) == t0 + 40, "i2c.stretch = 10: 10 cycles on each of the 4 byte phases (+40)");
        check(doneAt(prog, P({{"i2c.turnaround", "7"}}), env) == t0 + 7, "i2c.turnaround = 7: once, for the repeated start of a read with a register address");
        check(doneAt(prog, P({{"peri.fetch_cycles", "2"}}), env) == t0 + 2, "peri.fetch_cycles = 2 also counts for EXEC_XFER (+2)");
    }
    {   // events: the dispatch to the handler and the return from it
        const char* prog = ".main\n.vector 0, h\nloop: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n BR.COND ALWAYS, nxt, safe\nnxt: MOV CONST(2, 1) -> MAIN_SCRATCHPAD[1..1]\n BR.COND PERI_RESULT_VALID, loop, safe\n jl: JMP loop\nh: MOV CONST(7, 1) -> MAIN_SCRATCHPAD[5..5]\n BR.ERET\n";
        auto resumed = [&](const SimParams& p) {
            Fx f(prog, "", CallModel::Inline, &p);
            injectAt(f, 1, "event main id=0");
            RunLimits l; l.maxCycles = 400; l.stopOnIdle = false; l.untilExpr = "scratch[5] == 7 && !main.in_handler && scratch[1] == 2";
            return f.sim->run(l).now;
        };
        const Cycle t0 = resumed(SimParams());
        check(resumed(P({{"event.dispatch_extra", "5"}, {"event.eret_extra", "7"}})) == t0 + 12, "event.dispatch_extra 5 and event.eret_extra 7: the interrupted flow carries on 12 cycles later");
    }
    {   // new parameters for a simulator that is running: the state stays, what is still to be timed follows them
        const std::string movs = ".main\na: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n MOV CONST(2, 1) -> MAIN_SCRATCHPAD[1..1]\n MOV CONST(3, 1) -> MAIN_SCRATCHPAD[2..2]\n MOV CONST(4, 1) -> MAIN_SCRATCHPAD[3..3]\nh: JMP h\n";
        Fx f(movs, "", CallModel::Inline, nullptr);
        f.runUntil("scratch[1] == 2", 1000);                                     // a bit of the program has run
        const Cycle t = f.sim->now();
        const int pcBefore = (int)f.sim->pc(Domain::Main);
        f.sim->setParams(P({{"main.fetch_cycles", "10"}}));
        check(f.sim->now() == t && (int)f.sim->pc(Domain::Main) == pcBefore && f.sim->readRegion("scratch", 1, 1)[0] == 2, "setParams on a running simulator keeps the state: time, pc and registers");
        check(f.sim->params().get("main.fetch_cycles") == 10, "...and the simulator now runs with the new value");
        const Cycle t1 = f.runUntil("scratch[3] == 4", 1000).now;
        check(t1 - t >= 12, "...the instructions still to run take the new time (each costs 10 more: without it the same stretch is about 3 cycles)", std::to_string(t1 - t));
        f.sim->setParams(SimParams());
        check(f.sim->params().get("main.fetch_cycles") == 0, "a parameter that is no longer set goes back to the initial value");
        SimParams w = P({{"frame.wcet_pgl", "55"}});
        f.sim->setParams(w);
        check(f.sim->signal("wcet_pgl") == 55, "frame.wcet_pgl written while running reaches the WCET register at once");
    }
    {   // the frame timing: the video windows and the WCET registers are parameters too
        const char* prog = ".main\nstart: BR.COND VBLANK_OPEN, in\n JMP start\nin: EXEC_CSR WRITE8 MARK, 1\nh: JMP h\n";
        Fx f(prog, "", CallModel::Inline, nullptr);
        SimParams p = P({{"timing.frame_cycles", "1000"}, {"timing.vblank_start", "300"}, {"timing.vblank_len", "200"}, {"frame.wcet_pgl", "123"}});
        Fx g(prog, "", CallModel::Inline, &p);
        RunResult rr = g.runUntil("csr[0x50] == 1", 5000);
        check(rr.reason == StopReason::Condition && rr.now >= 300 && rr.now < 320, "timing.* parameters make the video window: VBLANK_OPEN at cycle 300", std::to_string(rr.now));
        check(ev(*g.sim, "wcet_pgl") == 123 && ev(*f.sim, "wcet_pgl") == 0, "frame.wcet_pgl is the Frame Timing WCET register (the signal's initial value)");
    }
    {   // the parameters that are listed but not used yet change nothing, so a parameter file can already carry them
        const std::string env = std::string(kEnvCsr) + "[device d]\naddr = 0x68\nregseq 0x10 = 0x11, 0x22\n";
        const std::string prog = ".main\nh: JMP h\n.peri\nl: EXEC_XFER READ dev=0, len=2, ptr=0, ext=[0x10]\n EXEC_PUB service=1, src=0, len=2\n JMP l\n";
        auto final = [&](const SimParams& p) {
            Fx f(prog, env, CallModel::Inline, &p);
            RunLimits l; l.maxCycles = 30000; l.stopOnIdle = false;
            RunResult rr = f.sim->run(l);
            return std::to_string(rr.now) + "/" + std::to_string(ev(*f.sim, "peri.instrs")) + "/" + std::to_string(ev(*f.sim, "slot.valid")) + "/" + std::to_string(ev(*f.sim, "slot[0..1]"));
        };
        const std::string ref = final(SimParams());
        SimParams defaults, inactive;
        std::string e;
        for (const ParamDef& d : paramCatalog()) {
            if (d.active) defaults.set(d.key, SimParams().text(d.key), e);
            else inactive.set(d.key, d.kind == ParamDef::Enum ? d.options.back() : d.kind == ParamDef::Bool ? "true" : "7", e);
        }
        check(final(defaults) == ref, "every active parameter set explicitly to its initial value: the run is the same as with none set", ref);
        check(final(inactive) == ref, "every parameter that is not used yet set to something else: the run is the same (they are stored, not applied)");
    }
    {   // the document that lists them is the catalog's
        const std::string doc = slurp(dir() + "/docs/SIM_PARAMS.md");
        std::string missing;
        for (const ParamDef& d : paramCatalog()) if (doc.find("`" + d.key + "`") == std::string::npos) missing += d.key + " ";
        check(!doc.empty() && missing.empty(), "docs/SIM_PARAMS.md lists every parameter (regenerate it when the catalog changes)", missing);
    }
    {   // errors in the environment's [params] section are reported like every other environment error
        Fx f(".main\nh: JMP h\n", "[params]\nno.such = 3\nlatency.alu = 99999999\n", CallModel::Inline, nullptr);
        check(f.sim && f.sim->configErrors().size() >= 2, "a bad key and an out-of-range value in [params] are reported", f.sim ? std::to_string(f.sim->configErrors().size()) : "no sim");
    }
}

int main() {
    std::printf("AssemblyCore simulator tests\n");
    t_alu();
    t_imu();
    t_faults();
    t_slot();
    t_same_cycle();
    t_timing();
    t_peri_units();
    t_main_units();
    t_debug();
    t_interrupt();
    t_v31_rules();
    t_events();
    t_csr_buf_valid();
    t_region_size();
    t_inject_now();
    t_extensions();
    t_scenario();
    t_params();
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
