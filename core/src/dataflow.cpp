// Data-flow check (spec 3.3.9, 5.2, 6.8, 10.4, 12.8, 13.9): operands loaded, buffers written before they are read, ALU results not lost,
// the result slot consumed correctly, publish and consume lengths agreeing, every published service handled.
//
// Structure (so a rule can change without touching the others):
//   Cfg            control-flow graph over the lowered operations (labels are nodes that do nothing)
//   solve<S>       a generic forward "must" analysis: a fact holds at a point only if it holds on EVERY path that reaches it
//   MainFlow/PeriFlow  the abstract state and the transfer function of each domain; every finding goes through Reporter::report
//   Reporter       turns a finding into a warning or an error (Options::dataflow), drops duplicates, names the call chain
//
// The analysis runs on the program with CALLs expanded INLINE whatever call model was chosen, because a software return is a jump to
// every call site and that would invent paths. When the model is softret the clobbers it causes (ALU_OPA, ALU_OPB, the compare flag)
// are applied where each call returns.
#include <algorithm>
#include <deque>
#include <set>

#include "codec.h"
#include "internal.h"

namespace asmcore {
namespace {

using namespace isa;

// ---- reporting ----------------------------------------------------------------------------------------------------------------------
struct Reporter {
    Ctx& c;
    std::set<std::string> seen;
    explicit Reporter(Ctx& cc) : c(cc) {}
    // code: "501" -> W501 (or E501 in Error mode)
    void report(const Op& o, const char* code, const std::string& msg) { reportAt(o.loc, o.frames, code, msg); }
    void reportAt(const SrcLoc& loc, const std::vector<Frame>& frames, const char* code, std::string msg) {
        if (c.opt.dataflow == DataflowMode::Off) return;
        if (!frames.empty()) msg += " (inside procedure " + frames.back().proc + ", called from line " + std::to_string(frames.back().callSite.line) + ")";
        std::string key = std::string(code) + "|" + loc.file + ":" + std::to_string(loc.line) + "|" + msg;
        if (!seen.insert(key).second) return;
        if (c.opt.dataflow == DataflowMode::Error) c.error(loc, std::string("E") + code, msg);
        else c.warn(loc, std::string("W") + code, msg);
    }
};

std::string range(const char* region, int off, int len) {
    return std::string(region) + (len == 1 ? "[" + std::to_string(off) + "]" : "[" + std::to_string(off) + ".." + std::to_string(off + len - 1) + "]");
}

// ---- control-flow graph ---------------------------------------------------------------------------------------------------------------
struct Cfg {
    std::vector<std::vector<std::pair<int, int>>> succ;     // (node, 0 = fall through / 1 = taken)
};
Cfg buildCfg(const std::vector<Op>& ops) {
    Cfg g;
    g.succ.resize(ops.size());
    std::map<std::string, int> lab;
    for (size_t i = 0; i < ops.size(); i++) if (ops[i].kind == OpKind::Label) lab[ops[i].label] = (int)i;
    for (size_t i = 0; i < ops.size(); i++) {
        const Op& o = ops[i];
        bool branch = o.kind == OpKind::MBr || o.kind == OpKind::PBr || o.kind == OpKind::MCascB || o.kind == OpKind::MCascE;
        if (branch) {
            auto it = lab.find(o.label);
            if (it != lab.end()) g.succ[i].push_back({it->second, 1});
            if (o.kind != OpKind::MCascB && !o.uncond && i + 1 < ops.size()) g.succ[i].push_back({(int)i + 1, 0});
        } else if (i + 1 < ops.size()) {
            g.succ[i].push_back({(int)i + 1, 0});
        }
    }
    return g;
}

// Forward fixpoint. S needs: `bool reach`, and join(into, from) returning whether `into` changed.
template <class S, class Join, class Transfer, class Edge>
void solve(const std::vector<Op>& ops, const Cfg& g, std::vector<S>& in, Join join, Transfer transfer, Edge edge) {
    std::deque<int> work;
    std::vector<char> queued(ops.size(), 0);
    if (ops.empty()) return;
    work.push_back(0);
    queued[0] = 1;
    size_t guard = 0, limit = ops.size() * 4000 + 100000;
    while (!work.empty() && guard++ < limit) {
        int i = work.front();
        work.pop_front();
        queued[(size_t)i] = 0;
        S out = in[(size_t)i];
        transfer((size_t)i, out);
        for (auto& e : g.succ[(size_t)i]) {
            S s = out;
            edge((size_t)i, e.second, s);
            if (join(in[(size_t)e.first], s) && !queued[(size_t)e.first]) { work.push_back(e.first); queued[(size_t)e.first] = 1; }
        }
    }
}

// ---- facts that cross the domains ------------------------------------------------------------------------------------------------------
struct Facts {
    std::map<int, std::set<int>> pubLens;           // service_id -> lengths published
    std::map<int, const Op*> pubOp;                 // service_id -> a publishing instruction (for messages)
    std::set<int> cmpServices;                      // service ids Main compares PERI_RESULT_META with
    bool consumesSlot = false;
    const Op* firstConsume = nullptr;
};

// ---- Main -----------------------------------------------------------------------------------------------------------------------------------
struct Tag {                                         // where a byte came from, to recognise "service_id == constant" comparisons
    uint8_t kind = 0, v = 0;                         // 1: constant byte v;  2: byte v of PERI_RESULT_META;  3: byte v of a service payload
    bool w = false;                                  // kind 3: the service declares the high byte first (.service ..., big)
    bool operator==(const Tag& o) const { return kind == o.kind && v == o.v && w == o.w; }
};
struct MState {
    bool reach = false;
    bool scr[32], buf[32];
    Tag scrT[32], bufT[32];
    bool opa = false, opb = false;
    Tag aT[2], bT[2];
    bool out = false, status = false, outUnread = false, cmpDef = false;
    int outLine = 0;
    bool valid = false;                              // PERI_RESULT_VALID was tested true and the result has not been consumed
    int svc = -1;                                    // service whose handler this is (-1: unknown)
    bool cmpSvc = false;
    int cmpVal = 0;
    MState() { for (int i = 0; i < 32; i++) scr[i] = buf[i] = false; }
};
bool joinTag(Tag& a, const Tag& b) { if (a == b) return false; if (a.kind == 0) return false; a = Tag(); return true; }
bool joinMain(MState& into, const MState& from) {
    if (!from.reach) return false;
    if (!into.reach) { into = from; return true; }
    bool ch = false;
    auto andb = [&](bool& a, bool b) { if (a && !b) { a = false; ch = true; } };
    for (int i = 0; i < 32; i++) {
        andb(into.scr[i], from.scr[i]);
        andb(into.buf[i], from.buf[i]);
        ch |= joinTag(into.scrT[i], from.scrT[i]);
        ch |= joinTag(into.bufT[i], from.bufT[i]);
    }
    andb(into.opa, from.opa);
    andb(into.opb, from.opb);
    for (int i = 0; i < 2; i++) { ch |= joinTag(into.aT[i], from.aT[i]); ch |= joinTag(into.bT[i], from.bT[i]); }
    andb(into.out, from.out);
    andb(into.status, from.status);
    andb(into.outUnread, from.outUnread);
    andb(into.cmpDef, from.cmpDef);
    andb(into.valid, from.valid);
    if (into.outLine != from.outLine && into.outLine != 0) { into.outLine = 0; ch = true; }
    if (into.svc != from.svc && into.svc != -1) { into.svc = -1; ch = true; }
    if (into.cmpSvc && (!from.cmpSvc || from.cmpVal != into.cmpVal)) { into.cmpSvc = false; ch = true; }
    return ch;
}

struct MainFlow {
    Ctx& c;
    const std::vector<uint8_t>& pool;
    Facts& facts;
    Reporter& rep;
    const std::map<std::string, bool>& clobberAt;     // label -> true: a softret call returns here
    bool reporting = false;

    void say(const Op& o, const char* code, const std::string& msg) { if (reporting) rep.report(o, code, msg); }

    void transfer(const Op& o, MState& s) {
        if (!s.reach) return;
        switch (o.kind) {
            case OpKind::Label: {
                if (clobberAt.count(o.label)) {                     // a software RET leaves its own values in the ALU (spec: CALL/RET clobbers)
                    s.opa = s.opb = false;
                    s.cmpDef = false;
                    s.cmpSvc = false;
                }
                break;
            }
            case OpKind::Mov: mov(o, s); break;
            case OpKind::Alu: alu(o, s); break;
            case OpKind::Csr1: {
                codec::Csr1 x = codec::unpackCsr1(o.desc);
                if (x.op == CSR1_READ8 && !o.extra.empty()) { int p = o.extra[0] & 31; s.buf[p] = true; s.bufT[p] = Tag(); }
                break;
            }
            case OpKind::CsrM: {
                codec::CsrM x = codec::unpackCsrM(o.desc);
                if (x.op == CSRM_WRITE_SP) {                          // v31: the data comes from the scratchpad
                    int bad = 0;
                    for (int i = 0; i < x.len && x.ptr + i < 32; i++) if (!s.scr[x.ptr + i]) bad++;
                    if (bad) say(o, "502", range("MAIN_SCRATCHPAD", x.ptr, x.len) + " is written to a CSR here (WRITE_SP), but " + std::to_string(bad) + " of its bytes are not written on every path (MOV them into the scratchpad first, or declare them with .assume)");
                } else {                                               // READ_BUF fills MAIN_CSR_DATA_BUF
                    for (int i = 0; i < x.len && x.ptr + i < 32; i++) { s.buf[x.ptr + i] = true; s.bufT[x.ptr + i] = Tag(); }
                }
                break;
            }
            default: break;
        }
    }

    void mov(const Op& o, MState& s) {
        codec::Move m = codec::unpackMove(o.desc);
        Tag tags[32];
        const int len = m.len;
        auto readBytes = [&](const char* name, bool* def, Tag* t, int size, bool needInit) {
            int bad = 0;
            for (int i = 0; i < len; i++) {
                int idx = m.srcOff + i;
                if (idx >= size) continue;
                if (needInit && !def[idx]) bad++;
                tags[i] = t[idx];
            }
            if (bad) say(o, "502", range(name, m.srcOff, len) + " is read here, but " + std::to_string(bad) + " of its bytes are not written on every path (write it first, or declare it with .assume / a profile reset value)");
        };
        switch (m.srcRegion) {
            case R_SCRATCH: readBytes("MAIN_SCRATCHPAD", s.scr, s.scrT, 32, true); break;
            case R_CSRBUF: readBytes("MAIN_CSR_DATA_BUF", s.buf, s.bufT, 32, true); break;
            case R_SLOT: {
                if (reporting && !facts.consumesSlot) { facts.consumesSlot = true; facts.firstConsume = &o; }
                else if (reporting && !facts.firstConsume) facts.firstConsume = &o;
                if (!s.valid) say(o, "505", "PERI_RESULT_SLOT is consumed here, but not every path to this MOV has tested BR.COND PERI_RESULT_VALID since the last consume (the slot may be empty)");
                if (reporting && s.svc >= 0) {
                    auto it = facts.pubLens.find(s.svc);
                    if (it != facts.pubLens.end() && !it->second.count(len)) {
                        std::string have;
                        for (int l : it->second) have += (have.empty() ? "" : ", ") + std::to_string(l);
                        say(o, "507", "this handler is for service " + std::to_string(s.svc) + ", which Peri publishes with " + have + " byte(s), but it consumes " + std::to_string(len));
                    }
                }
                if (s.svc >= 0) {
                    auto sd = c.services.find(s.svc);
                    if (sd != c.services.end()) {
                        if (sd->second.len >= 0 && len != sd->second.len)
                            say(o, "517", "service " + sd->second.name + " is declared with len=" + std::to_string(sd->second.len) + " (line " + std::to_string(sd->second.loc.line) + "), but this handler consumes " + std::to_string(len) + " byte(s)");
                        if (sd->second.wire == 'B')                                         // remember: this payload is in bus order, high byte first
                            for (int i = 0; i < len; i++) { tags[i].kind = 3; tags[i].v = (uint8_t)(m.srcOff + i); tags[i].w = true; }
                    }
                }
                s.valid = false;
                break;
            }
            case R_META: for (int i = 0; i < len; i++) { tags[i].kind = 2; tags[i].v = (uint8_t)(m.srcOff + i); } break;
            case R_OUT:
                if (!s.out) say(o, "503", "ALU_OUT is read here, but no ALU operation that produces a result (not CMP) has run on every path");
                s.outUnread = false;
                break;
            case R_STATUS:
                if (!s.status) say(o, "503", "ALU_STATUS is read here, but no ALU operation has run on every path");
                break;
            case R_CONST:
                for (int i = 0; i < len; i++) {
                    size_t idx = (size_t)(m.srcOff + i);
                    if (idx < pool.size()) { tags[i].kind = 1; tags[i].v = pool[idx]; }
                }
                break;
            default: break;
        }
        switch (m.dstRegion) {
            case R_SCRATCH: for (int i = 0; i < len && m.dstOff + i < 32; i++) { s.scr[m.dstOff + i] = true; s.scrT[m.dstOff + i] = tags[i]; } break;
            case R_CSRBUF: for (int i = 0; i < len && m.dstOff + i < 32; i++) { s.buf[m.dstOff + i] = true; s.bufT[m.dstOff + i] = tags[i]; } break;
            case R_OPA: case R_OPB: {
                if (len == 2 && tags[0].kind == 3 && tags[0].w && tags[1].kind == 3 && tags[1].w && tags[1].v == tags[0].v + 1 && tags[0].v % 2 == 0)
                    say(o, "514", std::string(m.dstRegion == R_OPA ? "ALU_OPA" : "ALU_OPB") + " is loaded with bytes " + std::to_string(tags[0].v) + ".." + std::to_string(tags[1].v) +
                                      " of a service payload that the contract declares high byte first (big-endian bus order), but ALU operands are little-endian (byte 0 = low byte): the ALU would see the value byte-swapped. "
                                      "Swap the two bytes first: MOV the low byte to scratch[T], the high byte to scratch[T+1] (two 1-byte MOVs, non-overlapping), then MOV scratch[T..T+1] -> " + (m.dstRegion == R_OPA ? "ALU_OPA" : "ALU_OPB") + " (spec v31 handoff 5.2)");
                if (m.dstRegion == R_OPA) { s.opa = true; s.aT[0] = tags[0]; s.aT[1] = tags[1]; }
                else { s.opb = true; s.bT[0] = tags[0]; s.bT[1] = tags[1]; }
                break;
            }
            default: break;
        }
    }

    void alu(const Op& o, MState& s) {
        codec::Alu a = codec::unpackAlu(o.desc, c.opt.profile.alu);
        bool useA = true, useB = true;
        if (a.cls == A_ARITH) useB = a.op == ARITH_ADD || a.op == ARITH_SUB || a.op == ARITH_MUL;
        else if (a.cls == A_LOGIC) useB = a.op != LOGIC_NOT;
        else if (a.cls == A_SHIFT) useB = false;
        if (useA && !s.opa) say(o, "501", "ALU_OPA is not loaded on every path to this ALU operation (MOV a 2-byte value into it first; a software CALL/RET leaves ALU_OPA and ALU_OPB clobbered)");
        if (useB && !s.opb) say(o, "501", "ALU_OPB is not loaded on every path to this ALU operation (MOV a 2-byte value into it first; a software CALL/RET leaves ALU_OPA and ALU_OPB clobbered)");
        s.status = true;
        if (a.cls == A_CMP) {
            s.cmpDef = true;
            s.cmpSvc = false;
            if (a.op == CMP_EQ && s.aT[0].kind == 2 && s.aT[0].v == 0 && s.aT[1].kind == 2 && s.aT[1].v == 1 && s.bT[0].kind == 1 && s.bT[1].kind == 1) {
                s.cmpSvc = true;
                s.cmpVal = s.bT[0].v | (s.bT[1].v << 8);
                if (reporting) facts.cmpServices.insert(s.cmpVal);
            }
            return;
        }
        if (s.out && s.outUnread)
            say(o, "504", "the result of the ALU operation at line " + (s.outLine ? std::to_string(s.outLine) : std::string("?")) + " is overwritten here without being moved out of ALU_OUT");
        s.out = true;
        s.outUnread = true;
        s.outLine = o.loc.line;
    }

    void edge(const Op& o, int kind, MState& s) {
        if (!s.reach || o.kind != OpKind::MBr || o.brType != MB_COND) return;
        int cond = o.brArg;
        if (cond == 1) s.valid = kind == 1;                   // PERI_RESULT_VALID: known true on the taken edge, false otherwise
        if (cond == 5) {                                       // ALU_CMP_TRUE
            s.svc = (kind == 1 && s.cmpSvc) ? s.cmpVal : -1;
            if (reporting && !s.cmpDef && kind == 0) say(o, "506", "BR.COND ALU_CMP_TRUE reads the compare flag, but no ALU CMP has run on every path to it (a software CALL/RET overwrites the flag)");
        }
        if (cond == 6 && reporting && !s.status && kind == 0) say(o, "506", "BR.COND ALU_OUT_OV16 reads an ALU flag, but no ALU operation has run on every path to it");
    }
};

// ---- Peri -------------------------------------------------------------------------------------------------------------------------------------
struct PState {
    bool reach = false;
    bool timer = false;                              // a timer_start CSR bit was written on every path (and not cleared since)
    bool armed = false, evald = false;               // poll_arm written / a successful READ into PERI_RX_BUF[0] followed it
    bool rx[32];
    PState() { for (int i = 0; i < 32; i++) rx[i] = false; }
};
bool joinPeri(PState& into, const PState& from) {
    if (!from.reach) return false;
    if (!into.reach) { into = from; return true; }
    bool ch = false;
    for (int i = 0; i < 32; i++) if (into.rx[i] && !from.rx[i]) { into.rx[i] = false; ch = true; }
    if (into.timer && !from.timer) { into.timer = false; ch = true; }
    if (into.armed && !from.armed) { into.armed = false; ch = true; }
    if (into.evald && !from.evald) { into.evald = false; ch = true; }
    return ch;
}

}  // namespace

// ---- driver -----------------------------------------------------------------------------------------------------------------------------------
void runDataflow(Ctx& c, const std::vector<Op>* mainOps, const std::vector<Op>* periOps) {
    if (c.opt.dataflow == DataflowMode::Off) return;
    Reporter rep(c);
    Facts facts;
    const Profile& pf = c.opt.profile;

    // ---- Peri first: it tells Main which services exist and how long they are
    if (periOps && !periOps->empty()) {
        const std::vector<Op>& ops = *periOps;
        Cfg g = buildCfg(ops);
        std::vector<PState> in(ops.size());
        in[0].reach = true;
        for (int i = 0; i < 32; i++) in[0].rx[i] = pf.periRxResetDefined;
        for (const Ctx::Assume& a : c.assumes) if (a.domain == 1) for (int i = 0; i < a.len; i++) in[0].rx[a.off + i] = true;
        bool reporting = false;
        bool hasTimerRole = false, hasPollRole = false;
        for (const Ctx::CsrBit& b : c.csrBits) { if (b.role == "timer_start") hasTimerRole = true; if (b.role == "poll_arm") hasPollRole = true; }
        auto xfer = [&](const Op& o, PState& s) {
            if (!s.reach) return;
            if (o.kind == OpKind::Csr1 && !o.extra.empty()) {
                codec::Csr1 x = codec::unpackCsr1(o.desc);
                if (x.op == CSR1_WRITE8 || x.op == CSR1_SET8)                       // a 1 written to the bit is the trigger (write-1 / set)
                    for (const Ctx::CsrBit& b : c.csrBits) {
                        if (b.addr != x.addr || !((o.extra[0] >> b.bit) & 1)) continue;
                        if (b.role == "timer_start") s.timer = true;
                        else if (b.role == "timer_clear") s.timer = false;
                        else if (b.role == "poll_arm") { s.armed = true; s.evald = false; }
                        else if (b.role == "poll_clear") { s.armed = false; s.evald = false; }
                    }
            } else if (o.kind == OpKind::PBr && reporting) {
                if ((o.brType == 6 || o.brType == 7) && hasTimerRole && !s.timer)
                    rep.report(o, "515", "the timer flag is tested here, but not every path to this branch has started the timer (.csrbit ... = timer_start): timer_done = 0 does not mean the timer runs, so this can wait forever. Start it once before the wait loop, not inside it");
                if ((o.brType == 1 || o.brType == 2) && hasPollRole && !s.evald)
                    rep.report(o, "516", "the poll result is tested here, but not every path to it arms the poll evaluator (.csrbit ... = poll_arm) and then reads PERI_RX_BUF[0] with a successful EXEC_XFER READ: nothing evaluates the status, so ready / not_ready never change");
            }
            if (o.kind == OpKind::Xfer1) {
                codec::Xfer1 x = codec::unpackXfer1(o.desc);
                if (x.rw && !o.extra.empty()) { s.rx[o.extra[0] & 31] = true; if (s.armed && (o.extra[0] & 31) == 0) { s.evald = true; s.armed = false; } }
            } else if (o.kind == OpKind::XferM) {
                codec::XferM x = codec::unpackXferM(o.desc);
                for (int i = 0; i < x.len && x.ptr + i < 32; i++) s.rx[x.ptr + i] = true;
                if (x.rw && s.armed && x.ptr == 0) { s.evald = true; s.armed = false; }
            } else if (o.kind == OpKind::Pub) {
                codec::Pub p = codec::unpackPub(o.desc);
                if (reporting) {
                    auto sd = c.services.find(p.service);
                    if (sd != c.services.end() && sd->second.len >= 0 && sd->second.len != p.len)
                        rep.report(o, "517", "service " + sd->second.name + " is declared with len=" + std::to_string(sd->second.len) + " (line " + std::to_string(sd->second.loc.line) + "), but EXEC_PUB publishes " + std::to_string(p.len) + " byte(s)");
                    int bad = 0;
                    for (int i = 0; i < p.len && p.src + i < 32; i++) if (!s.rx[p.src + i]) bad++;
                    if (bad) rep.report(o, "508", "EXEC_PUB publishes " + range("PERI_RX_BUF", p.src, p.len) + ", but " + std::to_string(bad) + " of its bytes are not read in by an EXEC_XFER on every path");
                    facts.pubLens[p.service].insert(p.len);
                    if (!facts.pubOp.count(p.service)) facts.pubOp[p.service] = &o;
                }
            }
        };
        auto noEdge = [](size_t, int, PState&) {};
        solve<PState>(ops, g, in, joinPeri, [&](size_t i, PState& s) { xfer(ops[i], s); }, noEdge);
        reporting = true;
        for (size_t i = 0; i < ops.size(); i++) { PState s = in[i]; xfer(ops[i], s); }
    }

    // ---- Main
    if (mainOps && !mainOps->empty()) {
        std::vector<Op> inlineOps;
        std::map<std::string, bool> clobberAt;
        const std::vector<Op>* ops = mainOps;
        std::vector<uint8_t> pool = c.constPool;
        if (c.opt.callModel == CallModel::SoftRet && !c.pendingProcs.empty()) {
            // the same program with every CALL expanded: no return dispatch, hence no invented paths
            Ctx tmp = c;
            tmp.opt.callModel = CallModel::Inline;
            tmp.res = Result();
            tmp.pendingCalls.clear();
            tmp.pendingProcs.clear();
            std::vector<ProcInfo> procsTmp;
            if (lowerDomain(tmp, Domain::Main, inlineOps, procsTmp)) {
                ops = &inlineOps;
                pool = tmp.constPool;
                std::map<std::string, int> sites;
                for (const Ctx::PendingProc& p : c.pendingProcs) if (p.info.domain == Domain::Main) sites[p.info.name] = p.info.callSites;
                for (const Ctx::PendingCall& pc : tmp.pendingCalls)
                    if (pc.info.domain == Domain::Main && sites.count(pc.info.callee) && sites[pc.info.callee] >= 2) clobberAt[pc.endLabel] = true;
            }
        }
        Cfg g = buildCfg(*ops);
        std::vector<MState> in(ops->size());
        in[0].reach = true;
        for (int i = 0; i < 32; i++) { in[0].scr[i] = pf.scratchpadResetDefined; in[0].buf[i] = pf.csrBufResetDefined; }
        for (const Ctx::Assume& a : c.assumes) if (a.domain == 0)
            for (int i = 0; i < a.len; i++) (a.region == "scratch" ? in[0].scr : in[0].buf)[a.off + i] = true;
        MainFlow fl{c, pool, facts, rep, clobberAt};
        solve<MState>(*ops, g, in, joinMain, [&](size_t i, MState& s) { fl.transfer((*ops)[i], s); },
                      [&](size_t i, int kind, MState& s) { fl.edge((*ops)[i], kind, s); });
        fl.reporting = true;
        for (size_t i = 0; i < ops->size(); i++) {
            if (!in[i].reach) continue;
            MState s = in[i];
            fl.transfer((*ops)[i], s);
            for (auto& e : g.succ[i]) { MState t = s; fl.edge((*ops)[i], e.second, t); }
        }
    }

    // ---- service contracts across the two domains (spec 12.8, 13.9)
    for (auto& kv : facts.pubLens) {
        const Op* op = facts.pubOp[kv.first];
        if (kv.second.size() > 1) {
            std::string have;
            for (int l : kv.second) have += (have.empty() ? "" : ", ") + std::to_string(l);
            rep.report(*op, "511", "service " + std::to_string(kv.first) + " is published with different lengths (" + have + "): Main consumes it with one fixed length");
        }
    }
    if (facts.consumesSlot && (!facts.cmpServices.empty() || facts.pubLens.size() >= 2)) {
        for (auto& kv : facts.pubLens) {
            if (facts.cmpServices.count(kv.first)) continue;
            rep.report(*facts.pubOp[kv.first], "510", "service " + std::to_string(kv.first) + " is published here, but Main never compares PERI_RESULT_META's service_id with " + std::to_string(kv.first) +
                                                         " (add a handler, or an explicit unknown_service path, spec 13.9)");
        }
    }
}

}  // namespace asmcore
