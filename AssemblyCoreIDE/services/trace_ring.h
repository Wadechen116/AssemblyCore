#pragma once
// The recent past of the debug session: the instructions of both sequencers and the things that happen between them (events, bus transfers, results published
// and taken ...), kept as a RunTrace for the time line. Fed from the session's updates; when it is full the oldest part goes. At full speed the session cannot
// deliver every event (it keeps the newest ones of each update): the missing stretch is recorded as a GAP, so the time line shows that something is missing
// instead of pretending. When the simulator is wound back to an earlier cycle, what is later than that goes (the run continues from there). No Qt.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sim_compare.h"
#include "sim_session.h"

namespace acide {

class TraceRing {
public:
    struct Gap { asmcore::sim::Cycle from = 0, to = 0; };

    explicit TraceRing(size_t maxRows = 60000) : maxRows_(maxRows < 100 ? 100 : maxRows) {}
    void setCapacity(size_t rows) { maxRows_ = rows < 100 ? 100 : rows; trim(); }
    size_t capacity() const { return maxRows_; }

    void clear();
    void feed(const SimUpdate& u);                          // the events of an update, and where the simulator is now
    void truncateAfter(asmcore::sim::Cycle t);              // the simulator was wound back to t

    const RunTrace& trace() const { return t_; }            // rows and markers in time order; endAt = where the simulator is
    const std::vector<Gap>& gaps() const { return gaps_; }
    asmcore::sim::Cycle firstCycle() const { return t_.rows.empty() ? 0 : t_.rows.front().at; }      // where the kept history starts
    uint64_t dropped() const { return dropped_; }           // instructions that fell off the old end
    // The row indexes shift when the old end is cut off: a view that keeps indexes starts again when this number changes.
    uint64_t trimEpoch() const { return epoch_; }

private:
    void trim();
    RunTrace t_;
    std::vector<Gap> gaps_;
    size_t maxRows_;
    uint64_t dropped_ = 0, epoch_ = 0;
};

}  // namespace acide
