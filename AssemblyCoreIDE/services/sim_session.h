#pragma once
// The simulator, run for a UI. A Simulator is not thread-safe and a run can take a long time, so it lives on one worker thread; the
// owner thread (the UI) sends commands and receives immutable views of the state.
//
//   owner thread                         worker thread
//   run() / step() / pause() ...  --->   command queue  --->  Simulator
//   updated(SimUpdate)            <---   dispatcher     <---  views + events, at most ~30 per second while running
//
// While a run is in progress it is cut into short slices, so the UI keeps getting fresh states and pause() takes effect at once.
// Every run has a cycle budget and a wall-clock budget (RunSpec), so a program that never ends cannot hang the IDE.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "asmcore/sim.h"
#include "build.h"
#include "dispatcher.h"
#include "signal.h"

namespace acide {

struct WatchValue {
    std::string expr;
    bool ok = false;
    int64_t value = 0;
    std::string error;
};

struct BreakpointSpec {
    asmcore::Domain domain = asmcore::Domain::Main;
    uint32_t pc = 0;
    std::string condition;
    bool operator==(const BreakpointSpec& o) const { return domain == o.domain && pc == o.pc && condition == o.condition; }
};

// What the UI sees of the simulator at one moment. Immutable; replaced as a whole.
struct SimView {
    uint64_t seq = 0;
    asmcore::sim::Cycle cycle = 0;
    uint32_t pc[2] = {0, 0};                                   // [0] Main, [1] Peri
    bool halted[2] = {false, false}, faulted[2] = {false, false};
    uint64_t instrs[2] = {0, 0};
    bool hasFault[2] = {false, false};
    asmcore::sim::Fault fault[2];
    std::map<std::string, std::vector<uint8_t>> regions;       // scratch csrbuf rx slot meta opa opb out status const
    std::vector<std::pair<std::string, int64_t>> signalValues;
    std::vector<std::pair<std::string, uint8_t>> csrs;          // the CSRs the program names (.csr)
    std::vector<asmcore::sim::CallFrame> stack[2];              // innermost first
    std::vector<asmcore::sim::Breakpoint> breakpoints;
    std::vector<WatchValue> watches;                           // the expression list set with setWatchExprs
    asmcore::Domain domainOf(int i) const { return i == 0 ? asmcore::Domain::Main : asmcore::Domain::Peri; }
};
using ViewPtr = std::shared_ptr<const SimView>;

enum class SimState { Empty, Paused, Running, Failed };

struct StopInfo {
    bool valid = false;                                        // a command finished with this outcome
    asmcore::sim::StopReason reason = asmcore::sim::StopReason::None;
    int id = -1;                                               // breakpoint / watchpoint id
    std::string detail;
    asmcore::sim::Cycle now = 0;
};

struct SimUpdate {
    ViewPtr view;
    SimState state = SimState::Empty;
    StopInfo stop;
    std::vector<asmcore::sim::Event> events;                   // new since the previous update (the newest ones if there were very many)
    size_t droppedEvents = 0;
    bool eventsReset = false;                                  // the history was rewound or reset: forget the earlier events
    std::vector<std::string> messages;                         // problems with a request (a bad watch...)
};

struct RunSpec {
    asmcore::sim::Cycle maxCycles = 50000000;                  // 0 = unlimited
    uint32_t maxMillis = 15000;                                // wall clock; 0 = unlimited
    std::string until;                                         // stop when this expression becomes true
    bool stopOnIdle = true;
};

class SimSession {
public:
    explicit SimSession(std::shared_ptr<IDispatcher> dispatcher);
    ~SimSession();
    SimSession(const SimSession&) = delete;
    SimSession& operator=(const SimSession&) = delete;

    // ---- owner-thread API (all asynchronous)
    // Starts a simulator for a built program. The program snapshot stays referenced: it is what the pcs of every view refer to.
    void load(SnapshotPtr program, const std::string& environmentText, const asmcore::Profile& profile, const asmcore::SimParams& params = asmcore::SimParams());
    void unload();
    void run(const RunSpec& spec = RunSpec());
    void pause();
    void step(asmcore::Domain d);
    void stepOver(asmcore::Domain d);
    void stepOut(asmcore::Domain d);
    void runToPc(asmcore::Domain d, uint32_t pc, const RunSpec& spec = RunSpec());
    void reset();
    void rewindTo(asmcore::sim::Cycle t);
    void setBreakpoints(const std::vector<BreakpointSpec>& v);           // the complete desired set; kept across load()
    void setDataWatches(const std::vector<std::string>& targets);        // "scratch[8..9]" | "csr 0x120" | "signal x" | expression
    void setWatchExprs(const std::vector<std::string>& exprs);
    void writeRegion(const std::string& region, int off, const std::vector<uint8_t>& bytes);
    void setSignal(const std::string& name, int64_t value);
    void command(const std::string& injectCommand);                      // "csr 0x120=5", "nack dev=0 count=1", "fault_clear main" ...
    // text | csv | vcd of the events kept so far; `done` runs on the owner thread
    void exportTrace(const std::string& format, std::function<void(const std::string&)> done);

    SimState state() const { return state_; }                            // as of the last update
    SnapshotPtr program() const { return program_; }
    ViewPtr view() const { return view_; }

    Signal<bool, const std::vector<std::string>&> loaded;                // ok, configuration errors
    Signal<const SimUpdate&> updated;

private:
    struct Worker;
    void post(std::function<void(Worker&)> fn);
    void loop();
    std::shared_ptr<IDispatcher> disp_;
    std::unique_ptr<Worker> w_;
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void(Worker&)>> q_;
    bool stop_ = false;
    std::atomic<bool> pause_{false};
    std::atomic<bool> running_{false};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    // owner-thread mirror
    SimState state_ = SimState::Empty;
    SnapshotPtr program_;
    ViewPtr view_;
};

}  // namespace acide
