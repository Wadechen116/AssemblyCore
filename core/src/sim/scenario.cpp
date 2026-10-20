// Scenario scripts: a tiny command language over the public Simulator API (docs/SIM.md).
//
//   run 5000                         advance 5000 cycles
//   run until slot.valid max 100000  run until the expression is true (or the cycle budget is used up)
//   run idle                         run until both domains stop
//   step main | step peri [n]        execute instructions of one domain
//   break main:start [if expr]       breakpoint at a label (or a number)
//   watch scratch[8..9] [if expr]    stop when it changes
//   inject at 5000 nack dev=0 count=2        inject in 100 signal pgl_pending=1
//   poke scratch[0] = 12 34          set signal pgl_pending = 1
//   expect expr [:: message]         check; expect_stop / expect_fault / expect_pc
//   dump | log text | history 1000 50 | rewind 4000 | trace on|off
#include <algorithm>
#include <sstream>

#include "impl.h"

namespace asmcore {
namespace sim {

namespace {

const char* reasonName(StopReason r) {
    static const char* n[] = {"none", "cycle_limit", "instr_limit", "breakpoint", "watchpoint", "fault", "idle", "condition"};
    return n[(int)r];
}

struct Runner {
    Simulator& sim;
    ScenarioResult out;
    const std::map<std::string, ScenarioCommand>* extra = nullptr;
    RunResult last;
    std::string traceBuf;
    bool tracing = false;
    int lineNo = 0;

    explicit Runner(Simulator& s) : sim(s) {}

    void log(const std::string& m) { out.log.push_back(m); }
    void fail(const std::string& m) { out.pass = false; out.failures++; log("FAIL line " + std::to_string(lineNo) + ": " + m); }
    void check(bool ok, const std::string& what) { out.checks++; if (ok) log("ok   line " + std::to_string(lineNo) + ": " + what); else fail(what); }

    bool domainOf(const std::string& w, Domain& d) {
        std::string l = lower(w);
        if (l == "main") { d = Domain::Main; return true; }
        if (l == "peri") { d = Domain::Peri; return true; }
        return false;
    }
    bool pcOf(Domain d, const std::string& t, uint32_t& pc) {
        std::string name = t;
        size_t c = name.find(':');
        if (c != std::string::npos) name = name.substr(c + 1);
        if (sim.lookupLabel(d, name, pc)) return true;
        int64_t v;
        std::string e;
        if (evalExpr(t, nullptr, v, e) && v >= 0) { pc = (uint32_t)v; return true; }
        return false;
    }
    static std::string rest(const std::vector<std::string>& w, size_t from) {
        std::string s;
        for (size_t i = from; i < w.size(); i++) s += (i > from ? " " : "") + w[i];
        return s;
    }
    void report(const RunResult& r) {
        last = r;
        log("     stop: " + std::string(reasonName(r.reason)) + " at cycle " + std::to_string(r.now) + (r.detail.empty() ? "" : " (" + r.detail + ")"));
    }
    void flushTrace() {
        if (!traceBuf.empty()) { log("     trace:\n" + traceBuf); traceBuf.clear(); }
    }

    void command(const std::vector<std::string>& w, const std::string& raw) {
        const std::string c = lower(w[0]);
        if (c == "run") {
            RunLimits l;
            l.maxCycles = 0;
            if (w.size() >= 2 && lower(w[1]) == "until") {
                size_t maxAt = w.size();
                for (size_t i = 2; i < w.size(); i++) if (lower(w[i]) == "max") { maxAt = i; break; }
                l.untilExpr = rest(std::vector<std::string>(w.begin(), w.begin() + (long)maxAt), 2);
                int64_t n = 1000000;
                std::string e;
                if (maxAt + 1 < w.size()) evalExpr(w[maxAt + 1], nullptr, n, e);
                l.maxCycles = (Cycle)n;
            } else if (w.size() >= 2 && lower(w[1]) == "idle") {
                int64_t n = 10000000;
                std::string e;
                if (w.size() >= 4 && lower(w[2]) == "max") evalExpr(w[3], nullptr, n, e);
                l.maxCycles = (Cycle)n;
            } else if (w.size() >= 2) {
                int64_t n = 0;
                std::string e;
                if (!evalExpr(w[1], nullptr, n, e) || n < 0) { fail("run: cycle count expected"); return; }
                l.maxCycles = (Cycle)n;
                l.stopOnIdle = false;
            } else { fail("run: argument expected"); return; }
            report(sim.run(l));
            flushTrace();
        } else if (c == "step" || c == "stepover") {
            Domain d;
            if (w.size() < 2 || !domainOf(w[1], d)) { fail(c + ": main or peri expected"); return; }
            int64_t n = 1;
            std::string e;
            if (w.size() >= 3) evalExpr(w[2], nullptr, n, e);
            for (int64_t i = 0; i < n; i++) { RunResult r = c == "step" ? sim.step(d) : sim.stepOver(d); report(r); if (r.reason != StopReason::InstrLimit) break; }
            flushTrace();
        } else if (c == "break") {
            Domain d;
            std::string spec = w.size() >= 2 ? w[1] : "";
            std::string dom = spec.substr(0, spec.find(':'));
            if (w.size() < 2 || !domainOf(dom, d)) { fail("break: expected main:<label> or peri:<label>"); return; }
            uint32_t pc;
            if (!pcOf(d, spec, pc)) { fail("break: unknown label or address '" + spec + "'"); return; }
            std::string cond;
            for (size_t i = 2; i < w.size(); i++) if (lower(w[i]) == "if") cond = rest(w, i + 1);
            int id = sim.addBreakpoint(d, pc, cond);
            log("     breakpoint " + std::to_string(id) + " at " + spec + " (pc " + std::to_string(pc) + ")");
        } else if (c == "delete") {
            int64_t id = 0;
            std::string e;
            if (w.size() < 2 || !evalExpr(w[1], nullptr, id, e)) { fail("delete: id expected"); return; }
            if (!sim.removeBreakpoint((int)id) && !sim.removeWatch((int)id)) fail("delete: no breakpoint or watch " + std::to_string(id));
        } else if (c == "watch") {
            size_t ifAt = w.size();
            for (size_t i = 1; i < w.size(); i++) if (lower(w[i]) == "if") { ifAt = i; break; }
            std::string target = rest(std::vector<std::string>(w.begin(), w.begin() + (long)ifAt), 1);
            std::string cond = ifAt < w.size() ? rest(w, ifAt + 1) : "";
            std::string err;
            int id = sim.addWatch(target, cond, err);
            if (id < 0) fail("watch: " + err); else log("     watch " + std::to_string(id) + " on " + target);
        } else if (c == "inject") {
            if (w.size() < 4) { fail("inject: at <cycle> <command> | in <cycles> <command>"); return; }
            int64_t t = 0;
            std::string e;
            if (!evalExpr(w[2], nullptr, t, e) || t < 0) { fail("inject: bad time"); return; }
            Cycle at = lower(w[1]) == "in" ? sim.now() + (Cycle)t : (Cycle)t;
            if (lower(w[1]) != "in" && lower(w[1]) != "at") { fail("inject: 'at' or 'in' expected"); return; }
            if (!sim.inject(at, rest(w, 3), e)) fail("inject: " + e);
        } else if (c == "poke") {
            size_t eq = raw.find('=');
            if (eq == std::string::npos) { fail("poke: region[off] = bytes"); return; }
            std::string lhs = trim(raw.substr(4, eq - 4)), rhs = trim(raw.substr(eq + 1));
            size_t lb = lhs.find('[');
            if (lb == std::string::npos || lhs.back() != ']') { fail("poke: region[off] = bytes"); return; }
            int64_t off = 0;
            std::string e;
            if (!evalExpr(lhs.substr(lb + 1, lhs.size() - lb - 2), nullptr, off, e)) { fail("poke: bad offset"); return; }
            std::vector<uint8_t> bytes;
            std::istringstream in(rhs);
            std::string t;
            while (in >> t) { int64_t v; if (!evalExpr(t, nullptr, v, e)) { fail("poke: bad byte '" + t + "'"); return; } bytes.push_back((uint8_t)v); }
            if (!sim.writeRegion(lower(trim(lhs.substr(0, lb))), (int)off, bytes)) fail("poke: cannot write there");
        } else if (c == "set") {
            size_t eq = raw.find('=');
            int64_t v = 0;
            std::string e;
            if (eq == std::string::npos || !evalExpr(trim(raw.substr(eq + 1)), nullptr, v, e)) { fail("set: signal = value"); return; }
            std::string name = trim(raw.substr(3, eq - 3));
            if (!sim.setSignal(name, v)) fail("set: unknown signal '" + name + "'");
        } else if (c == "expect") {
            std::string body = trim(raw.substr(6));
            std::string msg;
            size_t sep = body.find("::");
            if (sep != std::string::npos) { msg = trim(body.substr(sep + 2)); body = trim(body.substr(0, sep)); }
            int64_t v = 0;
            std::string e;
            if (!sim.eval(body, v, e)) { fail("expect: " + e + " in '" + body + "'"); return; }
            check(v != 0, (msg.empty() ? body : msg) + (v == 0 ? "   [" + body + " is false]" : ""));
        } else if (c == "expect_stop") {
            std::string want = w.size() >= 2 ? lower(w[1]) : "";
            check(want == reasonName(last.reason), "stop reason is " + want + " (was " + reasonName(last.reason) + ")");
        } else if (c == "expect_fault") {
            Domain d;
            if (w.size() < 2 || !domainOf(w[1], d)) { fail("expect_fault: main or peri expected"); return; }
            const Fault* f = sim.fault(d);
            if (w.size() >= 3) check(f && f->code == w[2], std::string(domainName(d)) + " fault is " + w[2] + (f ? " (was " + f->code + ")" : " (no fault)"));
            else check(f != nullptr, std::string(domainName(d)) + " has faulted");
        } else if (c == "expect_pc") {
            Domain d;
            uint32_t pc;
            if (w.size() < 3 || !domainOf(w[1], d) || !pcOf(d, w[2], pc)) { fail("expect_pc: main|peri <label|pc>"); return; }
            check(sim.pc(d) == pc, std::string(domainName(d)) + " pc is " + w[2] + " (was " + std::to_string(sim.pc(d)) + ")");
        } else if (c == "dump") {
            log("     state:\n" + sim.dumpState());
        } else if (c == "log") {
            log("     " + trim(raw.substr(3)));
        } else if (c == "history") {
            int64_t i = 0, k = 0;
            std::string e;
            if (w.size() < 3 || !evalExpr(w[1], nullptr, i, e) || !evalExpr(w[2], nullptr, k, e)) { fail("history: interval keep"); return; }
            sim.enableHistory((Cycle)i, (size_t)k);
        } else if (c == "rewind") {
            int64_t t = 0;
            std::string e;
            if (w.size() < 2 || !evalExpr(w[1], nullptr, t, e)) { fail("rewind: cycle expected"); return; }
            if (!sim.rewindTo((Cycle)t)) fail("rewind: no snapshot at or before cycle " + std::to_string(t) + " (use `history` first)");
        } else if (c == "trace") {
            bool on = w.size() >= 2 && lower(w[1]) == "on";
            if (on && !tracing) { sim.addSink(makeTextTrace(&traceBuf, false)); tracing = true; }
            if (!on) log("     (the trace sink stays attached; `trace off` only hides nothing -- start a new Simulator to detach)");
        } else if (extra && extra->count(c)) {
            std::string msg;
            out.checks++;
            if ((*extra->find(c)).second(sim, w, msg)) log("ok   line " + std::to_string(lineNo) + ": " + c + (msg.empty() ? "" : " - " + msg));
            else fail(c + (msg.empty() ? " failed" : ": " + msg));
        } else {
            fail("unknown command '" + w[0] + "'");
        }
    }
};

}  // namespace

ScenarioResult runScenario(Simulator& sim, const std::string& script, const std::map<std::string, ScenarioCommand>* extra) {
    Runner r(sim);
    r.extra = extra;
    std::istringstream in(script);
    std::string raw;
    while (std::getline(in, raw)) {
        r.lineNo++;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        // a comment starts with ';' at the start of a token run (so `::` messages and expressions keep working)
        size_t sc = raw.find(';');
        std::string line = trim(sc == std::string::npos ? raw : raw.substr(0, sc));
        if (line.empty()) continue;
        std::vector<std::string> w;
        std::istringstream ls(line);
        std::string t;
        while (ls >> t) w.push_back(t);
        r.command(w, line);
    }
    r.out.checks = std::max(r.out.checks, 0);
    return r.out;
}

}  // namespace sim
}  // namespace asmcore
