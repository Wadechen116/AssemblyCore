#pragma once
// AssemblyCore simulator: a behavioural model of the Main / Peri sequencers, the objects they share, and a configurable
// external environment (CSRs, I2C devices, video timing). Standard C++17 only.
//
// Design for change (the spec is still being settled, so nothing here is hard-wired to one reading of it):
//   * Simulator is a pimpl facade; everything behind it can be restructured without touching clients.
//   * Branch conditions are DATA: a table of  condition code -> expression over named signals  (spec 7.3, 11.2 are the defaults).
//   * CSR side effects are DATA: a CSR is a kind (ram / value / action / status / const) plus a bit -> action map, configured in
//     the environment file; actions are named and can be added through Extensions.
//   * I2C devices are plug-ins (Extensions::registerDevice); the built-in "scripted" device is configured from text.
//   * Everything the simulator does is announced through EventSink objects (Observer): tracing, VCD, coverage and the debugger
//     are all just sinks.
//   * All execution state lives in one copyable value (Snapshot): rewind, state diffs and "run the same thing twice" are free.
//   * Latency comes from asmcore::Latency, the same model the assembler uses for WCET.
//
// See docs/SIM.md for the environment file format and for every place where the spec was silent and a choice was made.
#include "asmcore/sim_params.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"

namespace asmcore {
namespace sim {

using Cycle = uint64_t;

// ---- events (Observer) -----------------------------------------------------------------------------------------------------
enum class EventType {
    Issue,        // an instruction starts: a = length in bytes
    Retire,       // its effects are complete
    RegionWrite,  // a data region changed: what = region name, a = offset, b = length
    CsrAccess,    // what = "read" | "write" | "set" | "clr", a = address, b = value
    Signal,       // a named signal changed: what = name, a = old, b = new
    Bus,          // an external transaction finished: what = "i2c.read" ..., a = device address, b = ack(1)/nack(0), c = bytes
    Publish,      // EXEC_PUB accepted: a = service_id, b = length
    Consume,      // MOV consumed the result slot: a = service_id
    Overrun,      // EXEC_PUB dropped (slot full or busy)
    Fault,        // what = fault code, text = message
    Inject,       // an injected environment command was applied
    Note,         // free text (scenario `log`, breakpoints...)
    HwEvent       // hardware event (spec v31 16): what = "raise" | "dispatch" | "eret", a = event id, b = resume PC (dispatch) / handler cycles (eret)
};
struct Event {
    Cycle t = 0;
    EventType type = EventType::Note;
    int domain = -1;              // 0 Main, 1 Peri, -1 system
    uint32_t pc = 0;
    std::string what, text;
    int64_t a = 0, b = 0, c = 0;
};
class EventSink {
public:
    virtual ~EventSink() {}
    virtual void onEvent(const Event& e) = 0;
};

// ---- extension points ------------------------------------------------------------------------------------------------------
// What an action (the effect of one bit of a CSR) is allowed to touch.
class ActionContext {
public:
    virtual ~ActionContext() {}
    virtual Cycle now() const = 0;
    virtual int64_t signal(const std::string& name) const = 0;
    virtual void setSignal(const std::string& name, int64_t value) = 0;
    virtual void setSignalAfter(Cycle delay, const std::string& name, int64_t value) = 0;
    // Like setSignalAfter, but the change only happens if `guardSignal` still has `guardValue` when the time comes. Bump the guard
    // signal to cancel everything that was scheduled against the old value (restartable timers).
    virtual void setSignalAfterGuarded(Cycle delay, const std::string& name, int64_t value, const std::string& guardSignal, int64_t guardValue) = 0;
    virtual void note(const std::string& text) = 0;
    virtual int64_t param(const std::string&) const { return 0; }       // a hardware parameter (sim_params.h) as the simulator is running with it
};
using ActionFn = std::function<void(ActionContext& ctx, const std::string& arg)>;

// A device on an I2C bus. All mutable state lives in `state` (the simulator keeps it inside its snapshots), never in the object.
struct I2cReply {
    bool ack = true;
    std::vector<uint8_t> data;    // for reads
    int extraCycles = 0;          // device-side delay added to the bus time
};
class I2cDevice {
public:
    virtual ~I2cDevice() {}
    virtual void init(std::vector<int64_t>& /*state*/) {}
    // `reg` is the register address sent after address+W (addrLen bytes, 0 = none). Reads return `len` bytes.
    virtual I2cReply read(uint32_t reg, int addrLen, int len, std::vector<int64_t>& state, Cycle now) = 0;
    virtual I2cReply write(uint32_t reg, int addrLen, const std::vector<uint8_t>& data, std::vector<int64_t>& state, Cycle now) = 0;
};
// The configuration lines of one [device ...] section: key -> every value given for that key, in order.
struct DeviceSpec {
    std::string name;
    int addr = 0;
    std::map<std::string, std::vector<std::string>> keys;
};
using DeviceFactory = std::function<std::unique_ptr<I2cDevice>(const DeviceSpec&, std::string& error)>;

// Registries handed to a Simulator. defaults() has the built-in actions and the "scripted" device.
class Extensions {
public:
    static Extensions defaults();
    void registerAction(const std::string& name, ActionFn fn) { actions_[name] = std::move(fn); }
    void registerDevice(const std::string& kind, DeviceFactory f) { devices_[kind] = std::move(f); }
    const std::map<std::string, ActionFn>& actions() const { return actions_; }
    const std::map<std::string, DeviceFactory>& devices() const { return devices_; }
private:
    std::map<std::string, ActionFn> actions_;
    std::map<std::string, DeviceFactory> devices_;
};

// ---- configuration ---------------------------------------------------------------------------------------------------------
struct SimConfig {
    Profile profile;              // layout assumptions (offset width, byte order) and latencies
    std::string env;              // environment file text (docs/SIM.md); may be empty
    SimParams params;             // hardware parameters (sim_params.h): these win over the environment file's sections and its [params]
};

// ---- faults, stops, breakpoints --------------------------------------------------------------------------------------------
struct Fault {
    Domain domain = Domain::Main;
    uint32_t pc = 0;
    Cycle at = 0;
    std::string code;             // e.g. "fault_cascade_step_mismatch"
    std::string msg;
};
// One hardware event from detection to the return into the interrupted code (spec v31 16.9 keeps these intervals apart).
struct EventRecord {
    Domain domain = Domain::Main;
    int id = 0;
    Cycle raisedAt = 0;           // became pending
    Cycle dispatchedAt = 0;       // the event_safe BR retired and the redirect to the vector happened
    Cycle returnedAt = 0;         // BR.ERET retired
    uint32_t resumePc = 0;        // the interrupted flow's next PC (full width)
    uint32_t vectorPc = 0;
    bool dispatched = false, returned = false;
    Cycle waited() const { return dispatched ? dispatchedAt - raisedAt : 0; }       // pending -> checkpoint
    Cycle handled() const { return returned ? returnedAt - dispatchedAt : 0; }      // dispatch -> ERET
};
enum class StopReason { None, CycleLimit, InstrLimit, Breakpoint, Watchpoint, Fault, Idle, Condition, Cancelled, TimeLimit };
struct RunLimits {
    Cycle maxCycles = 1000000;    // relative to the cycle at which run() starts; 0 = unlimited
    uint64_t maxInstrs = 0;       // 0 = unlimited
    std::string untilExpr;        // stop when this expression becomes true (checked after every instruction)
    bool stopOnIdle = true;       // stop when neither domain can ever make progress again
    // Run-away protection for interactive use. Both are polled every few hundred events, so a stop is prompt but not instruction-exact.
    uint32_t maxMillis = 0;       // wall-clock limit of this call, 0 = none -> StopReason::TimeLimit
    std::function<bool()> cancel; // return true to stop -> StopReason::Cancelled (called from the thread that calls run())
};
struct RunResult {
    StopReason reason = StopReason::None;
    Cycle now = 0;
    int id = -1;                  // breakpoint / watchpoint id
    std::string detail;
};
struct Breakpoint {
    int id = 0;
    Domain domain = Domain::Main;
    uint32_t pc = 0;
    std::string condition;        // optional expression
    bool enabled = true;
    bool oneShot = false;
    uint64_t hits = 0;
};
struct CallFrame {
    std::string proc;
    SrcLoc callSite;
    uint32_t returnPc = 0;
    bool inlined = false;         // inline model: reconstructed from debug info, there is no runtime frame
};

class Snapshot {
public:
    Cycle time() const;
    explicit operator bool() const { return (bool)data_; }
private:
    friend class Simulator;
    std::shared_ptr<const void> data_;
};

class Simulator {
public:
    // Images and debug information are copied: `assembled` does not have to outlive the Simulator.
    Simulator(const Result& assembled, const SimConfig& cfg, const Extensions* ext = nullptr);
    ~Simulator();
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    bool ok() const;                                        // false if the environment file had errors
    const std::vector<std::string>& configErrors() const;
    const SimParams& params() const;                        // the parameters the simulator runs with (environment file [params] and SimConfig::params merged)
    // New hardware parameters for a simulator that is running: nothing is reset, what is still to be timed follows the new numbers (the Frame Timing WCET
    // registers are written). A parameter that is no longer set goes back to what the environment file says.
    void setParams(const SimParams& p);
    // The video timing the simulator runs with (environment [timing] and the timing.* parameters); 0 = not set.
    struct FrameTiming { int64_t frameCycles = 0, vblankStart = 0, vblankLen = 0, lineCycles = 0, hblankStart = 0, hblankLen = 0; };
    FrameTiming frameTiming() const;
    void reset();                                           // power-on state, time 0, injections of the environment file re-armed

    // ---- running
    RunResult run(const RunLimits& limits);
    RunResult step(Domain d);                               // that domain's next instruction (anything due before it happens first)
    RunResult stepOver(Domain d);                           // like step, but a CALL is run to its return
    RunResult stepOut(Domain d);                            // run until the current procedure returns (outside any procedure: like step)
    // Polled by run / step / stepOver / stepOut in addition to RunLimits::cancel; lets another thread interrupt any of them.
    void setCancelHook(std::function<bool()> hook);
    RunResult runToPc(Domain d, uint32_t pc);
    Cycle now() const;

    // ---- state
    uint32_t pc(Domain d) const;
    bool halted(Domain d) const;                            // spinning on an unconditional branch to itself
    bool faulted(Domain d) const;
    const Fault* fault(Domain d) const;
    uint64_t instructions(Domain d) const;
    // ---- hardware events (inject "event main|peri id=N")
    bool inHandler(Domain d) const;                         // handler_active
    uint64_t pendingEvents(Domain d) const;                 // bit i = event i is pending
    std::vector<EventRecord> eventLog() const;              // every event so far, oldest first
    // Regions: scratch, csrbuf, opa, opb, out, status, rx, slot, meta, const
    int regionSize(const std::string& region) const;        // bytes, or -1 for an unknown region (const depends on the program)
    std::vector<uint8_t> readRegion(const std::string& region, int off, int len) const;
    bool writeRegion(const std::string& region, int off, const std::vector<uint8_t>& bytes);
    int64_t signal(const std::string& name) const;
    bool setSignal(const std::string& name, int64_t value);
    std::vector<std::string> signalNames() const;
    uint8_t csr(uint32_t addr) const;                       // stored value (ram CSRs); side-effect free
    bool lookupLabel(Domain d, const std::string& name, uint32_t& pc) const;   // a label of the assembled program
    bool eval(const std::string& expr, int64_t& value, std::string& error) const;   // expressions over everything above

    // ---- debugging
    int addBreakpoint(Domain d, uint32_t pc, const std::string& condition = "");
    bool removeBreakpoint(int id);
    std::vector<Breakpoint> breakpoints() const;
    // target: "scratch[8..9]" | "csrbuf[0]" | "csr 0x120" | "signal slot.valid" | any expression (watched for change)
    int addWatch(const std::string& target, const std::string& condition, std::string& error);
    bool removeWatch(int id);
    std::vector<CallFrame> callStack(Domain d) const;       // innermost first
    std::string dumpState() const;
    static std::string diffDumps(const std::string& before, const std::string& after);
    Snapshot snapshot() const;
    void restore(const Snapshot& s);
    // Keep a snapshot every `interval` cycles (at most `keep` of them); rewindTo restores the newest one at or before the target and
    // runs forward (the model is deterministic, injections are part of the state).
    void enableHistory(Cycle interval, size_t keep);
    bool rewindTo(Cycle t);

    // ---- environment control
    // Commands: "nack dev=0 count=1", "signal NAME=value", "slot_busy cycles=N", "fault_clear main|peri", ... (docs/SIM.md)
    bool inject(Cycle at, const std::string& command, std::string& error);
    bool injectNow(const std::string& command, std::string& error);          // apply at the current cycle, before anything else runs
    void addSink(std::shared_ptr<EventSink> sink);

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// ---- ready-made sinks ------------------------------------------------------------------------------------------------------
std::shared_ptr<EventSink> makeTextTrace(std::string* out, bool instructionsOnly = false);   // appends lines to *out
std::shared_ptr<EventSink> makeCsvTrace(std::string* out);
// Value-change dump of pc / status signals; call finish() after the run and take the text from *out.
class VcdSink : public EventSink {
public:
    explicit VcdSink(std::string* out);
    void onEvent(const Event& e) override;
    void finish(Cycle end);
private:
    struct Rep;
    std::shared_ptr<Rep> r_;
};

// ---- scenarios -------------------------------------------------------------------------------------------------------------
struct ScenarioResult {
    bool pass = true;
    int checks = 0, failures = 0;
    std::vector<std::string> log;
};
// A scenario command added by the caller: receives the words of the line (the command name first); returns false to fail the line.
using ScenarioCommand = std::function<bool(Simulator& sim, const std::vector<std::string>& words, std::string& message)>;
// A scenario is a text script (docs/SIM.md): run / step / break / inject / expect / dump / log ...
// `extra` adds commands (lower-case names); a built-in command of the same name wins.
ScenarioResult runScenario(Simulator& sim, const std::string& script, const std::map<std::string, ScenarioCommand>* extra = nullptr);

}  // namespace sim
}  // namespace asmcore
