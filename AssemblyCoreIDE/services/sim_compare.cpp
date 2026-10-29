#include "sim_compare.h"

#include <algorithm>
#include <cstdlib>
#include <map>

using namespace asmcore;
using namespace asmcore::sim;

namespace acide {

namespace {

class Recorder : public EventSink {
public:
    explicit Recorder(RunTrace& t, size_t maxRows) : t_(t), max_(maxRows) {}
    void onEvent(const Event& e) override {
        if (e.type == EventType::Issue) {
            if (e.domain < 0 || e.domain > 1) return;
            if (t_.rows.size() >= max_) { t_.truncated = true; return; }
            TraceRow r;
            r.domain = e.domain;
            r.pc = e.pc;
            r.at = e.t;
            r.what = e.what;
            r.len = (int)e.a;
            t_.rows.push_back(r);
            return;
        }
        TraceMarker m;
        if (markerOf(e, m) && t_.marks.size() < max_) t_.marks.push_back(m);
    }
private:
    RunTrace& t_;
    size_t max_;
};

}  // namespace

bool markerOf(const Event& e, TraceMarker& m) {
    m = TraceMarker();
    m.at = e.t;
    m.domain = e.domain;
    m.a = e.a;
    m.b = e.b;
    m.c = e.c;
    m.what = e.what;
    switch (e.type) {
        case EventType::HwEvent: m.kind = 'e'; break;
        case EventType::Bus: m.kind = 'b'; break;
        case EventType::Publish: m.kind = 'p'; break;
        case EventType::Consume: m.kind = 'c'; break;
        case EventType::Overrun: m.kind = 'o'; break;
        case EventType::Fault: m.kind = 'f'; break;
        case EventType::Stall: m.kind = 'z'; m.hasPc = e.domain == 0 || e.domain == 1; m.pc = e.pc; return true;       // a decoder waits for a resource both share
        case EventType::Signal:
            if (e.what == "timer.gen" || e.what == "main_timer.gen") {            // a new generation of a timer: it was (re)started (or cleared)
                const bool main = e.what[0] == 'm';
                m.kind = 's'; m.domain = main ? 0 : 1; m.what = main ? "main_timer" : "timer"; m.a = e.b;
            } else if ((e.what == "timer.done" || e.what == "main_timer.done") && e.b == 1) {
                const bool main = e.what[0] == 'm';
                m.kind = 'd'; m.domain = main ? 0 : 1; m.what = main ? "main_timer" : "timer";
            } else if (e.what == "vblank_open") {
                m.kind = e.b ? 'v' : 'w'; m.domain = -1;
            } else {
                return false;
            }
            return true;
        default: return false;
    }
    if (e.type != EventType::Consume && e.type != EventType::HwEvent && e.pc != 0) { m.hasPc = true; m.pc = e.pc; }
    if (e.type == EventType::HwEvent && e.what == "dispatch") { m.hasPc = true; m.pc = e.pc; }
    if (e.type == EventType::Publish || e.type == EventType::Fault || e.type == EventType::Bus || e.type == EventType::Overrun) { m.hasPc = e.domain == 0 || e.domain == 1; m.pc = e.pc; }
    return true;
}

std::vector<TraceLink> traceLinks(const RunTrace& t) {
    std::vector<TraceLink> v;
    int pendingPublish = -1;                                                  // the slot holds one result
    std::map<std::pair<int, int64_t>, int> raised, dispatched;                // (domain, id) -> the marker
    int timerStart[2] = {-1, -1};
    int blankOpen = -1;
    auto link = [&](TraceLink::Kind k, int domain, int from, int to, int64_t a, const std::string& what) {
        TraceLink l;
        l.kind = k; l.domain = domain; l.fromMark = from; l.toMark = to; l.a = a; l.what = what;
        l.from = t.marks[(size_t)from].at;
        l.open = to < 0;
        l.to = to < 0 ? t.endAt : t.marks[(size_t)to].at;
        v.push_back(l);
        return (int)v.size() - 1;
    };
    for (size_t i = 0; i < t.marks.size(); i++) {
        const TraceMarker& m = t.marks[i];
        const int mi = (int)i;
        switch (m.kind) {
            case 'p': pendingPublish = mi; break;
            case 'c':
                if (pendingPublish >= 0) { link(TraceLink::Result, 1, pendingPublish, mi, t.marks[(size_t)pendingPublish].a, "result"); pendingPublish = -1; }
                break;
            case 'o': { const int k = link(TraceLink::Result, 1, mi, mi, m.a, "overrun"); v[(size_t)k].lost = true; v[(size_t)k].open = false; break; }
            case 'e': {
                const std::pair<int, int64_t> key{m.domain, m.a};
                if (m.what == "raise") raised[key] = mi;
                else if (m.what == "dispatch") {
                    const auto it = raised.find(key);
                    if (it != raised.end()) { link(TraceLink::EventWait, m.domain, it->second, mi, m.a, "event"); raised.erase(it); }
                    dispatched[key] = mi;
                } else if (m.what == "eret") {
                    const auto it = dispatched.find(key);
                    if (it != dispatched.end()) { link(TraceLink::EventHandler, m.domain, it->second, mi, m.a, "handler"); dispatched.erase(it); }
                }
                break;
            }
            case 's': {
                const int d = m.domain == 0 ? 0 : 1;
                if (timerStart[d] >= 0 && t.marks[(size_t)timerStart[d]].at != m.at) { const int k = link(TraceLink::Timer, d, timerStart[d], mi, 0, m.what); v[(size_t)k].lost = true; v[(size_t)k].open = false; }
                timerStart[d] = mi;                                           // (a start in the same cycle as the one before replaces it: a clear and a start)
                break;
            }
            case 'd': {
                const int d = m.domain == 0 ? 0 : 1;
                if (timerStart[d] >= 0) { link(TraceLink::Timer, d, timerStart[d], mi, 0, m.what); timerStart[d] = -1; }
                break;
            }
            case 'v': blankOpen = mi; break;
            case 'w': if (blankOpen >= 0) { link(TraceLink::Blanking, -1, blankOpen, mi, 0, "blanking"); blankOpen = -1; } break;
            default: break;
        }
    }
    if (pendingPublish >= 0) link(TraceLink::Result, 1, pendingPublish, -1, t.marks[(size_t)pendingPublish].a, "result");
    for (const auto& kv : raised) link(TraceLink::EventWait, kv.first.first, kv.second, -1, kv.first.second, "event");
    for (const auto& kv : dispatched) link(TraceLink::EventHandler, kv.first.first, kv.second, -1, kv.first.second, "handler");
    for (int d = 0; d < 2; d++) if (timerStart[d] >= 0) link(TraceLink::Timer, d, timerStart[d], -1, 0, d == 0 ? "main_timer" : "timer");
    if (blankOpen >= 0) link(TraceLink::Blanking, -1, blankOpen, -1, 0, "blanking");
    return v;
}

TraceStats traceStats(const RunTrace& t) {
    TraceStats s;
    for (const TraceMarker& m : t.marks) {
        switch (m.kind) {
            case 'p': s.published++; break;
            case 'c': s.taken++; break;
            case 'o': s.overruns++; break;
            case 'f': s.faults++; break;
            case 'z': if (m.domain == 0 || m.domain == 1) { s.stalls[m.domain]++; s.stallCycles[m.domain] += (asmcore::sim::Cycle)m.a; } break;
            case 'b': s.busTransfers++; if (m.b == 0) s.busNacks++; break;
            case 's': s.timersStarted++; break;
            case 'd': s.timersDone++; break;
            case 'e': if (m.what == "raise") s.eventsRaised++; break;
            default: break;
        }
    }
    for (const TraceLink& l : traceLinks(t)) {
        if (l.open) continue;
        switch (l.kind) {
            case TraceLink::Result: if (!l.lost) s.resultLatency.add(l.length()); else s.lost++; break;
            case TraceLink::EventWait: s.eventWait.add(l.length()); break;
            case TraceLink::EventHandler: s.eventHandler.add(l.length()); break;
            case TraceLink::Timer: if (l.lost) s.timersCancelled++; else s.timerRun[l.domain == 0 ? 0 : 1].add(l.length()); break;
            default: break;
        }
    }
    return s;
}

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
