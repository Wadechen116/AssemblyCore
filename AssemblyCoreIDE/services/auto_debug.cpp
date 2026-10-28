#include "auto_debug.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>

using namespace asmcore;
using namespace asmcore::sim;

namespace acide {

namespace {

const char* const kDomain[2] = {"Main", "Peri"};

bool isConditional(const std::string& text) {
    return text.compare(0, 7, "BR.COND") == 0 || text.compare(0, 5, "BR.IF") == 0;
}

// Sees every event once; keeps only what the checks need.
class Watcher : public EventSink {
public:
    struct Loop {                                    // the stretch since the last time the loop was left or something happened
        std::set<uint32_t> pcs;
        std::map<uint32_t, std::string> branches;    // the conditional branches in it
        Cycle start = 0;
        int64_t instrs = 0;
    };
    struct Occurrence { Cycle at = 0; uint32_t pc = 0; int domain = -1; int count = 0; std::string text; int64_t a = 0, b = 0; };

    Loop loop[2];
    Cycle lastProgress[2] = {0, 0};
    std::set<std::pair<int, uint32_t>> issued;
    std::vector<Occurrence> faults;
    Occurrence overrun;
    std::map<int64_t, Occurrence> nacks;             // by device address
    std::map<std::pair<int, int64_t>, std::vector<Cycle>> raised;       // (domain, id) -> when each pending event was raised
    std::map<std::pair<int, int64_t>, int> dispatched;
    int publishes = 0, consumes = 0;
    Cycle firstPublish = 0;
    uint32_t firstPublishPc = 0;
    bool anyPublish = false;

    void onEvent(const Event& e) override {
        const int d = e.domain == 0 || e.domain == 1 ? e.domain : -1;
        switch (e.type) {
            case EventType::Issue: {
                if (d < 0) break;
                issued.insert({d, e.pc});
                Loop& l = loop[d];
                if (l.pcs.count(e.pc)) {
                    l.instrs++;
                } else if (l.pcs.size() < 8) {
                    l.pcs.insert(e.pc);
                    l.instrs++;
                } else {                                                  // a ninth distinct instruction: this is no small loop (any more)
                    l = Loop();
                    l.pcs.insert(e.pc);
                    l.start = e.t;
                    l.instrs = 1;
                }
                if (isConditional(e.what)) l.branches[e.pc] = e.what;
                break;
            }
            case EventType::Fault: {
                Occurrence o;
                o.at = e.t; o.pc = e.pc; o.domain = d; o.text = e.what + ": " + e.text;
                faults.push_back(o);
                if (d >= 0) progress(d, e.t);
                break;
            }
            case EventType::Overrun:
                if (overrun.count++ == 0) { overrun.at = e.t; overrun.pc = e.pc; overrun.domain = d; overrun.a = e.a; }
                break;
            case EventType::Publish:
                publishes++;
                if (!anyPublish) { anyPublish = true; firstPublish = e.t; firstPublishPc = e.pc; }
                if (d >= 0) progress(d, e.t);
                break;
            case EventType::Consume:
                consumes++;
                for (int i = 0; i < 2; i++) progress(i, e.t);             // (a result taken is a step forward for the whole program)
                break;
            case EventType::Bus: {
                if (d >= 0) progress(d, e.t);
                if (e.b == 0) {
                    Occurrence& o = nacks[e.a];
                    if (o.count++ == 0) { o.at = e.t; o.pc = e.pc; o.domain = d; o.a = e.a; o.text = e.what; }
                }
                break;
            }
            case EventType::HwEvent:
                if (e.what == "raise") raised[{d, e.a}].push_back(e.t);
                else if (e.what == "dispatch") { dispatched[{d, e.a}]++; if (d >= 0) progress(d, e.t); }
                else if (d >= 0) progress(d, e.t);
                break;
            default: break;
        }
    }

private:
    void progress(int d, Cycle t) {
        lastProgress[d] = std::max(lastProgress[d], t);
    }
};

const InstrInfo* find(const Result& r, int domain, uint32_t pc) {
    const Domain d = domain == 0 ? Domain::Main : Domain::Peri;
    for (const InstrInfo& i : r.instrs) if (i.domain == d && i.pc == pc) return &i;
    return nullptr;
}

void place(AutoFinding& f, const Result& r) {
    if (!f.hasPc || f.domain < 0) return;
    if (const InstrInfo* i = find(r, f.domain, f.pc)) { f.file = i->loc.file; f.line = i->loc.line; }
}

}  // namespace

AutoDebugReport autoDebug(const Result& program, const SimConfig& cfg, const AutoDebugOptions& opt) {
    AutoDebugReport rep;
    Simulator sim(program, cfg);
    if (!sim.ok()) {
        for (const std::string& e : sim.configErrors()) rep.error += (rep.error.empty() ? "" : "\n") + e;
        if (rep.error.empty()) rep.error = "the simulator could not be made";
        return rep;
    }
    auto watch = std::make_shared<Watcher>();
    sim.addSink(watch);
    RunLimits lim;
    lim.maxCycles = opt.maxCycles;
    lim.maxMillis = opt.maxMillis;
    lim.stopOnIdle = true;
    const RunResult rr = sim.run(lim);
    rep.endAt = sim.now();
    rep.reason = rr.reason;
    rep.stopDetail = rr.detail;
    rep.instrs[0] = (int64_t)sim.instructions(Domain::Main);
    rep.instrs[1] = (int64_t)sim.instructions(Domain::Peri);
    rep.publishes = watch->publishes;
    rep.consumes = watch->consumes;
    const Watcher& w = *watch;
    const Cycle stuckAfter = opt.stuckCycles ? opt.stuckCycles : std::max<Cycle>(5000, opt.maxCycles / 10);
    auto add = [&](AutoFinding f) { place(f, program); rep.findings.push_back(std::move(f)); };

    // ---- faults
    for (size_t i = 0; i < w.faults.size() && i < 10; i++) {
        const Watcher::Occurrence& o = w.faults[i];
        AutoFinding f;
        f.sev = AutoFinding::Error; f.code = "AD-FAULT";
        f.domain = o.domain; f.hasPc = o.domain >= 0; f.pc = o.pc; f.at = o.at;
        f.title = std::string(o.domain >= 0 ? kDomain[o.domain] : "The simulator") + " faulted";
        f.detail = o.text;
        f.hint = "The sequencer stopped at this instruction. The code after ':' says what the hardware would have complained about; docs/SIM.md lists the fault codes.";
        add(f);
    }
    // ---- result slot overrun
    if (w.overrun.count > 0) {
        AutoFinding f;
        f.sev = AutoFinding::Error; f.code = "AD-OVERRUN";
        f.domain = w.overrun.domain; f.hasPc = w.overrun.domain >= 0; f.pc = w.overrun.pc; f.at = w.overrun.at; f.count = w.overrun.count;
        f.title = "Result slot overrun";
        f.detail = "Peri published a result (service " + std::to_string(w.overrun.a) + ") before Main took the previous one; the earlier result was lost" +
                   (w.overrun.count > 1 ? " (" + std::to_string(w.overrun.count) + " times)." : ".");
        f.hint = "Main is too slow for Peri: take the result sooner (check PERI_RESULT_VALID more often, or do less between two checks), or let Peri publish less often (a longer Peri timer: timer.peri.clock_div).";
        add(f);
    }
    // ---- stuck
    for (int d = 0; d < 2; d++) {
        const Watcher::Loop& l = w.loop[d];
        if (l.instrs < 10 || l.branches.empty()) continue;
        if (sim.halted(d == 0 ? Domain::Main : Domain::Peri) || sim.faulted(d == 0 ? Domain::Main : Domain::Peri)) continue;
        const Cycle from = std::max(l.start, w.lastProgress[d]);
        if (rep.endAt < from || rep.endAt - from < stuckAfter) continue;
        if (rr.reason == StopReason::Idle) continue;
        AutoFinding f;
        f.sev = AutoFinding::Warning; f.code = "AD-STUCK";
        f.domain = d; f.at = from; f.hasPc = true; f.pc = *l.pcs.begin();
        for (const auto& b : l.branches) { f.pc = b.first; break; }
        f.title = std::string(kDomain[d]) + " is waiting in a loop";
        f.detail = std::string(kDomain[d]) + " has gone round the same " + std::to_string(l.pcs.size()) + " instruction(s) since cycle " + std::to_string(from) +
                   " (" + std::to_string(rep.endAt - from) + " cycles, to the end of the run) and nothing has happened in between: no result published or taken, no bus transfer, no event. It waits for:";
        for (const auto& b : l.branches) f.detail += "\n    " + b.second;
        f.hint = "If this wait is by design (a long timer, the next frame) run for more cycles. Otherwise the condition above never becomes true: who is meant to make it true (the other sequencer, a timer, the frame timing, the device in the environment file)?";
        add(f);
    }
    // ---- I2C devices that did not answer
    for (const auto& kv : w.nacks) {
        const Watcher::Occurrence& o = kv.second;
        AutoFinding f;
        f.sev = AutoFinding::Warning; f.code = "AD-NACK";
        f.domain = o.domain; f.hasPc = o.domain >= 0; f.pc = o.pc; f.at = o.at; f.count = o.count;
        f.title = "I2C device " + std::to_string(kv.first) + " did not acknowledge";
        f.detail = "A " + o.text + " to the device with I2C address " + std::to_string(kv.first) + " (0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%llX", (unsigned long long)kv.first); return std::string(b); }() +
                   ") got a NACK" + (o.count > 1 ? " " + std::to_string(o.count) + " times." : ".");
        f.hint = "The environment file has no device at that address (or it is told to NACK). Check the address in the DeviceMap (.devmap) and the [device ...] sections.";
        add(f);
    }
    // ---- events that nobody handled
    for (const auto& kv : w.raised) {
        const auto it = w.dispatched.find(kv.first);
        const size_t done = it == w.dispatched.end() ? 0 : (size_t)it->second;
        if (kv.second.size() <= done) continue;
        AutoFinding f;
        f.sev = AutoFinding::Warning; f.code = "AD-EVENT";
        f.domain = kv.first.first; f.at = kv.second[done]; f.count = (int)(kv.second.size() - done);
        f.title = "Hardware event " + std::to_string(kv.first.second) + " was never dispatched";
        f.detail = "The event was raised at cycle " + std::to_string(f.at) + " for " + (f.domain >= 0 ? kDomain[f.domain] : "a sequencer") +
                   " but the sequencer never reached a BR with event_safe, so its vector never ran.";
        f.hint = "Put an event checkpoint (BR ... , safe) into the main loop, or check that the event id has a .vector for that sequencer.";
        add(f);
    }
    // ---- results that Main never took
    if (w.publishes > 0 && w.consumes == 0) {
        AutoFinding f;
        f.sev = AutoFinding::Warning; f.code = "AD-UNTAKEN";
        f.domain = 1; f.hasPc = true; f.pc = w.firstPublishPc; f.at = w.firstPublish; f.count = w.publishes;
        f.title = "Results are published but Main never takes one";
        f.detail = "Peri published " + std::to_string(w.publishes) + " result(s); Main consumed none.";
        f.hint = "Main has to read PERI_RESULT_SLOT after seeing PERI_RESULT_VALID (spec 12); is Main waiting for something else first?";
        add(f);
    }
    // ---- a WCET longer than the window it has to fit in
    {
        const Simulator::FrameTiming ft = sim.frameTiming();
        const int64_t wcet = sim.signal("wcet_pgl");
        if (ft.vblankLen > 0 && wcet > ft.vblankLen) {
            AutoFinding f;
            f.sev = AutoFinding::Error; f.code = "AD-WCET";
            f.title = "The commit never fits the blanking window";
            f.detail = "frame.wcet_pgl is " + std::to_string(wcet) + " cycles but the vertical blanking window is only " + std::to_string(ft.vblankLen) +
                       " cycles long: PGL_COMMIT_ELIGIBLE can never be true, so a Main that waits for it waits for ever.";
            f.hint = "Lower frame.wcet_pgl (the Frame Timing WCET register) or lengthen the window (timing.vblank_len); both are parameters of the Simulator page.";
            add(f);
        }
    }
    // ---- code the run never reached
    {
        std::vector<const InstrInfo*> unreached[2];
        for (const InstrInfo& i : program.instrs) {
            if (i.synthetic) continue;
            const int d = i.domain == Domain::Main ? 0 : 1;
            if (!w.issued.count({d, i.pc})) unreached[d].push_back(&i);
        }
        for (int d = 0; d < 2; d++) {
            std::sort(unreached[d].begin(), unreached[d].end(), [](const InstrInfo* a, const InstrInfo* b) { return a->pc < b->pc; });
            int groups = 0;
            for (size_t i = 0; i < unreached[d].size() && groups < 8;) {
                size_t j = i + 1;
                while (j < unreached[d].size() && unreached[d][j]->pc == unreached[d][j - 1]->pc + unreached[d][j - 1]->length) j++;
                AutoFinding f;
                f.sev = AutoFinding::Info; f.code = "AD-UNREACHED";
                f.domain = d; f.hasPc = true; f.pc = unreached[d][i]->pc; f.count = (int)(j - i);
                f.title = std::to_string(j - i) + " " + kDomain[d] + " instruction(s) never ran";
                f.detail = "From \"" + unreached[d][i]->text + "\" on, " + std::to_string(j - i) + " instruction(s) were not reached in " + std::to_string(rep.endAt) + " cycles.";
                f.hint = "A path the run did not take (an error branch, a service that never came). Fine if that is as intended; otherwise the condition in front of it never selected it.";
                add(f);
                groups++;
                i = j;
            }
        }
    }
    std::stable_sort(rep.findings.begin(), rep.findings.end(), [](const AutoFinding& a, const AutoFinding& b) {
        if (a.sev != b.sev) return a.sev < b.sev;
        return a.at < b.at;
    });
    return rep;
}

}  // namespace acide
