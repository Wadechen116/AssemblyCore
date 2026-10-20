// Hardware events (spec v31 16): vector / ERET validation, handler-vs-foreground state conflicts, and the static event latency.
//
// The assembler cannot prove that a handler is harmless at a given `safe` branch, so it never decides that for you: `safe` is always
// written by the programmer, and where a handler may overwrite state that the code after the branch still reads, W512 says so.
#include <algorithm>
#include <bitset>
#include <functional>
#include <sstream>

#include "codec.h"
#include "internal.h"

namespace asmcore {
namespace {

using namespace isa;

// ---- control-flow graph over the real (non-label) operations -----------------------------------------------------------------------
bool isEret(const Op& o) { return (o.kind == OpKind::MBr && o.brType == MB_ERET) || (o.kind == OpKind::PBr && o.brType == PB_ERET); }
bool isOrdinaryBr(const Op& o) { return (o.kind == OpKind::MBr && o.brType == MB_COND) || (o.kind == OpKind::PBr && o.brType != PB_ERET); }

struct Graph {
    std::vector<const Op*> real;
    std::map<uint32_t, size_t> at;                       // pc -> index
    std::vector<std::vector<size_t>> succ, pred;

    Graph(const std::vector<Op>& ops, const std::map<const Op*, uint32_t>& target) {
        for (const Op& o : ops) if (o.kind != OpKind::Label) real.push_back(&o);
        for (size_t i = 0; i < real.size(); i++) at[real[i]->pc] = i;
        succ.assign(real.size(), {});
        pred.assign(real.size(), {});
        for (size_t i = 0; i < real.size(); i++) {
            const Op* o = real[i];
            const bool branch = o->kind == OpKind::MBr || o->kind == OpKind::MCascB || o->kind == OpKind::MCascE || o->kind == OpKind::PBr;
            if (isEret(*o)) continue;                                 // returns to the saved resume PC: no static successor
            if (branch) {
                auto t = target.find(o);
                if (t != target.end() && at.count(t->second)) succ[i].push_back(at[t->second]);
                if (o->kind == OpKind::MCascB) continue;
                if (!o->uncond && i + 1 < real.size()) succ[i].push_back(i + 1);
            } else if (i + 1 < real.size()) succ[i].push_back(i + 1);
        }
        for (size_t i = 0; i < real.size(); i++) for (size_t s : succ[i]) pred[s].push_back(i);
    }
    std::vector<char> reach(size_t from, bool stopAtEret = false) const {
        std::vector<char> seen(real.size(), 0);
        std::vector<size_t> st{from};
        seen[from] = 1;
        while (!st.empty()) {
            size_t i = st.back();
            st.pop_back();
            for (size_t s : succ[i]) if (!seen[s]) { seen[s] = 1; st.push_back(s); }
        }
        (void)stopAtEret;
        return seen;
    }
};

// ---- state a single operation reads / writes ----------------------------------------------------------------------------------------
// bit layout of the tracked state
constexpr int B_OPA = 0, B_OPB = 1, B_OUT = 2, B_STATUS = 3, B_SCR = 4, B_BUF = 36, B_RX = 68, B_PST = 100, B_COUNT = 101;
using Bits = std::bitset<B_COUNT>;

struct Effect {
    Bits read, write, kill;        // write: may be written; kill: definitely written (ends the liveness of the old value)
};

void range(Bits& b, int base, int off, int len) {
    for (int i = 0; i < len && off + i < 32 && off + i >= 0; i++) b.set((size_t)(base + off + i));
}

Effect effectOf(const Op& o, const Profile& pf) {
    Effect e;
    switch (o.kind) {
        case OpKind::Mov: {
            codec::Move m = codec::unpackMove(o.desc);
            switch (m.srcRegion) {
                case R_SCRATCH: range(e.read, B_SCR, m.srcOff, m.len); break;
                case R_CSRBUF: range(e.read, B_BUF, m.srcOff, m.len); break;
                case R_OUT: e.read.set(B_OUT); break;
                case R_STATUS: e.read.set(B_STATUS); break;
                default: break;
            }
            switch (m.dstRegion) {
                case R_SCRATCH: range(e.write, B_SCR, m.dstOff, m.len); break;
                case R_CSRBUF: range(e.write, B_BUF, m.dstOff, m.len); break;
                case R_OPA: e.write.set(B_OPA); break;
                case R_OPB: e.write.set(B_OPB); break;
                default: break;
            }
            e.kill = e.write;
            break;
        }
        case OpKind::Alu: {
            codec::Alu a = codec::unpackAlu(o.desc, pf.alu);
            e.read.set(B_OPA);
            e.read.set(B_OPB);
            e.write.set(B_STATUS);
            if (a.cls != A_CMP) e.write.set(B_OUT);
            e.kill = e.write;
            break;
        }
        case OpKind::Csr1:
            if (codec::unpackCsr1(o.desc).op == CSR1_READ8 && !o.extra.empty()) { range(e.write, B_BUF, o.extra[0] & 31, 1); e.kill = e.write; }
            break;
        case OpKind::CsrM: {
            codec::CsrM x = codec::unpackCsrM(o.desc);
            if (x.op == CSRM_WRITE_SP) range(e.read, B_SCR, x.ptr, x.len);
            else if (x.op == CSRM_READ_BUF) { range(e.write, B_BUF, x.ptr, x.len); e.kill = e.write; }
            break;
        }
        case OpKind::MBr:
            if (o.brType == MB_COND && (o.brArg == 5 || o.brArg == 6)) e.read.set(B_STATUS);      // ALU_CMP_TRUE, ALU_OUT_OV16
            break;
        case OpKind::PBr:
            if (o.brType != PB_ERET && o.brType != 0) e.read.set(B_PST);                         // everything but ALWAYS looks at ready / error / timer state
            break;
        case OpKind::Pub: {
            codec::Pub p = codec::unpackPub(o.desc);
            range(e.read, B_RX, p.src, p.len);
            break;
        }
        case OpKind::Xfer1:
            if (codec::unpackXfer1(o.desc).rw && !o.extra.empty()) { range(e.write, B_RX, o.extra[0] & 31, 1); e.kill = e.write; }
            e.write.set(B_PST);
            break;
        case OpKind::XferM: {
            codec::XferM x = codec::unpackXferM(o.desc);
            if (x.rw) { range(e.write, B_RX, x.ptr, x.len); e.kill = e.write; }
            e.write.set(B_PST);
            break;
        }
        default: break;
    }
    return e;
}

std::string describe(const Bits& b, Domain d) {
    std::vector<std::string> parts;
    if (b.test(B_OPA)) parts.push_back("ALU_OPA");
    if (b.test(B_OPB)) parts.push_back("ALU_OPB");
    if (b.test(B_OUT)) parts.push_back("ALU_OUT");
    if (b.test(B_STATUS)) parts.push_back("the ALU flags (ALU_STATUS, the compare result)");
    auto ranges = [&](int base, const char* name) {
        for (int i = 0; i < 32;) {
            if (!b.test((size_t)(base + i))) { i++; continue; }
            int j = i;
            while (j + 1 < 32 && b.test((size_t)(base + j + 1))) j++;
            parts.push_back(std::string(name) + "[" + std::to_string(i) + (j > i ? ".." + std::to_string(j) : std::string()) + "]");
            i = j + 1;
        }
    };
    if (d == Domain::Main) { ranges(B_SCR, "MAIN_SCRATCHPAD"); ranges(B_BUF, "MAIN_CSR_DATA_BUF"); }
    else { ranges(B_RX, "PERI_RX_BUF"); if (b.test(B_PST)) parts.push_back("the Peri ready / error latches"); }
    std::string s;
    for (size_t i = 0; i < parts.size(); i++) s += (i ? ", " : "") + parts[i];
    return s;
}

}  // namespace

void analyzeEvents(Ctx& c, Domain d, const std::vector<Op>& ops, const std::map<const Op*, uint32_t>& target) {
    const int di = domIdx(d);
    const Profile& pf = c.opt.profile;
    Graph g(ops, target);
    if (g.real.empty()) return;
    const auto& lab = c.labelPc[di];
    const char* dn = domainName(d);

    // ---- vectors ------------------------------------------------------------------------------------------------------------------
    struct V { int id; std::string label; SrcLoc loc; size_t idx; };
    std::vector<V> vecs;
    for (const Ctx::VecDecl& v : c.vectors[di]) {
        auto it = lab.find(v.label);
        if (it == lab.end()) { c.error(v.loc, "E315", "event " + std::to_string(v.id) + ": the vector label '" + v.label + "' is not defined in the " + dn + " code (a handler label must be in this domain's own flow, not inside a .proc)"); continue; }
        auto ai = g.at.find(it->second);
        if (ai == g.at.end()) { c.error(v.loc, "E315", "event " + std::to_string(v.id) + ": '" + v.label + "' is at the end of the code; a vector must point at an instruction (opcode byte)"); continue; }
        bool inCascade = false;
        for (auto& r : c.cascBody[di]) if (it->second >= r.first && it->second < r.second) inCascade = true;
        if (inCascade) { c.error(v.loc, "E315", "event " + std::to_string(v.id) + ": '" + v.label + "' is inside a cascade region; a vector must not point into a cascade body (spec v31 16.7)"); continue; }
        vecs.push_back({v.id, v.label, v.loc, ai->second});
        VectorInfo vi;
        vi.domain = d;
        vi.id = v.id;
        vi.label = v.label;
        vi.pc = it->second;
        vi.loc = v.loc;
        c.res.vectors.push_back(vi);
    }

    // ---- BR.ERET only inside handlers ----------------------------------------------------------------------------------------------
    const std::vector<char> normal = g.reach(0);
    for (size_t i = 0; i < g.real.size(); i++)
        if (normal[i] && isEret(*g.real[i]))
            c.error(g.real[i]->loc, "E315", std::string("BR.ERET is reachable from the start of the ") + dn + " program without an event: it only has a meaning inside an event handler (handler_active). Put the handler after the main loop's unconditional JMP and enter it only through '.vector'");

    std::vector<std::vector<char>> hreach;
    for (const V& v : vecs) {
        std::vector<char> h = g.reach(v.idx);
        hreach.push_back(h);
        bool anyEret = false;
        for (size_t i = 0; i < h.size(); i++) if (h[i] && isEret(*g.real[i])) anyEret = true;
        if (!anyEret) { c.error(v.loc, "E315", "the handler of event " + std::to_string(v.id) + " ('" + v.label + "') never reaches a BR.ERET: every normal exit of a handler must be BR.ERET (there is no other way back to the interrupted code)"); continue; }
        // every node of the handler must still be able to reach a BR.ERET
        std::vector<char> good(h.size(), 0);
        std::vector<size_t> st;
        for (size_t i = 0; i < h.size(); i++) if (h[i] && isEret(*g.real[i])) { good[i] = 1; st.push_back(i); }
        while (!st.empty()) {
            size_t i = st.back();
            st.pop_back();
            for (size_t p : g.pred[i]) if (h[p] && !good[p]) { good[p] = 1; st.push_back(p); }
        }
        for (size_t i = 0; i < h.size(); i++)
            if (h[i] && !good[i] && !g.real[i]->synthetic) {
                c.error(g.real[i]->loc, "E315", "from here the handler of event " + std::to_string(v.id) + " ('" + v.label + "') can never reach a BR.ERET (a loop or a jump out of the handler): the interrupted code would never resume");
                break;
            }
    }

    if (vecs.empty()) return;

    // ---- event timing (spec v31 16.9) -----------------------------------------------------------------------------------------------
    EventTiming& et = c.res.eventTiming[di];
    et.present = true;
    et.dispatchCycles = pf.latency.eventDispatch;
    et.eretCycles = pf.latency.eret;
    const size_t n = g.real.size();
    std::vector<char> safe(n, 0);
    for (size_t i = 0; i < n; i++) if (isOrdinaryBr(*g.real[i]) && g.real[i]->eventSafe) { safe[i] = 1; et.checkpoints++; }

    // longest path with cycle detection; `stop(i)` ends a path at node i (cost included)
    auto longest = [&](size_t from, const std::function<bool(size_t)>& stop, const std::function<bool(size_t)>& allowed, bool& bounded, std::string& why) {
        std::vector<int> color(n, 0);        // 0 new, 1 on stack, 2 done
        std::vector<int> memo(n, 0);
        std::function<int(size_t)> go = [&](size_t i) -> int {
            if (color[i] == 2) return memo[i];
            if (color[i] == 1) { bounded = false; if (why.empty()) why = "a loop without a checkpoint at line " + std::to_string(g.real[i]->loc.line); return 0; }
            color[i] = 1;
            int best = 0;
            const int cost = g.real[i]->cycles;
            if (!stop(i)) {
                bool any = false;
                for (size_t s : g.succ[i]) { if (!allowed(s)) continue; any = true; best = std::max(best, go(s)); }
                if (!any) { bounded = false; if (why.empty()) why = "the path from line " + std::to_string(g.real[i]->loc.line) + " ends without reaching it"; }
            }
            color[i] = 2;
            return memo[i] = best + cost;
        };
        return go(from);
    };
    {
        bool bounded = et.checkpoints > 0;
        std::string why = et.checkpoints > 0 ? "" : "the " + std::string(dn) + " program has no event_safe branch, so a pending event is never dispatched";
        int worst = 0;
        if (bounded)
            for (size_t i = 0; i < n; i++) {
                if (!normal[i]) continue;
                worst = std::max(worst, longest(i, [&](size_t k) { return safe[k] != 0; }, [&](size_t) { return true; }, bounded, why));
                if (!bounded) break;
            }
        et.checkpointBounded = bounded;
        et.checkpointCycles = bounded ? worst : 0;
        if (!bounded) {
            et.note = "no upper bound on the wait for the next event_safe branch: " + why;
            c.warn(vecs[0].loc, "W513", std::string("event response time of the ") + dn + " domain is unbounded: " + why + ". Add 'safe' to an ordinary BR.COND on every loop of the foreground flow if events must be answered in bounded time");
        }
    }
    bool allBounded = et.checkpointBounded;
    int worstHandler = 0;
    for (size_t k = 0; k < vecs.size(); k++) {
        EventTiming::Handler h;
        h.id = vecs[k].id;
        h.label = vecs[k].label;
        bool b = true;
        std::string why;
        const std::vector<char>& hr = hreach[k];
        int cyc = longest(vecs[k].idx, [&](size_t i) { return isEret(*g.real[i]); }, [&](size_t i) { return hr[i] != 0; }, b, why);
        h.bounded = b;
        h.cycles = b ? cyc : 0;
        if (!b) { allBounded = false; if (et.note.empty()) et.note = "handler of event " + std::to_string(h.id) + " is unbounded: " + why; }
        else worstHandler = std::max(worstHandler, cyc);
        et.handlers.push_back(h);
    }
    et.bounded = allBounded;
    et.worstCycles = allBounded ? et.checkpointCycles + et.dispatchCycles + worstHandler + et.eretCycles : 0;
    if (et.note.empty()) et.note = "engine-only planning latencies (spec v31 16.9): not an event response time signoff; detection-to-pending and SRAM / frontend contention are not included";

    // ---- W512: state the handler may overwrite that the code after a `safe` branch still reads --------------------------------------
    std::vector<Effect> eff(n);
    for (size_t i = 0; i < n; i++) eff[i] = effectOf(*g.real[i], pf);
    // may-write set of each handler
    Bits handlerWrites;
    std::vector<Bits> perHandler(vecs.size());
    for (size_t k = 0; k < vecs.size(); k++)
        for (size_t i = 0; i < n; i++) if (hreach[k][i]) perHandler[k] |= eff[i].write;
    // liveness before every operation (backward, may-analysis)
    std::vector<Bits> in(n), out(n);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t ii = n; ii-- > 0;) {
            Bits o;
            for (size_t s : g.succ[ii]) o |= in[s];
            Bits i2 = eff[ii].read | (o & ~eff[ii].kill);
            if (o != out[ii] || i2 != in[ii]) { out[ii] = o; in[ii] = i2; changed = true; }
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (!safe[i] || !normal[i]) continue;
        for (size_t k = 0; k < vecs.size(); k++) {
            Bits hit = out[i] & perHandler[k];
            if (hit.none()) continue;
            c.warn(g.real[i]->loc, "W512", "event_safe branch: if event " + std::to_string(vecs[k].id) + " is dispatched here, its handler '" + vecs[k].label + "' (line " + std::to_string(vecs[k].loc.line) + ") may overwrite " + describe(hit, d) +
                                           ", which the code after this branch still reads. Remove 'safe' from this branch, let the handler use other bytes, or make sure it saves and restores them (the assembler cannot prove that; hardware keeps no context)");
        }
    }
}

}  // namespace asmcore
