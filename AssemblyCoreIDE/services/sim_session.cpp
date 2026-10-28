#include "sim_session.h"

#include <algorithm>
#include <chrono>

using namespace asmcore;
using namespace asmcore::sim;

namespace acide {

namespace {

class Collect : public EventSink {
public:
    void onEvent(const Event& e) override { events.push_back(e); }
    std::vector<Event> events;
};

const char* const kRegions[] = {"scratch", "csrbuf", "rx", "slot", "meta", "opa", "opb", "out", "status", "const"};
const size_t kRingCap = 200000;          // events kept for export
const size_t kUpdateCap = 5000;          // events sent with one update

}  // namespace

// Everything below is touched by the worker thread only.
struct SimSession::Worker {
    SimSession* owner = nullptr;
    std::unique_ptr<Simulator> sim;
    std::shared_ptr<Collect> sink;
    std::deque<Event> ring;
    std::vector<std::pair<std::string, uint32_t>> csrNames;
    std::vector<BreakpointSpec> desiredBps;
    std::map<size_t, int> bpIds;                                // index into desiredBps -> simulator id
    std::vector<std::string> desiredWatches;
    std::map<std::string, int> watchIds;
    std::vector<std::string> watchExprs;
    uint64_t seq = 0;
    bool failed = false;
    bool eventsReset = false;
    std::vector<std::string> messages;
    SimState state = SimState::Empty;

    ViewPtr makeView() {
        auto v = std::make_shared<SimView>();
        v->seq = ++seq;
        if (!sim) return v;
        v->cycle = sim->now();
        for (int i = 0; i < 2; i++) {
            Domain d = i == 0 ? Domain::Main : Domain::Peri;
            v->pc[i] = sim->pc(d);
            v->halted[i] = sim->halted(d);
            v->faulted[i] = sim->faulted(d);
            v->instrs[i] = sim->instructions(d);
            if (const Fault* f = sim->fault(d)) { v->hasFault[i] = true; v->fault[i] = *f; }
            v->stack[i] = sim->callStack(d);
        }
        for (const char* r : kRegions) {
            int n = sim->regionSize(r);
            if (n > 0) v->regions[r] = sim->readRegion(r, 0, n);
        }
        for (const std::string& n : sim->signalNames()) v->signalValues.push_back({n, sim->signal(n)});
        for (auto& c : csrNames) v->csrs.push_back({c.first, sim->csr(c.second)});
        v->breakpoints = sim->breakpoints();
        for (const std::string& e : watchExprs) {
            WatchValue w;
            w.expr = e;
            w.ok = sim->eval(e, w.value, w.error);
            v->watches.push_back(w);
        }
        return v;
    }

    // Sends the current state (and the events since the last time) to the owner thread.
    void publish(SimState st, const StopInfo* stop = nullptr) {
        state = st;
        auto up = std::make_shared<SimUpdate>();
        up->view = makeView();
        up->state = st;
        if (stop) up->stop = *stop;
        if (sink) {
            std::vector<Event>& ev = sink->events;
            for (const Event& e : ev) { ring.push_back(e); if (ring.size() > kRingCap) ring.pop_front(); }
            if (ev.size() > kUpdateCap) { up->droppedEvents = ev.size() - kUpdateCap; up->events.assign(ev.end() - (long)kUpdateCap, ev.end()); }
            else up->events = std::move(ev);
            ev.clear();
        }
        up->eventsReset = eventsReset;
        eventsReset = false;
        up->messages.swap(messages);
        SimSession* o = owner;
        std::weak_ptr<bool> alive = o->alive_;
        o->disp_->post([o, alive, up] {
            auto a = alive.lock();
            if (!a || !*a) return;
            o->state_ = up->state;
            o->view_ = up->view;
            o->updated.fire(*up);
        });
    }

    void reconcileBreakpoints() {
        if (!sim) return;
        // drop what is no longer wanted or changed, add what is missing
        for (auto it = bpIds.begin(); it != bpIds.end();) {
            if (it->first >= desiredBps.size()) { sim->removeBreakpoint(it->second); it = bpIds.erase(it); }
            else ++it;
        }
        std::vector<Breakpoint> have = sim->breakpoints();
        for (size_t i = 0; i < desiredBps.size(); i++) {
            auto it = bpIds.find(i);
            if (it != bpIds.end()) {
                bool same = false;
                for (const Breakpoint& b : have) if (b.id == it->second) same = b.domain == desiredBps[i].domain && b.pc == desiredBps[i].pc && b.condition == desiredBps[i].condition;
                if (same) continue;
                sim->removeBreakpoint(it->second);
                bpIds.erase(it);
            }
            bpIds[i] = sim->addBreakpoint(desiredBps[i].domain, desiredBps[i].pc, desiredBps[i].condition);
        }
    }

    void reconcileWatches() {
        if (!sim) return;
        for (auto it = watchIds.begin(); it != watchIds.end();) {
            if (std::find(desiredWatches.begin(), desiredWatches.end(), it->first) == desiredWatches.end()) { sim->removeWatch(it->second); it = watchIds.erase(it); }
            else ++it;
        }
        for (const std::string& t : desiredWatches) {
            if (watchIds.count(t)) continue;
            std::string err;
            int id = sim->addWatch(t, "", err);
            if (id < 0) messages.push_back("watch '" + t + "': " + err);
            else watchIds[t] = id;
        }
    }

    StopInfo toStop(const RunResult& r) const {
        StopInfo s;
        s.valid = true;
        s.reason = r.reason;
        s.id = r.id;
        s.detail = r.detail;
        s.now = r.now;
        return s;
    }

    void doLoad(SnapshotPtr program, const std::string& env, const Profile& profile, const SimParams& params) {
        sim.reset();
        sink.reset();
        ring.clear();
        bpIds.clear();
        watchIds.clear();
        csrNames.clear();
        failed = false;
        SimConfig cfg;
        cfg.profile = profile;
        cfg.env = env;
        cfg.params = params;
        if (!program || !program->result.ok) {
            failed = true;
            SimSession* o = owner;
            std::weak_ptr<bool> alive = o->alive_;
            o->disp_->post([o, alive] {
                auto a = alive.lock();
                if (!a || !*a) return;
                o->state_ = SimState::Failed;
                o->loaded.fire(false, std::vector<std::string>{"the program has errors: build it first"});
            });
            return;
        }
        for (const SymbolInfo& s : program->result.symbols) if (s.kind == "csr") csrNames.push_back({s.name, (uint32_t)s.value});
        sim.reset(new Simulator(program->result, cfg));
        if (!sim->ok()) {
            std::vector<std::string> errs = sim->configErrors();
            sim.reset();
            failed = true;
            SimSession* o = owner;
            std::weak_ptr<bool> alive = o->alive_;
            o->disp_->post([o, alive, errs] {
                auto a = alive.lock();
                if (!a || !*a) return;
                o->state_ = SimState::Failed;
                o->loaded.fire(false, errs);
            });
            return;
        }
        sink = std::make_shared<Collect>();
        sim->addSink(sink);
        sim->enableHistory(10000, 500);
        SimSession* o = owner;
        sim->setCancelHook([o] { return o->pause_.load(); });
        reconcileBreakpoints();
        reconcileWatches();
        eventsReset = true;
        SimSession* oo = owner;
        std::weak_ptr<bool> alive = oo->alive_;
        oo->disp_->post([oo, alive] {
            auto a = alive.lock();
            if (!a || !*a) return;
            oo->loaded.fire(true, std::vector<std::string>());
        });
        publish(SimState::Paused);
    }

    void doRun(const RunSpec& spec, bool toPc, Domain d, uint32_t pc) {
        if (!sim) return;
        int tempBp = -1;
        if (toPc) tempBp = sim->addBreakpoint(d, pc, "");
        owner->pause_ = false;
        owner->running_ = true;
        publish(SimState::Running);
        const Cycle start = sim->now();
        const auto wallStart = std::chrono::steady_clock::now();
        RunResult r;
        for (;;) {
            RunLimits l;
            if (spec.maxCycles) {
                Cycle used = sim->now() - start;
                if (used >= spec.maxCycles) { r.reason = StopReason::CycleLimit; r.now = sim->now(); r.detail = "cycle budget used up"; break; }
                l.maxCycles = spec.maxCycles - used;
            } else {
                l.maxCycles = 0;
            }
            l.maxMillis = 30;                                         // one slice
            l.stopOnIdle = spec.stopOnIdle;
            l.untilExpr = spec.until;
            r = sim->run(l);
            if (r.reason != StopReason::TimeLimit) break;              // anything but "the slice ended"
            if (spec.maxMillis && std::chrono::steady_clock::now() - wallStart > std::chrono::milliseconds(spec.maxMillis)) {
                r.detail = "wall-clock budget of " + std::to_string(spec.maxMillis) + " ms used up";
                break;
            }
            publish(SimState::Running);
        }
        if (tempBp >= 0) sim->removeBreakpoint(tempBp);
        owner->running_ = false;
        owner->pause_ = false;
        StopInfo s = toStop(r);
        publish(SimState::Paused, &s);
    }

    void doStep(int kind, Domain d) {
        if (!sim) return;
        owner->pause_ = false;
        owner->running_ = true;                                       // so that pause() also interrupts a stepOver into a long callee
        RunResult r = kind == 0 ? sim->step(d) : kind == 1 ? sim->stepOver(d) : sim->stepOut(d);
        owner->running_ = false;
        owner->pause_ = false;
        StopInfo s = toStop(r);
        publish(SimState::Paused, &s);
    }
};

SimSession::SimSession(std::shared_ptr<IDispatcher> d) : disp_(std::move(d)), w_(new Worker) {
    w_->owner = this;
    th_ = std::thread([this] { loop(); });
}

SimSession::~SimSession() {
    *alive_ = false;
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
    }
    pause_ = true;
    cv_.notify_all();
    th_.join();
}

void SimSession::post(std::function<void(Worker&)> fn) {
    {
        std::lock_guard<std::mutex> l(m_);
        q_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void SimSession::loop() {
    for (;;) {
        std::function<void(Worker&)> fn;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return stop_ || !q_.empty(); });
            if (stop_) return;
            fn = std::move(q_.front());
            q_.pop_front();
        }
        fn(*w_);
    }
}

// ---- owner-thread API ---------------------------------------------------------------------------------------------------------------
void SimSession::load(SnapshotPtr program, const std::string& env, const Profile& profile, const SimParams& params) {
    program_ = program;
    view_.reset();
    state_ = SimState::Paused;
    post([program, env, profile, params](Worker& w) { w.doLoad(program, env, profile, params); });
}

void SimSession::unload() {
    program_.reset();
    view_.reset();
    state_ = SimState::Empty;
    pause_ = true;
    post([](Worker& w) {
        w.sim.reset();
        w.sink.reset();
        w.ring.clear();
        w.bpIds.clear();
        w.watchIds.clear();
        w.publish(SimState::Empty);
    });
}

void SimSession::run(const RunSpec& spec) { post([spec](Worker& w) { w.doRun(spec, false, Domain::Main, 0); }); state_ = SimState::Running; }
void SimSession::runToPc(Domain d, uint32_t pc, const RunSpec& spec) { post([spec, d, pc](Worker& w) { w.doRun(spec, true, d, pc); }); state_ = SimState::Running; }
void SimSession::pause() { if (running_) pause_ = true; }
void SimSession::step(Domain d) { post([d](Worker& w) { w.doStep(0, d); }); }
void SimSession::stepOver(Domain d) { post([d](Worker& w) { w.doStep(1, d); }); }
void SimSession::stepOut(Domain d) { post([d](Worker& w) { w.doStep(2, d); }); }

void SimSession::reset() {
    post([](Worker& w) {
        if (!w.sim) return;
        w.sim->reset();
        w.sink->events.clear();
        w.ring.clear();
        w.eventsReset = true;
        w.publish(SimState::Paused);
    });
}

void SimSession::rewindTo(Cycle t) {
    post([t](Worker& w) {
        if (!w.sim) return;
        bool ok = w.sim->rewindTo(t);
        w.sink->events.clear();                                       // the replay re-emits what came before t: start the list afresh
        w.ring.clear();
        w.eventsReset = true;
        if (!ok) w.messages.push_back("cannot rewind to cycle " + std::to_string(t) + " (no earlier snapshot)");
        w.publish(SimState::Paused);
    });
}

void SimSession::setBreakpoints(const std::vector<BreakpointSpec>& v) {
    post([v](Worker& w) {
        w.desiredBps = v;
        w.reconcileBreakpoints();
        if (w.sim) w.publish(w.state == SimState::Running ? SimState::Running : SimState::Paused);
    });
}

void SimSession::setDataWatches(const std::vector<std::string>& targets) {
    post([targets](Worker& w) {
        w.desiredWatches = targets;
        w.reconcileWatches();
        if (w.sim) w.publish(SimState::Paused);
    });
}

void SimSession::setWatchExprs(const std::vector<std::string>& exprs) {
    post([exprs](Worker& w) {
        w.watchExprs = exprs;
        if (w.sim) w.publish(SimState::Paused);
    });
}

void SimSession::writeRegion(const std::string& region, int off, const std::vector<uint8_t>& bytes) {
    post([region, off, bytes](Worker& w) {
        if (!w.sim) return;
        if (!w.sim->writeRegion(region, off, bytes)) w.messages.push_back("cannot write " + region + "[" + std::to_string(off) + "]");
        w.publish(SimState::Paused);
    });
}

void SimSession::setSignal(const std::string& name, int64_t value) {
    post([name, value](Worker& w) {
        if (!w.sim) return;
        if (!w.sim->setSignal(name, value)) w.messages.push_back("no signal named " + name);
        w.publish(SimState::Paused);
    });
}

void SimSession::command(const std::string& cmd) {
    post([cmd](Worker& w) {
        if (!w.sim) return;
        std::string err;
        if (!w.sim->injectNow(cmd, err)) w.messages.push_back(cmd + ": " + err);
        w.publish(SimState::Paused);
    });
}

void SimSession::exportTrace(const std::string& format, std::function<void(const std::string&)> done) {
    SimSession* o = this;
    post([format, done, o](Worker& w) {
        std::string out;
        std::shared_ptr<EventSink> sink;
        std::shared_ptr<VcdSink> vcd;
        if (format == "csv") sink = makeCsvTrace(&out);
        else if (format == "vcd") { vcd = std::make_shared<VcdSink>(&out); sink = vcd; }
        else sink = makeTextTrace(&out);
        Cycle last = 0;
        for (const Event& e : w.ring) { sink->onEvent(e); last = e.t; }
        if (vcd) vcd->finish(last);
        std::weak_ptr<bool> alive = o->alive_;
        o->disp_->post([done, out, alive] {
            auto a = alive.lock();
            if (a && *a) done(out);
        });
    });
}

}  // namespace acide
