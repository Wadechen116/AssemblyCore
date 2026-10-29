#include "trace_ring.h"

#include <algorithm>

using namespace asmcore::sim;

namespace acide {

void TraceRing::clear() {
    t_ = RunTrace();
    gaps_.clear();
    dropped_ = 0;
    epoch_++;
}

void TraceRing::feed(const SimUpdate& u) {
    const Cycle before = t_.endAt;
    if (u.droppedEvents > 0 && !u.events.empty() && u.events.front().t > before) gaps_.push_back(Gap{before, u.events.front().t});
    for (const Event& e : u.events) {
        if (e.type == EventType::Issue) {
            if (e.domain < 0 || e.domain > 1) continue;
            TraceRow r;
            r.domain = e.domain;
            r.pc = e.pc;
            r.at = e.t;
            r.what = e.what;
            r.len = (int)e.a;
            t_.rows.push_back(r);
            t_.instrs[e.domain]++;
            continue;
        }
        TraceMarker m;
        if (markerOf(e, m)) t_.marks.push_back(m);
    }
    if (u.view) t_.endAt = std::max(t_.endAt, u.view->cycle);
    if (!t_.rows.empty()) t_.endAt = std::max(t_.endAt, t_.rows.back().at);
    trim();
}

// Over the capacity by a quarter: the oldest rows go (cutting in chunks keeps the cost low), with the markers and gaps older than what is left.
void TraceRing::trim() {
    if (t_.rows.size() <= maxRows_ + maxRows_ / 4) return;
    const size_t cut = t_.rows.size() - maxRows_;
    const Cycle from = t_.rows[cut].at;
    t_.rows.erase(t_.rows.begin(), t_.rows.begin() + (long)cut);
    dropped_ += cut;
    t_.truncated = true;
    t_.marks.erase(t_.marks.begin(), std::lower_bound(t_.marks.begin(), t_.marks.end(), from, [](const TraceMarker& m, Cycle c) { return m.at < c; }));
    gaps_.erase(std::remove_if(gaps_.begin(), gaps_.end(), [from](const Gap& g) { return g.to < from; }), gaps_.end());
    epoch_++;
}

void TraceRing::truncateAfter(Cycle t) {
    const auto rowEnd = std::upper_bound(t_.rows.begin(), t_.rows.end(), t, [](Cycle c, const TraceRow& r) { return c < r.at; });
    for (auto it = rowEnd; it != t_.rows.end(); ++it) t_.instrs[it->domain]--;
    t_.rows.erase(rowEnd, t_.rows.end());
    t_.marks.erase(std::upper_bound(t_.marks.begin(), t_.marks.end(), t, [](Cycle c, const TraceMarker& m) { return c < m.at; }), t_.marks.end());
    for (Gap& g : gaps_) g.to = std::min(g.to, t);
    gaps_.erase(std::remove_if(gaps_.begin(), gaps_.end(), [](const Gap& g) { return g.to <= g.from; }), gaps_.end());
    t_.endAt = t;
    epoch_++;
}

}  // namespace acide
