#pragma once
// Internals of the simulator. Nothing outside core/src/sim includes this file; sim.h is the only public surface.
//
// Layout of the implementation:
//   impl.cpp      construction, environment file, signals, conditions, CSR bus, bus/devices, timing, scheduler, run control
//   main_core.cpp Main instruction semantics (MOV, ALU, EXEC_CSR, BR)
//   peri_core.cpp Peri instruction semantics (EXEC_CSR, BR, EXEC_PUB, EXEC_XFER)
//   debug.cpp     breakpoints, watches, call stack, snapshots/history, state dump, trace and VCD sinks
//   scenario.cpp  scenario scripts
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../util.h"
#include "asmcore/sim.h"
#include "../image_view.h"
#include "ini.h"
#include "sexpr.h"

namespace asmcore {
namespace sim {

inline int di(Domain d) { return d == Domain::Main ? 0 : 1; }
const Cycle kNever = ~(Cycle)0;

// ---- execution state: ONE copyable value (Snapshot = a copy of this) -------------------------------------------------------
struct MainState {
    uint32_t pc = 0;
    uint8_t scr[32] = {}, buf[32] = {};
    uint32_t bufOk = 0;                // csr_buf_valid: bit i = MAIN_CSR_DATA_BUF[i] holds the result of a completed READ (only used when the profile has it)
    uint16_t opa = 0, opb = 0;
    uint32_t out = 0;
    bool cmp = false, zero = false, neg = false, carry = false, borrow = false, ovs = false, ov16 = false;
    bool cascActive = false;
    uint8_t cascSel = 0, cascStep = 0;
    Cycle readyAt = 0;
    bool halted = false, faulted = false;
    uint64_t instrs = 0;
};
struct XferInFlight {
    bool active = false, read = true;
    int dev = 0, addrLen = 0, len = 1, ptr = 0;
    uint32_t addr = 0;
    uint8_t data = 0;
};
struct PeriState {
    uint32_t pc = 0;
    uint8_t rx[32] = {};
    Cycle readyAt = 0;
    bool halted = false, faulted = false;
    uint64_t instrs = 0;
    XferInFlight xfer;
};
struct EventState {                                     // per-domain event controller (spec v31 16.2)
    uint64_t pending = 0;
    bool active = false;                                // handler_active
    bool resumeValid = false;
    uint32_t resumePc = 0;                              // full width
    int cause = -1;
    Cycle raisedAt[64] = {};
};
struct SlotState {
    bool valid = false, busy = false, overrun = false;
    uint8_t svc = 0;
    uint8_t payload[32] = {};
    Cycle clearedAt = kNever;      // when Main last cleared valid (spec 12.6: no same-cycle reuse)
};
struct TimedEvent {
    enum Kind { SetSignal, SlotConsumeDone, XferDone, FrameEnd, WindowEdge, Command, SlotBusyEnd };
    Cycle t = 0;
    uint64_t seq = 0;
    Kind kind = SetSignal;
    int a = 0;
    int64_t b = 0;
    std::string s;                 // token (SetSignal) or command text (Command)
};
struct State {
    Cycle now = 0;
    uint64_t seq = 0;
    MainState m;
    PeriState p;
    SlotState slot;
    EventState ev[2];
    std::vector<EventRecord> evlog;
    std::vector<int64_t> sig;                          // stored signals
    std::map<uint32_t, uint8_t> csr;                   // stored CSR bytes
    std::vector<std::vector<int64_t>> dev;             // per-device state
    std::map<int, int> nack;                           // I2C address -> NACKs still to inject
    std::vector<TimedEvent> q;
    std::vector<CallFrame> shadow[2];
    bool hasFault[2] = {false, false};
    Fault fault[2];
};

// ---- environment model ---------------------------------------------------------------------------------------------------------
struct SigDef {
    std::string name;
    int stored = -1;                                   // index into State::sig, or -1 for a computed signal
    int64_t init = 0;
};

struct CsrDef {
    std::string name;
    uint32_t addr = 0;
    std::string kind = "ram";                          // ram | value | action | status | const
    int reset = 0;
    std::string bind;                                  // value: signal name
    int bytes = 1;                                     // value: how many CSR bytes of the signal this register spans
    int64_t constVal = 0;
    std::string bitAction[8];                          // action: "name[:arg]"
    std::string bitExpr[8];                            // status: expression (source text)
};

struct Cond {
    bool defined = false;
    std::string name;
    Expr e;
};

struct TimingCfg {
    int64_t frameCycles = 0, vStart = 0, vLen = 0, lineCycles = 0, hStart = 0, hLen = 0;
};

struct DevInst {
    DeviceSpec spec;
    std::unique_ptr<I2cDevice> dev;
};

struct WatchDef {
    int id = 0;
    std::string target, condition;
    // what is watched: a byte range of a region, a CSR, or a general expression
    enum Kind { Region, Csr, Expression } kind = Expression;
    std::string region;
    int off = 0, len = 1;
    uint32_t addr = 0;
    Expr expr, cond;
    bool haveCond = false;
    std::vector<uint8_t> last;
    int64_t lastVal = 0;
};

struct WriteRec {                                      // one data write done by the instruction that just ran
    std::string region;
    int off, len;
};

// What an opcode handler is given and what it may change about how the instruction retires.
struct OpCtx {
    uint32_t pc = 0;
    int arg = 0;                   // instruction byte [5:0]
    uint32_t next = 0;             // pc after the instruction (default pc + 1)
    int latency = 1;               // cycles until the domain can issue again (< 0: the handler arranged the wake-up itself)
    bool skipAdvance = false;      // the handler finished the instruction on its own (halt)
    bool checkpoint = false;       // an ordinary BR with event_safe=1 retires: a pending event may be dispatched now (v31 16.3)
};

struct Simulator::Impl : public ExprBinder, public ExprEnv, public ActionContext {
    // ---- immutable after construction
    SimConfig cfg;
    Extensions ext;
    Profile pf;
    Latency lat;
    ImageView img[2];
    std::vector<InstrInfo> instrs;
    std::vector<CallSiteInfo> callSites;
    std::map<std::string, uint32_t> labels;            // "main:name" -> pc
    std::map<std::string, uint32_t> csrSymbols;
    std::map<uint32_t, const InstrInfo*> instrAt[2];
    std::map<uint32_t, const CallSiteInfo*> callByTransfer[2];
    std::vector<std::string> errors;
    bool softret = false;                              // call model of the assembled program
    int64_t vecPc[2][64];                              // event vector per domain and id (-1: none)

    std::vector<SigDef> sigs;
    std::map<std::string, int> sigIndex;
    int nStored = 0;
    std::map<uint32_t, CsrDef> csrDefs;
    std::map<uint32_t, uint32_t> csrOwner;             // address -> base address of the def that spans it
    bool unmappedFault = false;
    Cond cond[2][16];
    TimingCfg timing;
    std::vector<DevInst> devs;
    std::map<int, int> devIndexByAddr;
    int devMapCount = 8;
    std::vector<std::pair<Cycle, std::string>> envInjects;
    bool tieMainFirst = true;
    bool faultStopsAll = false;
    // ---- hardware parameters (sim_params.h): read once from `params` by applyParams()
    SimParams params;
    int fetchExtra[2] = {0, 0};                        // extra cycles per instruction of each decoder
    int csrAccessOf[2] = {-1, -1};                     // one CSR access as each decoder sees it (-1: lat.csrAccess)
    int bootCycles = 0;
    int eventDispatchExtra = 0, eretExtra = 0;
    int timerDiv[2] = {1, 1}, timerStart[2] = {0, 0}, timerDone[2] = {0, 0};     // [0] Main timer, [1] Peri timer
    int i2chOverhead = 0, i2cAck = 0, i2cStretch = 0, i2cTurnaround = 0;
    void applyParams();
    Latency latFor(int domain) const { Latency l = lat; if (csrAccessOf[domain] >= 0) l.csrAccess = csrAccessOf[domain]; return l; }
    int64_t xferExtra(bool read, int addrLen, int dataLen) const {         // the I2C path's extras on top of Latency::xfer
        const int phases = Latency::xferPhases(read, addrLen, dataLen);
        return (int64_t)i2chOverhead + (int64_t)phases * (i2cAck + i2cStretch) + ((read && addrLen > 0) ? i2cTurnaround : 0);
    }
    int64_t param(const std::string& key) const override { return params.get(key); }
    uint32_t constSize = 0;

    // ---- mutable
    State st;
    std::vector<std::shared_ptr<EventSink>> sinks;
    std::vector<Breakpoint> bps;
    std::vector<WatchDef> watches;
    int nextId = 1;
    std::vector<WriteRec> lastWrites;
    std::deque<std::pair<Cycle, std::shared_ptr<const State>>> history;
    Cycle histInterval = 0, nextSnap = 0;
    size_t histKeep = 0;
    bool suppressStops = false;                        // rewinding: breakpoints off
    bool stopRequested = false;
    std::function<bool()> cancelHook;                  // Simulator::setCancelHook
    bool skipValid = false;                            // resume from a breakpoint without stopping on it again
    int skipDomain = 0;
    uint32_t skipPc = 0;
    StopReason stopWhy = StopReason::None;
    int stopId = -1;
    std::string stopDetail;

    Impl(const Result& r, const SimConfig& c, const Extensions* e);

    // ---- construction helpers (impl.cpp)
    void defStored(const std::string& name, int64_t init);
    void defComputed(const std::string& name, std::function<int64_t(const Impl&)> get, std::function<void(Impl&, int64_t)> set = nullptr);
    void buildSignals();
    void buildDefaultConditions();
    void loadEnv(const std::string& text);
    bool buildCsr(const IniSection& s);
    bool buildDevice(const IniSection& s);
    void resetState();
    std::vector<std::function<int64_t(const Impl&)>> getters;      // computed signal getters (parallel to sigs)
    std::vector<std::function<void(Impl&, int64_t)>> setters;

    // ---- signals
    int sigId(const std::string& name) const { auto it = sigIndex.find(name); return it == sigIndex.end() ? -1 : it->second; }
    int64_t sigGet(int id) const;
    void sigSet(int id, int64_t v);
    // ActionContext
    Cycle now() const override { return st.now; }
    int64_t signal(const std::string& name) const override;
    void setSignal(const std::string& name, int64_t value) override;
    void setSignalAfter(Cycle delay, const std::string& name, int64_t value) override;
    void setSignalAfterGuarded(Cycle delay, const std::string& name, int64_t value, const std::string& guardSignal, int64_t guardValue) override;
    void note(const std::string& text) override;

    // ---- expressions (ExprBinder / ExprEnv)
    int bind(const std::string& name, bool indexed) override;
    int64_t get(int handle, int64_t idx, int len) const override;
    bool compile(const std::string& text, Expr& out, std::string& err);

    // ---- events
    template <class F> void emit(F&& build) {
        if (sinks.empty()) return;
        Event e;
        e.t = st.now;
        build(e);
        for (auto& s : sinks) s->onEvent(e);
    }
    void emitNote(const std::string& text);

    // ---- data regions and CSRs
    uint8_t* regionPtr(Domain d, const std::string& region, int& size);
    bool regionBytes(const std::string& region, int off, int len, std::vector<uint8_t>& out) const;
    void noteWrite(const std::string& region, int off, int len);
    uint8_t csrRead8(uint32_t addr, Domain by);
    void csrWrite(uint32_t addr, uint8_t v, int mode /*0 write, 1 set, 2 clr*/, Domain by);
    void runAction(const std::string& spec);
    uint8_t aluStatusByte() const;

    // ---- faults
    void raise(Domain d, uint32_t pc, const std::string& code, const std::string& msg);

    // ---- queue
    void schedule(Cycle t, TimedEvent::Kind k, int a = 0, int64_t b = 0, const std::string& s = "");
    void handleTimed(const TimedEvent& e);
    void applyCommand(const std::string& cmd);

    // ---- execution (main_core.cpp / peri_core.cpp)
    // One entry per opcode ([7:6] of the instruction byte). The spec's tables are the defaults (registerOps); replace an entry
    // to change what an opcode does, or fill an unused one.
    std::function<void(OpCtx&)> mainOps[4], periOps[4];
    void registerOps();
    void opMainCsr(OpCtx&); void opMainBr(OpCtx&); void opMainMov(OpCtx&); void opMainAlu(OpCtx&);
    void opPeriCsr(OpCtx&); void opPeriBr(OpCtx&); void opPeriPub(OpCtx&); void opPeriXfer(OpCtx&);
    void stepMain();
    void stepPeri();
    void xferComplete();
    void raiseEvent(int domain, int id);
    void eventCheckpoint(int domain);                  // after an event_safe ordinary BR retired: dispatch a pending event if every guard holds
    bool doEret(int domain, OpCtx& oc);                // BR.ERET; false on a fault
    bool fetchOk(Domain d, uint32_t pc);

    // ---- scheduling / running (impl.cpp)
    Cycle nextTime(int& who) const;                    // who: 0 main, 1 peri, 2 timed event, -1 nothing
    bool advance();                                    // executes the next action; false if nothing can run
    void afterInstruction(Domain d, uint32_t pcBefore);
    bool checkStops(Domain d, uint32_t pcBefore, const RunLimits* lim, const Expr* until);
    RunResult runLoop(const RunLimits& lim, int onlyDomain, bool overCall, uint32_t overReturnPc, size_t overDepth);

    // ---- debug (debug.cpp)
    void trackCalls(Domain d, uint32_t pcBefore);
    std::vector<CallFrame> stack(Domain d) const;
    std::string dump() const;
    void takeSnapshotIfDue();
    void primeWatches();
    bool evalWatches(int& id, std::string& detail);
    bool evalBreakpoint(Domain d, uint32_t pc, int& id);
    bool evalBool(const Expr& e) const { return e.eval(*this) != 0; }
};

}  // namespace sim
}  // namespace asmcore
