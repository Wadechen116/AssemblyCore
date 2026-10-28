// Scheduler, timed events, injection, run control, and the Simulator facade methods that are not about debugging.
#include "../codec.h"
#include <chrono>
#include <algorithm>
#include <sstream>

#include "impl.h"

namespace asmcore {
namespace sim {

// ---- timed events --------------------------------------------------------------------------------------------------------------
void Simulator::Impl::schedule(Cycle t, TimedEvent::Kind k, int a, int64_t b, const std::string& s) {
    TimedEvent e;
    e.t = std::max(t, st.now);
    e.seq = st.seq++;
    e.kind = k;
    e.a = a;
    e.b = b;
    e.s = s;
    auto pos = std::upper_bound(st.q.begin(), st.q.end(), e, [](const TimedEvent& x, const TimedEvent& y) { return x.t != y.t ? x.t < y.t : x.seq < y.seq; });
    st.q.insert(pos, e);
}

void Simulator::Impl::handleTimed(const TimedEvent& e) {
    switch (e.kind) {
        case TimedEvent::SetSignal: {
            if (!e.s.empty()) {                              // guarded: "<signal>=<value>" must still hold
                size_t eq = e.s.find('=');
                if (eq != std::string::npos && signal(e.s.substr(0, eq)) != std::stoll(e.s.substr(eq + 1))) break;
            }
            sigSet(e.a, e.b);
            break;
        }
        case TimedEvent::SlotBusyEnd: st.slot.busy = false; break;
        case TimedEvent::SlotConsumeDone: {
            int svc = st.slot.svc;
            st.slot.valid = false;
            st.slot.busy = false;
            st.slot.clearedAt = st.now;
            emit([&](Event& ev) { ev.type = EventType::Consume; ev.domain = 0; ev.a = svc; });
            break;
        }
        case TimedEvent::XferDone: xferComplete(); break;
        case TimedEvent::FrameEnd:
            sigSet(sigId("frame_end_evt"), 1);
            schedule(st.now + (Cycle)timing.frameCycles, TimedEvent::FrameEnd);
            break;
        case TimedEvent::WindowEdge: {   // informational: the windows themselves are computed from time
            bool v = e.a == 0;
            bool opening = e.b == 1;
            int64_t len = v ? timing.vLen : timing.hLen;
            int64_t period = v ? timing.frameCycles : timing.lineCycles;
            const char* name = v ? "vblank_open" : "hblank_open";
            emit([&](Event& ev) { ev.type = EventType::Signal; ev.what = name; ev.a = opening ? 0 : 1; ev.b = opening ? 1 : 0; });
            if (opening) {
                schedule(st.now + (Cycle)len, TimedEvent::WindowEdge, e.a, 0);
                schedule(st.now + (Cycle)period, TimedEvent::WindowEdge, e.a, 1);
            }
            break;
        }
        case TimedEvent::Command: applyCommand(e.s); break;
    }
}

void Simulator::Impl::applyCommand(const std::string& cmd) {
    emit([&](Event& e) { e.type = EventType::Inject; e.text = cmd; });
    std::istringstream in(cmd);
    std::string verb;
    in >> verb;
    verb = lower(verb);
    std::map<std::string, std::string> kv;
    std::vector<std::string> words;
    std::string w;
    while (in >> w) {
        size_t eq = w.find('=');
        if (eq != std::string::npos) kv[lower(w.substr(0, eq))] = w.substr(eq + 1);
        else words.push_back(w);
    }
    auto num = [&](const std::string& s, int64_t dflt) { int64_t v = dflt; std::string e; if (!s.empty()) evalExpr(s, nullptr, v, e); return v; };
    if (verb == "nack") {
        int64_t addr = -1;
        if (kv.count("addr")) addr = num(kv["addr"], -1);
        else if (kv.count("dev")) { uint32_t dm = 0; if (img[1].entry(PT_DEVMAP, (uint32_t)num(kv["dev"], 0), pf.deviceMapEntryBytes, dm)) addr = codec::unpackDev(pf, dm).addr7; }
        if (addr < 0) { emitNote("inject nack: dev= or addr= is required"); return; }
        st.nack[(int)addr] += (int)num(kv.count("count") ? kv["count"] : "1", 1);
    } else if (verb == "signal") {
        for (auto& p : kv) setSignal(p.first, num(p.second, 0));
        for (auto& wd : words) { size_t eq = wd.find('='); (void)eq; }
    } else if (verb == "slot_busy") {
        st.slot.busy = true;
        schedule(st.now + (Cycle)num(kv["cycles"], 1), TimedEvent::SlotBusyEnd);
    } else if (verb == "fault_clear") {
        bool main = words.empty() || lower(words[0]) == "main";
        int i = main ? 0 : 1;
        st.hasFault[i] = false;
        (main ? st.m.faulted : st.p.faulted) = false;
        (main ? st.m.readyAt : st.p.readyAt) = st.now;
    } else if (verb == "csr") {
        for (auto& p : kv) { int64_t a = num(p.first, 0); csrWrite((uint32_t)a, (uint8_t)num(p.second, 0), 0, Domain::Main); }
    } else if (verb == "event") {
        // event main|peri id=N     (also: event main N)
        const std::string w0 = words.empty() ? "" : lower(words[0]);
        if (w0 != "main" && w0 != "peri") { emitNote("inject event: say which domain, e.g. 'event main id=0'"); return; }
        int64_t id = kv.count("id") ? num(kv["id"], -1) : (words.size() > 1 ? num(words[1], -1) : -1);
        if (id < 0 || id >= 64) { emitNote("inject event: id must be 0..63"); return; }
        raiseEvent(w0 == "main" ? 0 : 1, (int)id);
    } else if (verb == "note") {
        emitNote(cmd.substr(cmd.find(' ') == std::string::npos ? cmd.size() : cmd.find(' ') + 1));
    } else {
        emitNote("unknown inject command '" + verb + "'");
    }
}

// ---- scheduling ----------------------------------------------------------------------------------------------------------------
Cycle Simulator::Impl::nextTime(int& who) const {
    Cycle tq = st.q.empty() ? kNever : st.q.front().t;
    Cycle tm = (st.m.halted || st.m.faulted) ? kNever : st.m.readyAt;
    Cycle tp = (st.p.halted || st.p.faulted) ? kNever : st.p.readyAt;
    Cycle best = tq;
    who = tq == kNever ? -1 : 2;
    auto consider = [&](Cycle t, int w) { if (t < best || (t == best && who == -1 && t != kNever)) { best = t; who = w; } };
    if (tieMainFirst) { consider(tm, 0); consider(tp, 1); }
    else { consider(tp, 1); consider(tm, 0); }
    if (best == kNever) who = -1;
    return best;
}

static bool bothStopped(const State& s) { return (s.m.halted || s.m.faulted) && (s.p.halted || s.p.faulted); }

bool Simulator::Impl::evalBreakpoint(Domain d, uint32_t pc, int& id) {
    for (Breakpoint& b : bps) {
        if (!b.enabled || b.domain != d || b.pc != pc) continue;
        if (!b.condition.empty()) {
            Expr ex;
            std::string e;
            if (!compile(b.condition, ex, e) || !ex.eval(*this)) continue;
        }
        b.hits++;
        id = b.id;
        return true;
    }
    return false;
}

RunResult Simulator::Impl::runLoop(const RunLimits& lim, int stepDomain, bool, uint32_t, size_t) {
    RunResult rr;
    const Cycle start = st.now;
    uint64_t executed = 0;
    Expr until;
    if (!lim.untilExpr.empty()) {
        std::string e;
        if (!compile(lim.untilExpr, until, e)) { rr.reason = StopReason::Condition; rr.detail = "bad condition: " + e; rr.now = st.now; return rr; }
    }
    primeWatches();
    stopRequested = false;
    const auto wallStart = std::chrono::steady_clock::now();
    uint32_t polls = 0;
    for (;;) {
        if ((++polls & 0xFF) == 0) {                       // cheap enough to do often, rare enough not to show in a profile
            if ((lim.cancel && lim.cancel()) || (cancelHook && cancelHook())) { rr.reason = StopReason::Cancelled; rr.detail = "cancelled"; break; }
            if (lim.maxMillis && std::chrono::steady_clock::now() - wallStart > std::chrono::milliseconds(lim.maxMillis)) {
                rr.reason = StopReason::TimeLimit;
                rr.detail = "wall-clock limit of " + std::to_string(lim.maxMillis) + " ms";
                break;
            }
        }
        if (polls == 1 && ((lim.cancel && lim.cancel()) || (cancelHook && cancelHook()))) { rr.reason = StopReason::Cancelled; rr.detail = "cancelled"; break; }
        if (bothStopped(st)) {
            if (lim.stopOnIdle || st.q.empty()) { rr.reason = StopReason::Idle; rr.detail = "both domains halted or faulted"; break; }
        }
        int who;
        Cycle t = nextTime(who);
        if (who < 0) { rr.reason = StopReason::Idle; rr.detail = "nothing left to run"; break; }
        if (lim.maxCycles && t - start > lim.maxCycles) { st.now = start + lim.maxCycles; rr.reason = StopReason::CycleLimit; break; }
        st.now = t;
        if (who == 2) {
            TimedEvent e = st.q.front();
            st.q.erase(st.q.begin());
            handleTimed(e);
        } else {
            Domain d = who == 0 ? Domain::Main : Domain::Peri;
            uint32_t pcBefore = who == 0 ? st.m.pc : st.p.pc;
            if (!suppressStops) {
                int id = -1;
                bool skip = skipValid && skipDomain == who && skipPc == pcBefore;
                if (skipValid && skipDomain == who) skipValid = false;
                if (!skip && evalBreakpoint(d, pcBefore, id)) {
                    rr.reason = StopReason::Breakpoint;
                    rr.id = id;
                    rr.detail = std::string(domainName(d)) + " breakpoint at pc " + std::to_string(pcBefore);
                    skipValid = true; skipDomain = who; skipPc = pcBefore;
                    break;
                }
            }
            lastWrites.clear();
            if (who == 0) stepMain(); else stepPeri();
            afterInstruction(d, pcBefore);
            executed++;
        }
        takeSnapshotIfDue();
        if (stopRequested) { stopRequested = false; rr.reason = stopWhy; rr.detail = stopDetail; break; }
        if (suppressStops) continue;
        int wid = -1;
        std::string wd;
        if (evalWatches(wid, wd)) { rr.reason = StopReason::Watchpoint; rr.id = wid; rr.detail = wd; break; }
        if (until.valid() && until.eval(*this)) { rr.reason = StopReason::Condition; rr.detail = "until " + lim.untilExpr; break; }
        if (stepDomain >= 0 && who == stepDomain) { rr.reason = StopReason::InstrLimit; rr.detail = "stepped"; break; }
        if (lim.maxInstrs && executed >= lim.maxInstrs) { rr.reason = StopReason::InstrLimit; rr.detail = "instruction limit"; break; }
    }
    rr.now = st.now;
    return rr;
}

void Simulator::Impl::afterInstruction(Domain d, uint32_t pcBefore) { trackCalls(d, pcBefore); }

// ---- facade ---------------------------------------------------------------------------------------------------------------------
Simulator::Simulator(const Result& assembled, const SimConfig& cfg, const Extensions* ext) : p_(new Impl(assembled, cfg, ext)) {}
Simulator::~Simulator() {}
bool Simulator::ok() const { return p_->errors.empty(); }
const std::vector<std::string>& Simulator::configErrors() const { return p_->errors; }
const SimParams& Simulator::params() const { return p_->params; }
void Simulator::reset() { p_->resetState(); p_->skipValid = false; }
Cycle Simulator::now() const { return p_->st.now; }

RunResult Simulator::run(const RunLimits& l) { return p_->runLoop(l, -1, false, 0, 0); }

RunResult Simulator::step(Domain d) {
    Impl& I = *p_;
    bool stopped = d == Domain::Main ? (I.st.m.halted || I.st.m.faulted) : (I.st.p.halted || I.st.p.faulted);
    if (stopped) { RunResult r; r.reason = StopReason::Idle; r.now = I.st.now; r.detail = std::string(domainName(d)) + " is halted or faulted"; return r; }
    RunLimits l;
    l.maxCycles = 0;
    l.stopOnIdle = false;
    I.skipValid = true;
    I.skipDomain = di(d);
    I.skipPc = d == Domain::Main ? I.st.m.pc : I.st.p.pc;
    return I.runLoop(l, di(d), false, 0, 0);
}

namespace {
const InstrInfo* infoAtPc(Simulator::Impl& I, Domain d) {
    uint32_t pc = d == Domain::Main ? I.st.m.pc : I.st.p.pc;
    auto it = I.instrAt[di(d)].find(pc);
    return it == I.instrAt[di(d)].end() ? nullptr : it->second;
}
bool sameExpansion(const InstrInfo* a, const InstrInfo* b) {
    if (!a || !b || a->frames.empty() || b->frames.empty()) return false;
    const Frame &x = a->frames.back(), &y = b->frames.back();
    return x.proc == y.proc && x.callSite.line == y.callSite.line && x.callSite.file == y.callSite.file;
}
// Nesting depth of the instruction about to run in domain d: runtime frames (softret) plus the frames of an inline expansion.
// `prev` is the instruction executed just before (nullptr at the start). The first instruction of an inline expansion IS the CALL
// (it belongs to the caller's level, depth - 1) unless we got there by looping inside the same expansion. `asCall` = false keeps
// the raw depth: a stop on that instruction means "inside the procedure".
int nestDepth(Simulator::Impl& I, Domain d, const InstrInfo* prev, bool asCall) {
    int n = (int)I.st.shadow[di(d)].size();
    const InstrInfo* ii = infoAtPc(I, d);
    if (!ii) return n;
    n += (int)ii->frames.size();
    if (asCall || prev)
        for (const CallSiteInfo& cs : I.callSites)
            if (!I.softret && cs.domain == d && cs.transferPc == ii->pc) { if (!sameExpansion(prev, ii)) n -= 1; break; }
    return n;
}
}  // namespace

RunResult Simulator::stepOver(Domain d) {
    // One step is one SOURCE line: the instructions the assembler generated for a CALL (softret: store the return id, branch)
    // belong to that line, and so does everything executed inside the callee until it returns.
    Impl& I = *p_;
    const InstrInfo* first = infoAtPc(I, d);
    const int d0 = nestDepth(I, d, nullptr, true);
    const int line0 = first ? first->loc.line : 0;
    const bool firstSynthetic = first && first->synthetic;
    const InstrInfo* prev = first;
    RunResult r = step(d);
    for (int guard = 0; guard < 10000000 && r.reason == StopReason::InstrLimit; guard++) {
        if (I.cancelHook && I.cancelHook()) { r.reason = StopReason::Cancelled; r.detail = "cancelled"; break; }
        const InstrInfo* now = infoAtPc(I, d);
        bool sameCallSeq = firstSynthetic && now && now->synthetic && now->loc.line == line0 && now->frames.size() == (first ? first->frames.size() : 0);
        if (nestDepth(I, d, prev, true) > d0 || sameCallSeq) { prev = now; r = step(d); }
        else break;
    }
    return r;
}

RunResult Simulator::stepOut(Domain d) {
    Impl& I = *p_;
    const InstrInfo* prev = infoAtPc(I, d);
    const int d0 = nestDepth(I, d, nullptr, false);
    RunResult r = step(d);
    if (d0 <= 0) return r;                                   // not inside a procedure: nothing to get out of
    for (int guard = 0; guard < 10000000 && r.reason == StopReason::InstrLimit; guard++) {
        if (I.cancelHook && I.cancelHook()) { r.reason = StopReason::Cancelled; r.detail = "cancelled"; break; }
        if (nestDepth(I, d, prev, true) < d0) break;
        prev = infoAtPc(I, d);
        r = step(d);
    }
    return r;
}

void Simulator::setCancelHook(std::function<bool()> hook) { p_->cancelHook = std::move(hook); }

RunResult Simulator::runToPc(Domain d, uint32_t pc) {
    int id = addBreakpoint(d, pc, "");
    for (auto& b : p_->bps) if (b.id == id) b.oneShot = true;
    RunLimits l;
    RunResult r = run(l);
    removeBreakpoint(id);
    return r;
}

uint32_t Simulator::pc(Domain d) const { return d == Domain::Main ? p_->st.m.pc : p_->st.p.pc; }
bool Simulator::halted(Domain d) const { return d == Domain::Main ? p_->st.m.halted : p_->st.p.halted; }
bool Simulator::faulted(Domain d) const { return d == Domain::Main ? p_->st.m.faulted : p_->st.p.faulted; }
const Fault* Simulator::fault(Domain d) const { return p_->st.hasFault[di(d)] ? &p_->st.fault[di(d)] : nullptr; }
bool Simulator::inHandler(Domain d) const { return p_->st.ev[di(d)].active; }
uint64_t Simulator::pendingEvents(Domain d) const { return p_->st.ev[di(d)].pending; }
std::vector<EventRecord> Simulator::eventLog() const { return p_->st.evlog; }

// ---- hardware events (spec v31 16) --------------------------------------------------------------------------------------------------
void Simulator::Impl::raiseEvent(int domain, int id) {
    EventState& e = st.ev[domain];
    if (!(e.pending >> id & 1)) e.raisedAt[id] = st.now;        // a second event with the same id while one is pending is merged
    e.pending |= (uint64_t)1 << id;
    emit([&](Event& ev) { ev.type = EventType::HwEvent; ev.domain = domain; ev.what = "raise"; ev.a = id; });
}

// Called when an ordinary BR with event_safe=1 has retired; `pc` of the domain is already the resolved next PC.
void Simulator::Impl::eventCheckpoint(int domain) {
    EventState& e = st.ev[domain];
    if (!e.pending || e.active) return;                         // nothing pending, or a handler is running (no nesting)
    if (domain == 0 && st.m.cascActive) return;                 // a cascade is never interrupted
    int id = -1;
    for (int i = 0; i < 64; i++) if ((e.pending >> i & 1) && vecPc[domain][i] >= 0) { id = i; break; }     // lowest id first (priority is the profile's business)
    if (id < 0) return;                                         // no valid vector: the event stays pending
    uint32_t& pc = domain == 0 ? st.m.pc : st.p.pc;
    Cycle& ready = domain == 0 ? st.m.readyAt : st.p.readyAt;
    const uint32_t limit = img[domain].codeLimit;
    if ((uint32_t)vecPc[domain][id] >= limit) { raise(domain == 0 ? Domain::Main : Domain::Peri, pc, "fault_event_vector", "event vector " + std::to_string(id) + " is outside the code"); return; }
    EventRecord r;
    r.domain = domain == 0 ? Domain::Main : Domain::Peri;
    r.id = id;
    r.raisedAt = e.raisedAt[id];
    r.dispatchedAt = st.now;
    r.resumePc = pc;
    r.vectorPc = (uint32_t)vecPc[domain][id];
    r.dispatched = true;
    st.evlog.push_back(r);
    e.pending &= ~((uint64_t)1 << id);
    e.active = true;
    e.resumeValid = true;
    e.resumePc = pc;
    e.cause = id;
    pc = r.vectorPc;
    ready += (Cycle)(lat.eventDispatch + eventDispatchExtra);
    emit([&](Event& ev) { ev.type = EventType::HwEvent; ev.domain = domain; ev.what = "dispatch"; ev.pc = r.vectorPc; ev.a = id; ev.b = r.resumePc; });
}

bool Simulator::Impl::doEret(int domain, OpCtx& oc) {
    EventState& e = st.ev[domain];
    const Domain dm = domain == 0 ? Domain::Main : Domain::Peri;
    if (!e.active || !e.resumeValid) { raise(dm, oc.pc, "fault_bad_eret_context", "BR.ERET outside an event handler (handler_active = 0 or no resume PC is saved)"); return false; }
    if (e.resumePc >= img[domain].codeLimit) { raise(dm, oc.pc, "fault_branch_target", "the saved resume PC " + std::to_string(e.resumePc) + " is outside the code"); return false; }
    oc.next = e.resumePc;                                       // full-width resume PC; no stack, and ERET itself never dispatches another event
    oc.latency = lat.eret + eretExtra;
    for (size_t i = st.evlog.size(); i-- > 0;)
        if (st.evlog[i].domain == dm && st.evlog[i].dispatched && !st.evlog[i].returned) {
            st.evlog[i].returned = true;
            st.evlog[i].returnedAt = st.now;
            break;
        }
    emit([&](Event& ev) { ev.type = EventType::HwEvent; ev.domain = domain; ev.what = "eret"; ev.pc = oc.pc; ev.a = e.cause; ev.b = e.resumePc; });
    e.active = false;
    e.resumeValid = false;
    e.cause = -1;
    return true;
}

uint64_t Simulator::instructions(Domain d) const { return d == Domain::Main ? p_->st.m.instrs : p_->st.p.instrs; }

int Simulator::regionSize(const std::string& region) const {
    if (region == "scratch" || region == "csrbuf" || region == "rx" || region == "slot" || region == "meta") return 32;
    if (region == "opa" || region == "opb") return 2;
    if (region == "out" || region == "status") return 4;
    if (region == "const") return p_->img[0].present ? (int)p_->constSize : 0;
    return -1;
}

std::vector<uint8_t> Simulator::readRegion(const std::string& region, int off, int len) const {
    std::vector<uint8_t> v;
    p_->regionBytes(region, off, len, v);
    return v;
}
bool Simulator::writeRegion(const std::string& region, int off, const std::vector<uint8_t>& bytes) {
    Impl& I = *p_;
    uint8_t* base = nullptr;
    int size = 0;
    if (region == "scratch") { base = I.st.m.scr; size = 32; }
    else if (region == "csrbuf") { base = I.st.m.buf; size = 32; }
    else if (region == "rx") { base = I.st.p.rx; size = 32; }
    else if (region == "slot") { base = I.st.slot.payload; size = 32; }
    if (!base || off < 0 || (size_t)off + bytes.size() > (size_t)size) return false;
    std::copy(bytes.begin(), bytes.end(), base + off);
    return true;
}
int64_t Simulator::signal(const std::string& name) const { return p_->signal(name); }
bool Simulator::setSignal(const std::string& name, int64_t v) {
    if (p_->sigId(name) < 0) return false;
    p_->setSignal(name, v);
    return true;
}
std::vector<std::string> Simulator::signalNames() const {
    std::vector<std::string> v;
    for (auto& s : p_->sigs) v.push_back(s.name);
    return v;
}
uint8_t Simulator::csr(uint32_t addr) const {
    auto it = p_->st.csr.find(addr);
    return it == p_->st.csr.end() ? 0 : it->second;
}
bool Simulator::lookupLabel(Domain d, const std::string& name, uint32_t& pc) const {
    auto it = p_->labels.find((d == Domain::Main ? "main:" : "peri:") + name);
    if (it == p_->labels.end()) return false;
    pc = it->second;
    return true;
}
bool Simulator::eval(const std::string& expr, int64_t& value, std::string& error) const {
    Expr e;
    if (!p_->compile(expr, e, error)) return false;
    value = e.eval(*p_);
    return true;
}

}  // namespace sim
}  // namespace asmcore
