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
    char kind = 'n';                    // 'e' hardware event (raise / dispatch / eret), 'b' bus transaction ended, 'p' publish, 'c' slot consumed, 'o' overrun, 'f' fault
    std::string what;
    int64_t a = 0, b = 0;
};
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
