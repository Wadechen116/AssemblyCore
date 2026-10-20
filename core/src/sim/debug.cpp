// Debugging: breakpoints, watches, call stack, state dump, snapshots and history, injection, event sinks (text, CSV, VCD).
#include <algorithm>
#include <cstdio>
#include <regex>
#include <sstream>

#include "impl.h"

namespace asmcore {
namespace sim {

Cycle Snapshot::time() const { return data_ ? static_cast<const State*>(data_.get())->now : 0; }

// ---- breakpoints and watches ---------------------------------------------------------------------------------------------------
int Simulator::addBreakpoint(Domain d, uint32_t pc, const std::string& condition) {
    Breakpoint b;
    b.id = p_->nextId++;
    b.domain = d;
    b.pc = pc;
    b.condition = condition;
    p_->bps.push_back(b);
    return b.id;
}
bool Simulator::removeBreakpoint(int id) {
    auto& v = p_->bps;
    auto it = std::remove_if(v.begin(), v.end(), [&](const Breakpoint& b) { return b.id == id; });
    bool any = it != v.end();
    v.erase(it, v.end());
    return any;
}
std::vector<Breakpoint> Simulator::breakpoints() const { return p_->bps; }

int Simulator::addWatch(const std::string& target, const std::string& condition, std::string& error) {
    Impl& I = *p_;
    WatchDef w;
    w.id = I.nextId;
    w.target = target;
    w.condition = condition;
    static const std::regex region("^\\s*(scratch|csrbuf|rx|slot|meta|opa|opb|out|status)\\s*\\[\\s*(\\d+)\\s*(?:\\.\\.\\s*(\\d+))?\\s*\\]\\s*$", std::regex::icase);
    static const std::regex csr("^\\s*csr\\s+(\\S+)\\s*$", std::regex::icase);
    static const std::regex sig("^\\s*signal\\s+(\\S+)\\s*$", std::regex::icase);
    std::smatch m;
    if (std::regex_match(target, m, region)) {
        w.kind = WatchDef::Region;
        w.region = lower(m[1]);
        w.off = std::stoi(m[2]);
        w.len = m[3].matched ? std::stoi(m[3]) - w.off + 1 : 1;
        std::vector<uint8_t> t;
        if (w.len < 1 || !I.regionBytes(w.region, w.off, w.len, t)) { error = "bad range in '" + target + "'"; return -1; }
    } else if (std::regex_match(target, m, csr)) {
        int64_t a = 0;
        std::string e;
        if (!evalExpr(m[1], nullptr, a, e) || a < 0) { error = "bad CSR address"; return -1; }
        w.kind = WatchDef::Csr;
        w.addr = (uint32_t)a;
    } else {
        std::string t = std::regex_match(target, m, sig) ? std::string(m[1]) : target;
        w.kind = WatchDef::Expression;
        if (!I.compile(t, w.expr, error)) return -1;
    }
    if (!condition.empty()) {
        if (!I.compile(condition, w.cond, error)) return -1;
        w.haveCond = true;
    }
    I.nextId++;
    I.watches.push_back(w);
    return w.id;
}
bool Simulator::removeWatch(int id) {
    auto& v = p_->watches;
    auto it = std::remove_if(v.begin(), v.end(), [&](const WatchDef& w) { return w.id == id; });
    bool any = it != v.end();
    v.erase(it, v.end());
    return any;
}

static void watchValue(const Simulator::Impl& I, const WatchDef& w, std::vector<uint8_t>& bytes, int64_t& val) {
    bytes.clear();
    val = 0;
    if (w.kind == WatchDef::Region) I.regionBytes(w.region, w.off, w.len, bytes);
    else if (w.kind == WatchDef::Csr) { auto it = I.st.csr.find(w.addr); bytes.push_back(it == I.st.csr.end() ? 0 : it->second); }
    else val = w.expr.eval(I);
}
void Simulator::Impl::primeWatches() {
    for (WatchDef& w : watches) watchValue(*this, w, w.last, w.lastVal);
}
bool Simulator::Impl::evalWatches(int& id, std::string& detail) {
    for (WatchDef& w : watches) {
        std::vector<uint8_t> b;
        int64_t v;
        watchValue(*this, w, b, v);
        bool changed = w.kind == WatchDef::Expression ? v != w.lastVal : b != w.last;
        if (!changed) continue;
        std::ostringstream o;
        o << "watch " << w.id << " (" << w.target << ") changed ";
        if (w.kind == WatchDef::Expression) o << w.lastVal << " -> " << v;
        else {
            char t[8];
            for (uint8_t x : w.last) { std::snprintf(t, sizeof t, "%02X", x); o << t; }
            o << " -> ";
            for (uint8_t x : b) { std::snprintf(t, sizeof t, "%02X", x); o << t; }
        }
        w.last = b;
        w.lastVal = v;
        if (w.haveCond && !w.cond.eval(*this)) continue;
        id = w.id;
        detail = o.str();
        return true;
    }
    return false;
}

// ---- call stack ------------------------------------------------------------------------------------------------------------------
void Simulator::Impl::trackCalls(Domain d, uint32_t pcBefore) {
    if (!softret) return;
    int i = di(d);
    uint32_t pcNow = d == Domain::Main ? st.m.pc : st.p.pc;
    if (pcNow == pcBefore) return;
    auto it = callByTransfer[i].find(pcBefore);
    if (it != callByTransfer[i].end()) {
        CallFrame f;
        f.proc = it->second->callee;
        f.callSite = it->second->loc;
        f.returnPc = it->second->returnPc;
        st.shadow[i].push_back(f);
    }
    while (!st.shadow[i].empty() && pcNow == st.shadow[i].back().returnPc) st.shadow[i].pop_back();
}

std::vector<CallFrame> Simulator::Impl::stack(Domain d) const {
    int i = di(d);
    std::vector<CallFrame> out;
    uint32_t pc = d == Domain::Main ? st.m.pc : st.p.pc;
    auto it = instrAt[i].find(pc);
    if (it != instrAt[i].end()) {
        for (size_t k = it->second->frames.size(); k-- > 0;) {
            CallFrame f;
            f.proc = it->second->frames[k].proc;
            f.callSite = it->second->frames[k].callSite;
            f.inlined = true;
            out.push_back(f);
        }
    }
    for (size_t k = st.shadow[i].size(); k-- > 0;) out.push_back(st.shadow[i][k]);
    return out;
}
std::vector<CallFrame> Simulator::callStack(Domain d) const { return p_->stack(d); }

// ---- state dump ------------------------------------------------------------------------------------------------------------------
static std::string hexRows(const uint8_t* b, int n) {
    std::string s;
    char t[8];
    for (int i = 0; i < n; i++) {
        if (i && i % 16 == 0) s += "\n                 ";
        std::snprintf(t, sizeof t, "%02X ", b[i]);
        s += t;
    }
    return s;
}
std::string Simulator::Impl::dump() const {
    std::ostringstream o;
    char t[96];
    o << "cycle: " << st.now << "\n";
    auto domain = [&](const char* name, uint32_t pc, bool halted, bool faulted, uint64_t n, int i) {
        o << name << ".pc: " << (std::snprintf(t, sizeof t, "0x%04X", pc), t) << "\n";
        o << name << ".state: " << (faulted ? "FAULTED" : halted ? "halted" : "running") << "  instructions=" << n << "\n";
        if (st.hasFault[i]) o << name << ".fault: " << st.fault[i].code << " at pc " << st.fault[i].pc << " (" << st.fault[i].msg << ")\n";
    };
    domain("main", st.m.pc, st.m.halted, st.m.faulted, st.m.instrs, 0);
    o << "main.scratch:  " << hexRows(st.m.scr, 32) << "\n";
    o << "main.csrbuf:   " << hexRows(st.m.buf, 32) << "\n";
    std::snprintf(t, sizeof t, "opa=0x%04X opb=0x%04X out=0x%08X", st.m.opa, st.m.opb, st.m.out);
    o << "main.alu: " << t << "\n";
    o << "main.flags: cmp=" << st.m.cmp << " zero=" << st.m.zero << " neg=" << st.m.neg << " carry=" << st.m.carry << " borrow=" << st.m.borrow << " ovs=" << st.m.ovs << " ov16=" << st.m.ov16 << "\n";
    o << "main.cascade: active=" << st.m.cascActive << " sel=" << (int)st.m.cascSel << " step=" << (int)st.m.cascStep << "\n";
    o << "main.event: pending=0x" << std::hex << st.ev[0].pending << std::dec << " handler_active=" << st.ev[0].active << " resume_pc=" << st.ev[0].resumePc << "\n";
    domain("peri", st.p.pc, st.p.halted, st.p.faulted, st.p.instrs, 1);
    o << "peri.rx:       " << hexRows(st.p.rx, 32) << "\n";
    if (st.p.xfer.active) o << "peri.xfer: in flight to I2C " << st.p.xfer.dev << "\n";
    o << "peri.event: pending=0x" << std::hex << st.ev[1].pending << std::dec << " handler_active=" << st.ev[1].active << " resume_pc=" << st.ev[1].resumePc << "\n";
    o << "slot: valid=" << st.slot.valid << " busy=" << st.slot.busy << " overrun=" << st.slot.overrun << " service_id=" << (int)st.slot.svc << "\n";
    o << "slot.payload:  " << hexRows(st.slot.payload, 32) << "\n";
    std::vector<std::string> names;
    for (auto& s : sigs) names.push_back(s.name);
    std::sort(names.begin(), names.end());
    for (auto& n : names) o << "signal " << n << ": " << signal(n) << "\n";
    for (auto& kv : st.csr) o << "csr[0x" << (std::snprintf(t, sizeof t, "%X", kv.first), t) << "]: " << (int)kv.second << "\n";
    o << "pending events: " << st.q.size() << "\n";
    for (int i = 0; i < 2; i++) {
        auto stk = stack(i == 0 ? Domain::Main : Domain::Peri);
        for (size_t k = 0; k < stk.size(); k++) o << (i == 0 ? "main" : "peri") << ".call[" << k << "]: " << stk[k].proc << " from " << stk[k].callSite.file << ":" << stk[k].callSite.line << (stk[k].inlined ? " (inlined)" : "") << "\n";
    }
    return o.str();
}
std::string Simulator::dumpState() const { return p_->dump(); }

std::string Simulator::diffDumps(const std::string& before, const std::string& after) {
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        std::istringstream in(s);
        std::string l;
        while (std::getline(in, l)) v.push_back(l);
        return v;
    };
    // a dump line "key: value" may continue on following indented lines; compare by logical entries
    auto join = [](const std::vector<std::string>& lines) {
        std::vector<std::string> out;
        for (auto& l : lines) {
            if (!l.empty() && l[0] == ' ' && !out.empty()) out.back() += " " + trim(l);
            else out.push_back(l);
        }
        return out;
    };
    std::vector<std::string> a = join(split(before)), b = join(split(after));
    std::multiset<std::string> sa(a.begin(), a.end()), sb(b.begin(), b.end());
    std::ostringstream o;
    for (auto& l : a) if (!sb.count(l) && l.compare(0, 6, "cycle:") != 0) o << "- " << l << "\n";
    for (auto& l : b) if (!sa.count(l) && l.compare(0, 6, "cycle:") != 0) o << "+ " << l << "\n";
    return o.str();
}

// ---- snapshots and history -------------------------------------------------------------------------------------------------------
Snapshot Simulator::snapshot() const {
    Snapshot s;
    s.data_ = std::make_shared<const State>(p_->st);
    return s;
}
void Simulator::restore(const Snapshot& s) {
    if (!s.data_) return;
    p_->st = *static_cast<const State*>(s.data_.get());
    p_->lastWrites.clear();
    p_->stopRequested = false;
    p_->skipValid = false;
    p_->primeWatches();
}
void Simulator::enableHistory(Cycle interval, size_t keep) {
    p_->histInterval = interval;
    p_->histKeep = keep;
    p_->history.clear();
    p_->nextSnap = p_->st.now;
    if (interval) p_->takeSnapshotIfDue();
}
void Simulator::Impl::takeSnapshotIfDue() {
    if (!histInterval || st.now < nextSnap) return;
    history.push_back({st.now, std::make_shared<const State>(st)});
    while (histKeep && history.size() > histKeep) history.pop_front();
    nextSnap = st.now + histInterval;
}
bool Simulator::rewindTo(Cycle t) {
    Impl& I = *p_;
    const State* best = nullptr;
    for (auto& h : I.history) if (h.first <= t) best = h.second.get();
    if (!best) return false;
    State keep = *best;
    I.history.erase(std::remove_if(I.history.begin(), I.history.end(), [&](const std::pair<Cycle, std::shared_ptr<const State>>& h) { return h.first > keep.now; }), I.history.end());
    I.st = keep;
    I.lastWrites.clear();
    I.skipValid = false;
    I.nextSnap = I.st.now + I.histInterval;
    bool old = I.suppressStops;
    I.suppressStops = true;
    RunLimits l;
    l.maxCycles = t > I.st.now ? t - I.st.now : 0;
    l.stopOnIdle = false;
    if (t > I.st.now) I.runLoop(l, -1, false, 0, 0);
    I.suppressStops = old;
    return true;
}

// ---- environment control and sinks -----------------------------------------------------------------------------------------------
bool Simulator::inject(Cycle at, const std::string& command, std::string& error) {
    std::istringstream in(command);
    std::string verb;
    in >> verb;
    verb = lower(verb);
    static const char* known[] = {"nack", "signal", "slot_busy", "fault_clear", "csr", "note", "event"};
    bool ok = false;
    for (const char* k : known) if (verb == k) ok = true;
    if (!ok) { error = "unknown inject command '" + verb + "'"; return false; }
    p_->schedule(at, TimedEvent::Command, 0, 0, command);
    return true;
}
bool Simulator::injectNow(const std::string& command, std::string& error) {
    std::istringstream in(command);
    std::string verb;
    in >> verb;
    verb = lower(verb);
    static const char* known[] = {"nack", "signal", "slot_busy", "fault_clear", "csr", "note", "event"};
    bool ok = false;
    for (const char* k : known) if (verb == k) ok = true;
    if (!ok) { error = "unknown inject command '" + verb + "'"; return false; }
    p_->applyCommand(command);
    return true;
}
void Simulator::addSink(std::shared_ptr<EventSink> sink) { p_->sinks.push_back(std::move(sink)); }

namespace {

const char* typeName(EventType t) {
    static const char* n[] = {"issue", "retire", "write", "csr", "signal", "bus", "publish", "consume", "overrun", "fault", "inject", "note"};
    return n[(int)t];
}

class TextTrace : public EventSink {
public:
    TextTrace(std::string* out, bool only) : out_(out), only_(only) {}
    void onEvent(const Event& e) override {
        if (only_ && e.type != EventType::Issue) return;
        char b[64];
        std::snprintf(b, sizeof b, "[%9llu] %c ", (unsigned long long)e.t, e.domain == 0 ? 'M' : e.domain == 1 ? 'P' : '-');
        *out_ += b;
        if (e.type == EventType::Issue) { std::snprintf(b, sizeof b, "%04X  ", e.pc); *out_ += b; *out_ += e.what; }
        else {
            *out_ += std::string("  ") + typeName(e.type) + " " + e.what;
            if (!e.text.empty()) *out_ += " " + e.text;
            if (e.type == EventType::CsrAccess || e.type == EventType::Signal || e.type == EventType::RegionWrite || e.type == EventType::Bus || e.type == EventType::Publish)
                *out_ += " (" + std::to_string(e.a) + ", " + std::to_string(e.b) + ")";
        }
        *out_ += "\n";
    }
private:
    std::string* out_;
    bool only_;
};

class CsvTrace : public EventSink {
public:
    explicit CsvTrace(std::string* out) : out_(out) { *out_ += "cycle,domain,type,pc,what,text,a,b,c\n"; }
    void onEvent(const Event& e) override {
        auto q = [](const std::string& s) { std::string r = "\""; for (char c : s) { if (c == '"') r += '"'; r += c; } return r + "\""; };
        *out_ += std::to_string(e.t) + "," + (e.domain == 0 ? "main" : e.domain == 1 ? "peri" : "sys") + "," + typeName(e.type) + "," + std::to_string(e.pc) + "," + q(e.what) + "," + q(e.text) + "," +
                 std::to_string(e.a) + "," + std::to_string(e.b) + "," + std::to_string(e.c) + "\n";
    }
private:
    std::string* out_;
};

}  // namespace

std::shared_ptr<EventSink> makeTextTrace(std::string* out, bool instructionsOnly) { return std::make_shared<TextTrace>(out, instructionsOnly); }
std::shared_ptr<EventSink> makeCsvTrace(std::string* out) { return std::make_shared<CsvTrace>(out); }

struct VcdSink::Rep {
    std::string* out;
    struct Ch { Cycle t; std::string name; int64_t v; };
    std::vector<Ch> ch;
};
VcdSink::VcdSink(std::string* out) : r_(std::make_shared<Rep>()) { r_->out = out; }
void VcdSink::onEvent(const Event& e) {
    if (e.type == EventType::Issue) r_->ch.push_back({e.t, e.domain == 0 ? "main_pc" : "peri_pc", (int64_t)e.pc});
    else if (e.type == EventType::Signal) { std::string n = e.what; for (char& c : n) if (c == '.') c = '_'; r_->ch.push_back({e.t, n, e.b}); }
    else if (e.type == EventType::Publish) r_->ch.push_back({e.t, "slot_valid", 1});
    else if (e.type == EventType::Consume) r_->ch.push_back({e.t, "slot_valid", 0});
    else if (e.type == EventType::Overrun) { r_->ch.push_back({e.t, "slot_overrun", 1}); }
    else if (e.type == EventType::Fault) r_->ch.push_back({e.t, e.domain == 0 ? "main_fault" : "peri_fault", 1});
}
void VcdSink::finish(Cycle end) {
    std::vector<std::string> names;
    for (auto& c : r_->ch) if (std::find(names.begin(), names.end(), c.name) == names.end()) names.push_back(c.name);
    std::ostringstream o;
    o << "$date asmcore $end\n$version AssemblyCore simulator $end\n$timescale 1 ns $end\n$scope module sim $end\n";
    auto id = [&](size_t i) { std::string s; size_t n = i; do { s += (char)('!' + n % 90); n /= 90; } while (n); return s; };
    for (size_t i = 0; i < names.size(); i++) o << "$var wire 32 " << id(i) << " " << names[i] << " $end\n";
    o << "$upscope $end\n$enddefinitions $end\n";
    Cycle last = ~(Cycle)0;
    std::map<std::string, int64_t> cur;
    for (auto& c : r_->ch) {
        if (cur.count(c.name) && cur[c.name] == c.v) continue;
        cur[c.name] = c.v;
        if (c.t != last) { o << "#" << c.t << "\n"; last = c.t; }
        std::string bits;
        uint32_t v = (uint32_t)c.v;
        if (!v) bits = "0";
        while (v) { bits.insert(bits.begin(), (char)('0' + (v & 1))); v >>= 1; }
        o << "b" << bits << " " << id((size_t)(std::find(names.begin(), names.end(), c.name) - names.begin())) << "\n";
    }
    o << "#" << end << "\n";
    *r_->out = o.str();
}

}  // namespace sim
}  // namespace asmcore
