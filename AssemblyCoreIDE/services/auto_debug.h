#pragma once
// Auto debug: the program is run in a simulator of its own, at full speed, for a stretch of cycles, and what happened is searched for the things that are
// wrong or suspicious - without a breakpoint and without watching it run. What it looks for:
//   * a sequencer that FAULTED
//   * the result slot OVERRUN (Peri published before Main took the last result)
//   * a sequencer that is STUCK: still going round a small loop with a conditional branch at the end of the run, with nothing happening since (no publish, no
//     result taken, no bus transfer, no event) - what a wait for something that never comes looks like
//   * an I2C device that did not ACKNOWLEDGE
//   * hardware events that were raised and never dispatched (no event_safe BR is ever reached)
//   * results published and never taken by Main
//   * a WCET register longer than the blanking window (PGL_COMMIT_ELIGIBLE can never be true)
//   * instructions that never ran (a path the run never took)
// Each finding says when, where (the source line of the instruction), what it means and what to look at. No Qt.
#include <cstdint>
#include <string>
#include <vector>

#include "asmcore/sim.h"

namespace acide {

struct AutoFinding {
    enum Severity { Error, Warning, Info } sev = Info;
    std::string code;                    // AD-FAULT AD-OVERRUN AD-STUCK AD-NACK AD-EVENT AD-UNTAKEN AD-WCET AD-UNREACHED
    std::string title, detail, hint;
    asmcore::sim::Cycle at = 0;          // the first cycle it shows
    int domain = -1;                     // 0 Main, 1 Peri, -1: neither
    bool hasPc = false;
    uint32_t pc = 0;
    std::string file;                    // where the instruction came from (empty if not known)
    int line = 0;
    int count = 1;                       // how many times it happened
};

struct AutoDebugOptions {
    asmcore::sim::Cycle maxCycles = 200000;    // how long the program is run
    uint32_t maxMillis = 3000;                 // ... and the wall-clock limit
    asmcore::sim::Cycle stuckCycles = 0;       // how long a loop without progress counts as stuck; 0 = a tenth of maxCycles, at least 5000
};

struct AutoDebugReport {
    std::string error;                   // the simulator could not be made (the environment file has errors ...)
    std::vector<AutoFinding> findings;   // errors first, then warnings, then notes; inside each in time order
    asmcore::sim::Cycle endAt = 0;
    asmcore::sim::StopReason reason = asmcore::sim::StopReason::None;
    std::string stopDetail;
    int64_t instrs[2] = {0, 0};
    int publishes = 0, consumes = 0;
    bool ok() const { return error.empty(); }
    int count(AutoFinding::Severity s) const { int n = 0; for (const AutoFinding& f : findings) if (f.sev == s) n++; return n; }
};

AutoDebugReport autoDebug(const asmcore::Result& program, const asmcore::sim::SimConfig& cfg, const AutoDebugOptions& opt = AutoDebugOptions());

}  // namespace acide
