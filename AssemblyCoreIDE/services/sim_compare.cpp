#include "sim_compare.h"

#include <algorithm>
#include <cstdlib>

using namespace asmcore;
using namespace asmcore::sim;

namespace acide {

namespace {

class Recorder : public EventSink {
public:
    explicit Recorder(RunTrace& t, size_t maxRows) : t_(t), max_(maxRows) {}
    void onEvent(const Event& e) override {
        TraceMarker m;
        m.at = e.t;
        m.domain = e.domain;
        m.a = e.a;
        m.b = e.b;
        m.what = e.what;
        switch (e.type) {
            case EventType::Issue:
                if (e.domain < 0 || e.domain > 1) return;
                if (t_.rows.size() >= max_) { t_.truncated = true; return; }
                {
                    TraceRow r;
                    r.domain = e.domain;
                    r.pc = e.pc;
                    r.at = e.t;
                    r.what = e.what;
                    r.len = (int)e.a;
                    t_.rows.push_back(r);
                }
                return;
            case EventType::HwEvent: m.kind = 'e'; break;
            case EventType::Bus: m.kind = 'b'; break;
            case EventType::Publish: m.kind = 'p'; break;
            case EventType::Consume: m.kind = 'c'; break;
            case EventType::Overrun: m.kind = 'o'; break;
            case EventType::Fault: m.kind = 'f'; m.what = e.what; break;
            default: return;
        }
        if (t_.marks.size() < max_) t_.marks.push_back(m);
    }
private:
    RunTrace& t_;
    size_t max_;
};

}  // namespace

Cycle RunTrace::endOf(size_t i) const {
    for (size_t k = i + 1; k < rows.size(); k++) if (rows[k].domain == rows[i].domain) return rows[k].at;
    return std::max(endAt, rows[i].at);
}

RunTrace recordRun(const Result& program, const SimConfig& cfg, Cycle maxCycles, size_t maxRows) {
    RunTrace t;
    Simulator sim(program, cfg);
    if (!sim.ok()) {
        t.error = sim.configErrors().empty() ? std::string("the simulator could not be made") : sim.configErrors()[0];
        return t;
    }
    sim.addSink(std::make_shared<Recorder>(t, maxRows));
    RunLimits l;
    l.maxCycles = maxCycles;
    l.maxMillis = 4000;
    const RunResult rr = sim.run(l);
    t.endAt = rr.now;
    t.reason = rr.reason;
    for (int d = 0; d < 2; d++) {
        int64_t v = 0;
        std::string err;
        if (sim.eval(d == 0 ? "main.instrs" : "peri.instrs", v, err)) t.instrs[d] = v;
    }
    return t;
}

TraceDiff compareTraces(const RunTrace& base, const RunTrace& cur) {
    TraceDiff d;
    d.endDelta = (long long)cur.endAt - (long long)base.endAt;
    std::vector<size_t> b[2], c[2];
    for (size_t i = 0; i < base.rows.size(); i++) b[base.rows[i].domain].push_back(i);
    for (size_t i = 0; i < cur.rows.size(); i++) c[cur.rows[i].domain].push_back(i);
    for (int dom = 0; dom < 2; dom++) {
        d.total[dom][0] = b[dom].size();
        d.total[dom][1] = c[dom].size();
        const size_t n = std::min(b[dom].size(), c[dom].size());
        size_t k = 0;
        for (; k < n; k++) {
            const TraceRow& x = base.rows[b[dom][k]];
            const TraceRow& y = cur.rows[c[dom][k]];
            if (x.pc != y.pc || x.what != y.what) { d.diverged[dom] = k; break; }       // not the same instruction: the two runs went different ways
            const long long delta = (long long)y.at - (long long)x.at;
            d.finalShift[dom] = delta;
            d.maxShift[dom] = std::max<long long>(d.maxShift[dom], std::llabs(delta));
            if (delta != 0) {
                if (d.firstMoved[dom] == SIZE_MAX) d.firstMoved[dom] = k;
                RowDelta r;
                r.domain = dom; r.index = k; r.pc = y.pc; r.what = y.what; r.baseAt = x.at; r.curAt = y.at; r.delta = delta;
                d.moved.push_back(r);
            }
        }
        d.matched[dom] = k;
        if (d.diverged[dom] == SIZE_MAX && b[dom].size() != c[dom].size()) d.diverged[dom] = n;       // one run went on, the other did not
    }
    std::sort(d.moved.begin(), d.moved.end(), [](const RowDelta& x, const RowDelta& y) { return x.curAt != y.curAt ? x.curAt < y.curAt : x.domain < y.domain; });
    const size_t nm = std::min(base.marks.size(), cur.marks.size());
    for (size_t i = 0; i < nm; i++) {
        if (base.marks[i].kind != cur.marks[i].kind || base.marks[i].domain != cur.marks[i].domain || base.marks[i].what != cur.marks[i].what) break;
        d.markShift = std::max<long long>(d.markShift, std::llabs((long long)cur.marks[i].at - (long long)base.marks[i].at));
    }
    d.same = d.endDelta == 0 && d.moved.empty() && d.diverged[0] == SIZE_MAX && d.diverged[1] == SIZE_MAX && d.markShift == 0;
    return d;
}

}  // namespace acide
