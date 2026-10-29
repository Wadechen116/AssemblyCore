#pragma once
// Runs a program in a simulator of its own (no session, no debugger) and keeps what happened as a trace, and compares two traces. This is what shows,
// when a hardware parameter is changed a little, WHERE and HOW MUCH the two sequencers move (docs/SIM_VISUAL_DESIGN.md). No Qt.
#include <cstdint>
#include <string>
#include <vector>

#include "asmcore/sim.h"

namespace acide {

struct TraceRow {                       // one instruction of one sequencer; it lasts until the next row of the same sequencer (or the end of the run)
    int domain = 0;                     // 0 Main, 1 Peri
    uint32_t pc = 0;
    asmcore::sim::Cycle at = 0;         // the cycle it was issued
    std::string what;                   // "MOV", "EXEC_XFER" ...
    int len = 0;                        // bytes of code
};
struct TraceMarker {                    // something that happens between the sequencers or outside them
    asmcore::sim::Cycle at = 0;
    int domain = -1;
    // 'e' hardware event (what = raise / dispatch / eret, a = id), 'b' bus transaction ended (a = device address, b = acknowledged, c = bytes), 'p' publish (a = service,
    // b = length), 'c' slot consumed (a = service), 'o' overrun, 'f' fault (what = code), 's' a timer (re)started (what = "timer" Peri's / "main_timer" Main's),
    // 'd' a timer is done, 'v' / 'w' the vertical blanking window opens / closes, 'z' a decoder waits for a shared resource (what = csr | xfer, a = cycles)
    char kind = 'n';
    std::string what;
    int64_t a = 0, b = 0, c = 0;
    bool hasPc = false;                 // the instruction that caused it is known
    uint32_t pc = 0;
};

// The marker an event stands for; false for the events that are not markers (instructions, retirements, CSR traffic ...).
bool markerOf(const asmcore::sim::Event& e, TraceMarker& m);
struct RunTrace {
    std::vector<TraceRow> rows;
    std::vector<TraceMarker> marks;
    asmcore::sim::Cycle endAt = 0;      // where the run stopped
    asmcore::sim::StopReason reason = asmcore::sim::StopReason::None;
    int64_t instrs[2] = {0, 0};
    bool truncated = false;             // more rows than the limit: the rest is not kept
    std::string error;                  // the simulator could not be made (environment errors ...)
    bool ok() const { return error.empty(); }
    // the cycle where row `i` of a domain ends: the next row of the same domain, else the end of the run
    asmcore::sim::Cycle endOf(size_t i) const;
};

// Runs `program` with `cfg` (its environment and parameters) for up to `maxCycles` and records both sequencers. The run also stops when both are done, and after a
// wall-clock limit (a program that polls for ever is cut at maxCycles anyway).
RunTrace recordRun(const asmcore::Result& program, const asmcore::sim::SimConfig& cfg, asmcore::sim::Cycle maxCycles, size_t maxRows = 20000);

// Markers that belong together, as one thing with a start and an end: what the time line connects.
struct TraceLink {
    enum Kind { Result, EventWait, EventHandler, Timer, Blanking } kind = Result;
    int domain = -1;                    // the lane: 0 Main, 1 Peri (a Result goes from Peri to Main; Blanking belongs to neither)
    asmcore::sim::Cycle from = 0, to = 0;
    int fromMark = -1, toMark = -1;     // indexes into RunTrace::marks (toMark -1: not over yet, `to` is the end of the trace)
    bool open = false;                  // not over yet
    bool lost = false;                  // a Result that was overwritten, a timer that was started again before it was done
    int64_t a = 0;                      // service / event id
    std::string what;
    asmcore::sim::Cycle length() const { return to - from; }
};
std::vector<TraceLink> traceLinks(const RunTrace& t);

struct TraceStats {
    struct Span { uint64_t n = 0; asmcore::sim::Cycle sum = 0, max = 0; double avg() const { return n ? (double)sum / (double)n : 0; } void add(asmcore::sim::Cycle v) { n++; sum += v; if (v > max) max = v; } };
    uint64_t stalls[2] = {0, 0};                         // [0] Main, [1] Peri: how often it had to wait for a shared resource (CSR arbiter, Data Xfer) ...
    asmcore::sim::Cycle stallCycles[2] = {0, 0};          // ... and for how many cycles in all
    uint64_t published = 0, taken = 0, overruns = 0, lost = 0, faults = 0, busTransfers = 0, busNacks = 0, timersStarted = 0, timersDone = 0, timersCancelled = 0, eventsRaised = 0;
    Span resultLatency;                 // published -> taken
    Span eventWait;                     // raised -> dispatched
    Span eventHandler;                  // dispatched -> returned
    Span timerRun[2];                   // started -> done: [0] Main's timer, [1] Peri's
};
TraceStats traceStats(const RunTrace& t);

struct RowDelta {
    int domain = 0;
    size_t index = 0;                   // the n-th instruction of that domain
    uint32_t pc = 0;
    std::string what;
    asmcore::sim::Cycle baseAt = 0, curAt = 0;
    long long delta = 0;                // curAt - baseAt: positive = later
};
struct TraceDiff {
    bool same = true;                   // not a cycle differs, nothing diverges
    long long endDelta = 0;             // the end of the run
    size_t matched[2] = {0, 0};         // instructions that are the same instruction in both runs (same domain, same order, same pc) before the paths part
    size_t diverged[2] = {SIZE_MAX, SIZE_MAX};   // the index where the two runs execute different instructions (SIZE_MAX: they do not)
    size_t total[2][2] = {{0, 0}, {0, 0}};       // [domain][0 baseline, 1 current]: how many instructions each run executed
    long long maxShift[2] = {0, 0};     // the largest |delta| among the matched ones
    long long finalShift[2] = {0, 0};   // the delta of the last matched one
    size_t firstMoved[2] = {SIZE_MAX, SIZE_MAX}; // the first matched instruction whose time differs
    std::vector<RowDelta> moved;        // every matched instruction whose time differs (in time order of the current run)
    long long markShift = 0;            // the largest |shift| among the matched markers (bus, publish, events ...)
};
TraceDiff compareTraces(const RunTrace& base, const RunTrace& cur);

}  // namespace acide
