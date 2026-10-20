// Layout, descriptor tables, validation, image construction, WCET queries and the assemble() driver.
#include <algorithm>
#include <functional>
#include <set>

#include "codec.h"
#include "internal.h"

namespace asmcore {

int Op::length() const {
    switch (kind) {
        case OpKind::Label: return 0;
        case OpKind::Csr1: case OpKind::CsrM: case OpKind::Xfer1: return 2;
        case OpKind::XferM: return 1 + (int)extra.size();
        default: return 1;
    }
}

namespace {

using namespace isa;

int opCycles(const Op& o, const Profile& pf) {
    const Latency& L = pf.latency;
    switch (o.kind) {
        case OpKind::Mov: { codec::Move m = codec::unpackMove(o.desc); return L.mov(m.len, m.srcRegion == R_SLOT); }
        case OpKind::Alu: { codec::Alu a = codec::unpackAlu(o.desc, pf.alu); return L.aluOp(a.cls == A_ARITH && a.op == ARITH_MUL); }
        case OpKind::Csr1: return L.csrOne(codec::unpackCsr1(o.desc).op);
        case OpKind::CsrM: return L.csrMulti(codec::unpackCsrM(o.desc).len);
        case OpKind::MCascB: case OpKind::MCascE: return L.cascade;
        case OpKind::Pub: return L.pub(codec::unpackPub(o.desc).len);
        case OpKind::Xfer1: { codec::Xfer1 x = codec::unpackXfer1(o.desc); return (int)L.xfer(x.rw != 0, x.addrLen, 1); }
        case OpKind::XferM: { codec::XferM x = codec::unpackXferM(o.desc); return (int)L.xfer(true, x.addrLen, x.len); }
        case OpKind::MBr: case OpKind::PBr: return L.brCond;
        default: return 0;
    }
}

// Logical bytes -> 32-bit SRAM words: byte 4n+k is bits [8k+7:8k] of word n (spec v31 3.1); the last word is padded.
std::vector<uint32_t> packWords(const std::vector<uint8_t>& bytes, uint8_t pad) {
    std::vector<uint32_t> w;
    for (size_t i = 0; i < bytes.size(); i += 4) {
        uint32_t v = 0;
        for (size_t k = 0; k < 4; k++) v |= (uint32_t)(i + k < bytes.size() ? bytes[i + k] : pad) << (8 * k);
        w.push_back(v);
    }
    return w;
}

void put(std::vector<uint8_t>& b, uint32_t v, int bytes, bool big) {
    for (int i = 0; i < bytes; i++) {
        int sh = big ? 8 * (bytes - 1 - i) : 8 * i;
        b.push_back((uint8_t)((v >> sh) & 0xFF));
    }
}

struct Layout {
    Ctx& c;
    Domain d;
    std::vector<Op>& ops;
    std::map<std::string, uint32_t>& lab;
    uint32_t codeSize = 0;
    bool ok = true;

    Layout(Ctx& cc, Domain dd, std::vector<Op>& o) : c(cc), d(dd), ops(o), lab(cc.labelPc[domIdx(dd)]) {}

    void err(const SrcLoc& l, const char* code, const std::string& m) { c.error(l, code, m); ok = false; }
    bool isMain() const { return d == Domain::Main; }

    void assign() {
        uint32_t pc = 0;
        for (Op& o : ops) {
            o.pc = pc;
            if (o.kind == OpKind::Label) {
                if (lab.count(o.label)) err(o.loc, "E104", "label '" + o.label + "' is defined twice");
                lab[o.label] = pc;
            } else {
                o.cycles = opCycles(o, c.opt.profile);
                pc += (uint32_t)o.length();
            }
        }
        codeSize = pc;
    }

    static bool isEret(const Op& o) { return (o.kind == OpKind::MBr && o.brType == MB_ERET) || (o.kind == OpKind::PBr && o.brType == PB_ERET); }

    bool resolveBranches(std::map<const Op*, uint32_t>& target) {
        for (Op& o : ops) {
            if (o.kind != OpKind::MBr && o.kind != OpKind::MCascB && o.kind != OpKind::MCascE && o.kind != OpKind::PBr) continue;
            if (isEret(o)) continue;                                   // BR.ERET: no target (it returns to the saved resume PC)
            auto it = lab.find(o.label);
            if (it == lab.end()) { err(o.loc, "E301", "undefined label '" + o.label + "'"); continue; }
            uint32_t t = it->second;
            if (t >= codeSize) { err(o.loc, "E301", "label '" + o.label + "' is at the end of the code: a branch target must be an opcode byte"); continue; }
            if (t > 255) { err(o.loc, "E310", "branch target '" + o.label + "' is at byte " + std::to_string(t) + ", but target_pc_byte has 8 bits (spec v31 7.2, 11.2): a branch can only reach bytes 0..255. The hardware PC may be wider than that, but this branch cannot be encoded; move the label (and the code it needs to reach) into the first 256 bytes, or restructure the code. Nothing is truncated or rewritten for you"); continue; }
            target[&o] = t;
        }
        return ok;
    }

    void validateFlow() {
        const Profile& pf = c.opt.profile;
        {
            const int pcw = isMain() ? pf.mainPcWidth : pf.periPcWidth;
            const int64_t byPc = (int64_t)1 << pcw;
            const int64_t limit = std::min<int64_t>(pf.maxCodeBytes, byPc);
            if ((int64_t)codeSize > limit)
                err(SrcLoc{}, "E302", std::string(domainName(d)) + " code is " + std::to_string(codeSize) + " bytes; the limit is " + std::to_string(limit) + " (" + (limit == byPc ? std::string("a ") + std::to_string(pcw) + "-bit PC, profile " + (isMain() ? "main" : "peri") + "_pc_width" : "profile max_code_bytes") + ")");
        }
        // the last instruction, and the one before every emitted procedure, must not fall through
        std::vector<const Op*> real;
        for (const Op& o : ops) if (o.kind != OpKind::Label) real.push_back(&o);
        if (real.empty()) return;
        if (!real.back()->uncond) err(real.back()->loc, "E303", std::string(domainName(d)) + " code can run off its end: the last instruction must be an unconditional branch");
        std::set<uint32_t> procStarts;
        for (auto& pp : c.pendingProcs) if (pp.info.domain == d && lab.count(pp.beginLabel)) procStarts.insert(lab[pp.beginLabel]);
        for (size_t i = 1; i < real.size(); i++) {
            if (procStarts.count(real[i]->pc) && !real[i - 1]->uncond && real[i]->pc != 0)
                err(real[i - 1]->loc, "E303", "execution falls through into a procedure: end the main flow with an unconditional branch");
        }
    }

    void validateCascade(const std::map<const Op*, uint32_t>& target) {
        if (!isMain()) return;
        std::vector<const Op*> real;
        for (const Op& o : ops) if (o.kind != OpKind::Label) real.push_back(&o);
        std::set<const Op*> covered;
        for (size_t i = 0; i < real.size(); i++) {
            const Op* b = real[i];
            if (b->kind != OpKind::MCascB) continue;
            auto tb = target.find(b);
            if (tb == target.end()) continue;
            uint32_t t = tb->second;
            size_t j = 0;
            while (j < real.size() && real[j]->pc < t) j++;
            int expect = 0, maxIdx = -1;
            uint32_t exitPc = 0;
            bool firstEnd = true;
            const size_t regionStart = j;
            size_t regionEnd = j;                                  // one past the last CASCADE_END of the region
            for (; j < real.size(); j++) {
                const Op* o = real[j];
                if (o->kind == OpKind::MCascB) { err(o->loc, "E304", "nested CASCADE_BEGIN inside a cascade region (fault_nested_cascade, spec 7.6)"); break; }
                if (o->kind == OpKind::MCascE) {
                    if (o->brArg != expect) {
                        if (maxIdx < b->brArg) err(o->loc, "E304", "CASCADE_END block index " + std::to_string(o->brArg) + " where " + std::to_string(expect) + " is expected (fault_cascade_step_mismatch)");
                        break;
                    }
                    covered.insert(o);
                    auto te = target.find(o);
                    if (te != target.end()) {
                        if (firstEnd) { exitPc = te->second; firstEnd = false; }
                        else if (te->second != exitPc) c.warn(o->loc, "W405", "CASCADE_END exits differ; the baseline recommends one common exit label");
                    }
                    maxIdx = expect++;
                    regionEnd = j + 1;
                    if (maxIdx >= 15) break;
                }
            }
            // v31 7.6.4: no ordinary BR.COND, BR.ERET or event_safe descriptor inside a cascade region
            for (size_t k = regionStart; k < regionEnd && k < real.size(); k++) {
                const Op* o = real[k];
                if (o->kind != OpKind::MBr) continue;
                if (o->brType == MB_ERET) err(o->loc, "E304", "BR.ERET inside a cascade region (spec v31 7.6.4): a cascade is never interrupted, so it has no event handler to return from");
                else if (o->eventSafe) err(o->loc, "E208", "event_safe inside a cascade region (spec v31 7.6.4, 16.7): events wait until the whole cascade has finished. Remove 'safe' here and put it on an ordinary BR.COND after the cascade");
                else err(o->loc, "E304", "ordinary branch inside a cascade region (spec v31 7.6.4): a cascade block may only fall through to its CASCADE_END" + std::string(o->synthetic ? " (a CALL expands to a branch: call the procedure before or after the cascade)" : ""));
            }
            if (regionEnd > regionStart && regionEnd <= real.size()) c.cascBody[domIdx(d)].push_back({real[regionStart]->pc, real[regionEnd - 1]->pc + (uint32_t)real[regionEnd - 1]->length()});
            if (maxIdx < b->brArg) err(b->loc, "E304", "CASCADE_BEGIN selector " + std::to_string(b->brArg) + " exceeds the last block index " + std::to_string(maxIdx) + " of its region");
        }
        for (const Op* o : real) if (o->kind == OpKind::MCascE && !covered.count(o)) err(o->loc, "E304", "CASCADE_END without a matching CASCADE_BEGIN (fault_cascade_end_without_begin)");
    }

    void successors(size_t i, const std::vector<const Op*>& real, const std::map<const Op*, uint32_t>& target, std::vector<std::pair<size_t, bool>>& out) {
        // out: (index into real, isTaken)
        const Op* o = real[i];
        std::map<uint32_t, size_t> byPc;
        (void)byPc;
        auto idxOf = [&](uint32_t pc) -> long {
            for (size_t k = 0; k < real.size(); k++) if (real[k]->pc == pc) return (long)k;
            return -1;
        };
        bool branch = o->kind == OpKind::MBr || o->kind == OpKind::MCascB || o->kind == OpKind::MCascE || o->kind == OpKind::PBr;
        if (branch) {
            auto t = target.find(o);
            if (t != target.end()) { long k = idxOf(t->second); if (k >= 0) out.push_back({(size_t)k, true}); }
            if (o->kind == OpKind::MCascB) return;
            if (!o->uncond && i + 1 < real.size()) out.push_back({i + 1, false});
            return;
        }
        if (i + 1 < real.size()) out.push_back({i + 1, false});
    }

    void validateReach(const std::map<const Op*, uint32_t>& target) {
        std::vector<const Op*> real;
        for (const Op& o : ops) if (o.kind != OpKind::Label) real.push_back(&o);
        if (real.empty()) return;
        std::vector<char> seen(real.size(), 0);
        std::vector<size_t> st{0};
        seen[0] = 1;
        for (const Ctx::VecDecl& v : c.vectors[domIdx(d)]) {          // an event handler is reachable from its vector
            auto it = lab.find(v.label);
            if (it == lab.end()) continue;
            for (size_t k = 0; k < real.size(); k++) if (real[k]->pc == it->second && !seen[k]) { seen[k] = 1; st.push_back(k); break; }
        }
        while (!st.empty()) {
            size_t i = st.back();
            st.pop_back();
            std::vector<std::pair<size_t, bool>> s;
            successors(i, real, target, s);
            for (auto& e : s) if (!seen[e.first]) { seen[e.first] = 1; st.push_back(e.first); }
        }
        bool inRun = false;
        for (size_t i = 0; i < real.size(); i++) {
            if (!seen[i] && !real[i]->synthetic) {
                if (!inRun) c.warn(real[i]->loc, "W402", "unreachable instruction '" + real[i]->text + "'");
                inRun = true;
            } else inRun = false;
        }
    }
};

}  // namespace

// ---- WCET ---------------------------------------------------------------------------------------------------------------------
static WcetResult wcetQuery(Ctx& c, std::vector<Op>& ops, const std::map<const Op*, uint32_t>& target, const std::string& name, const std::string& from, const std::string& to) {
    WcetResult r;
    r.name = name;
    r.from = from;
    r.to = to;
    auto& lab = c.labelPc[0];
    if (!lab.count(from) || !lab.count(to)) {
        r.note = "label not found in Main";
        return r;
    }
    std::vector<const Op*> real;
    for (const Op& o : ops) if (o.kind != OpKind::Label) real.push_back(&o);
    uint32_t startPc = lab[from], endPc = lab[to];
    std::map<uint32_t, size_t> idx;
    for (size_t i = 0; i < real.size(); i++) idx[real[i]->pc] = i;
    // SoftRet: the return dispatch of a procedure can reach every one of its call sites, but a given execution returns to ONE of
    // them. The analysis is therefore call-string sensitive: a call transfer pushes its return pc, the dispatch block pops it.
    // The dispatch is charged as if every comparison ran (an upper bound).
    std::map<std::string, int> dispCost;
    std::map<std::string, size_t> dispHead;
    for (size_t i = 0; i < real.size(); i++) {
        const Op* o = real[i];
        if (o->dispatchOf.empty()) continue;
        if (!dispHead.count(o->dispatchOf)) dispHead[o->dispatchOf] = i;
        bool lastOfBlock = i + 1 >= real.size() || real[i + 1]->dispatchOf != o->dispatchOf;
        (void)lastOfBlock;
        dispCost[o->dispatchOf] += o->cycles;
    }
    typedef std::pair<size_t, std::vector<uint32_t>> Key;
    std::map<Key, int> memo;
    std::set<Key> onStack;
    bool unbounded = false;
    std::string why;
    std::function<int(size_t, const std::vector<uint32_t>&)> longest = [&](size_t i, const std::vector<uint32_t>& stk) -> int {
        if (real[i]->pc == endPc) return 0;
        Key key(i, stk);
        auto m = memo.find(key);
        if (m != memo.end()) return m->second;
        if (onStack.count(key) || stk.size() > 64) { unbounded = true; why = "a loop is reachable before the end label"; return 0; }
        onStack.insert(key);
        const Op* o = real[i];
        int best = 0;
        bool any = false;
        auto consider = [&](long k, int edge, const std::vector<uint32_t>& ns) {
            if (k < 0) return;
            any = true;
            best = std::max(best, edge + longest((size_t)k, ns));
        };
        auto at = [&](uint32_t pc) -> long { auto f = idx.find(pc); return f == idx.end() ? -1 : (long)f->second; };
        if (!o->dispatchOf.empty()) {
            if (dispHead[o->dispatchOf] != i) { unbounded = true; why = "entered a return dispatch in the middle"; }
            else if (stk.empty()) { unbounded = true; why = "the path returns from '" + o->dispatchOf + "' without a call inside the query"; }
            else {
                std::vector<uint32_t> ns(stk.begin(), stk.end() - 1);
                consider(at(stk.back()), dispCost[o->dispatchOf], ns);
            }
        } else {
            bool branch = o->kind == OpKind::MBr || o->kind == OpKind::MCascB || o->kind == OpKind::MCascE || o->kind == OpKind::PBr;
            if (branch) {
                auto t = target.find(o);
                std::vector<uint32_t> ns = stk;
                if (!o->pushLabel.empty() && lab.count(o->pushLabel)) ns.push_back(lab[o->pushLabel]);
                if (t != target.end()) consider(at(t->second), o->cycles, ns);
                if (o->kind != OpKind::MCascB && !o->uncond && i + 1 < real.size())
                    consider((long)i + 1, o->cycles, stk);
            } else if (i + 1 < real.size()) {
                consider((long)i + 1, o->cycles, stk);
            }
        }
        if (!any && !unbounded) { unbounded = true; why = "the path leaves the code before the end label"; }
        onStack.erase(key);
        memo[key] = best;
        return best;
    };
    auto it = idx.find(startPc);
    if (it == idx.end()) { r.note = "start label is not an instruction"; return r; }
    int cyc = longest(it->second, std::vector<uint32_t>());
    r.bounded = !unbounded;
    r.cycles = cyc;
    r.note = unbounded ? why : "worst path; latency numbers are placeholders until the real table is supplied";
    return r;
}

// ---- image ---------------------------------------------------------------------------------------------------------------------
void buildImage(Ctx& c, Domain d, std::vector<Op>& ops) {
    Layout lo(c, d, ops);
    const Profile& pf = c.opt.profile;
    bool mainD = d == Domain::Main;
    lo.assign();
    if (lo.codeSize == 0) { c.error(SrcLoc{}, "E302", std::string(domainName(d)) + " has no instructions"); return; }
    std::map<const Op*, uint32_t> target;
    lo.resolveBranches(target);
    lo.validateFlow();
    lo.validateCascade(target);
    lo.validateReach(target);
    analyzeEvents(c, d, ops, target);

    // ---- descriptor pools and instruction bytes
    Pool move, br, csr1, csrm, alu, xfer1, xferm, pub;
    std::vector<uint8_t> code;
    auto idx = [&](Pool& p, uint32_t v, int depth, int hardLimit, const Op& o, const char* what) {
        int i = p.add(v);
        if (i >= depth || i >= hardLimit) {
            lo.err(o.loc, "E207", std::string("descriptor table full: ") + what + " needs entry " + std::to_string(i) + " but the profile allows " + std::to_string(std::min(depth, hardLimit)) +
                                      " (identical descriptors are shared; reduce the number of distinct " + what + " descriptors)");
            return 0;
        }
        return i;
    };
    struct DescRef { const char* table; int index; };
    std::map<const Op*, DescRef> descOf;
    for (const Op& o : ops) {
        if (o.kind == OpKind::Label) continue;
        uint8_t b = 0;
        switch (o.kind) {
            case OpKind::Mov: b = (uint8_t)((M_MOV << 6) | idx(move, o.desc, pf.mainMoveDepth, 64, o, "MOV")); break;
            case OpKind::Alu: b = (uint8_t)((M_ALU << 6) | idx(alu, o.desc, pf.mainAluDepth, 64, o, "ALU")); break;
            case OpKind::Csr1: {
                int i = idx(csr1, o.desc, mainD ? pf.mainCsr1Depth : pf.periCsr1Depth, 32, o, "EXEC_CSR one-byte");
                b = (uint8_t)((int)(mainD ? (int)M_CSR : (int)P_CSR) << 6 | i);
                break;
            }
            case OpKind::CsrM: b = (uint8_t)((M_CSR << 6) | (1 << 5) | idx(csrm, o.desc, pf.mainCsrMDepth, 32, o, "EXEC_CSR multi-byte")); break;
            case OpKind::MBr: case OpKind::MCascB: case OpKind::MCascE: {
                uint32_t t = target.count(&o) ? target[&o] : 0;
                codec::MainBranch mb;
                mb.type = o.brType; mb.arg = o.brArg; mb.target = t; mb.eventSafe = o.eventSafe ? 1 : 0;
                uint32_t v = codec::pack(mb);
                b = (uint8_t)((M_BR << 6) | idx(br, v, pf.mainBranchDepth, 64, o, "branch"));
                break;
            }
            case OpKind::PBr: {
                uint32_t t = target.count(&o) ? target[&o] : 0;
                codec::PeriBranch pbr;
                pbr.type = o.brType; pbr.target = t; pbr.eventSafe = o.eventSafe ? 1 : 0;
                uint32_t v = codec::pack(pbr);
                b = (uint8_t)((P_BR << 6) | idx(br, v, pf.periBranchDepth, 64, o, "branch"));
                break;
            }
            case OpKind::Pub: b = (uint8_t)((P_PUB << 6) | idx(pub, o.desc, pf.periPubDepth, 64, o, "EXEC_PUB")); break;
            case OpKind::Xfer1: b = (uint8_t)((P_XFER << 6) | idx(xfer1, o.desc, pf.periXfer1Depth, 32, o, "EXEC_XFER one-byte")); break;
            case OpKind::XferM: b = (uint8_t)((P_XFER << 6) | (1 << 5) | idx(xferm, o.desc, pf.periXferMDepth, 32, o, "EXEC_XFER multi-byte")); break;
            default: break;
        }
        {
            const char* tn = nullptr;
            int mask = 0x3F;
            switch (o.kind) {
                case OpKind::Mov: tn = "MainMoveDesc_32b"; break;
                case OpKind::Alu: tn = "MainAluDesc_8b"; break;
                case OpKind::Csr1: tn = mainD ? "MainCsrOneByteDesc_16b" : "PeriCsrOneByteDesc_16b"; mask = 0x1F; break;
                case OpKind::CsrM: tn = "MainCsrMultiByteDesc_16b"; mask = 0x1F; break;
                case OpKind::MBr: case OpKind::MCascB: case OpKind::MCascE: tn = "MainBranchDesc_16b"; break;
                case OpKind::PBr: tn = "PeriBranchDesc_16b"; break;
                case OpKind::Pub: tn = "PeriPublishDesc_16b"; break;
                case OpKind::Xfer1: tn = "PeriXferOneByteDesc_16b"; mask = 0x1F; break;
                case OpKind::XferM: tn = "PeriXferMultiByteDesc_16b"; mask = 0x1F; break;
                default: break;
            }
            if (tn) descOf[&o] = DescRef{tn, b & mask};
        }
        code.push_back(b);
        for (uint8_t x : o.extra) code.push_back(x);
        if (o.kind == OpKind::Xfer1 || o.kind == OpKind::XferM) {
            if (o.dev < 0 || o.dev > 7 || !c.dev[o.dev].defined) lo.err(o.loc, "E309", "DeviceMap[" + std::to_string(o.dev) + "] is not defined (use .devmap)");
        }
        if (o.kind == OpKind::Mov && codec::unpackMove(o.desc).srcRegion == R_CONST) {
            uint32_t off = (uint32_t)codec::unpackMove(o.desc).srcOff, len = (uint32_t)codec::unpackMove(o.desc).len;
            if (off + len > c.constPool.size()) lo.err(o.loc, "E308", "MAIN_CONST_POOL range " + std::to_string(off) + ".." + std::to_string(off + len - 1) + " is outside the " + std::to_string(c.constPool.size()) + " byte pool");
        }
    }

    // SoftRet private state must not be touched by user code
    if (mainD) {
        std::vector<RangeUse> reserved;
        bool any = false;
        for (auto& pp : c.pendingProcs) if (pp.info.domain == d && pp.info.slot >= 0) { reserved.push_back({R_SCRATCH, pp.info.slot, 2}); any = true; }
        if (any) reserved.push_back({R_SCRATCH, pf.retStageBase, 2});          // v31: the return-id comparison stages its constant in the scratchpad
        for (const Op& o : ops) for (const RangeUse& u : o.uses) for (const RangeUse& r : reserved)
            if (u.region == r.region && u.off < r.off + r.len && r.off < u.off + u.len)
                lo.err(o.loc, "E306", std::string(kRegions[u.region].name) + "[" + std::to_string(u.off) + ".." + std::to_string(u.off + u.len - 1) + "] overlaps bytes the SoftRet CALL / RET sequences use (" + kRegions[r.region].name + "[" + std::to_string(r.off) + ".." + std::to_string(r.off + r.len - 1) + "]); move them with ret_slot_base / ret_stage_base");
    }

    // ---- assemble the image
    DomainImage& im = mainD ? c.res.main : c.res.peri;
    im = DomainImage();
    im.domain = d;
    im.present = true;
    im.codeSize = lo.codeSize;
    std::vector<uint8_t>& b = im.bytes;
    b = code;
    const bool big = pf.bigEndian;
    auto padTo = [&](size_t align) { while (align > 1 && b.size() % align) b.push_back((uint8_t)pf.bootPadByte); };
    auto alignFor = [&](int sec, int entryBytes) -> size_t { int a = pf.sectionAlign[sec]; return (size_t)(a < 0 ? entryBytes : a); };
    std::vector<uint32_t>& offs = im.layout;                     // the six sideband fields, in the order of spec v31 3.1
    offs.clear();
    auto addTable = [&](int sec, const char* name, Pool& p, int entryBytes, int depth) {
        padTo(alignFor(sec, entryBytes));
        uint32_t off = (uint32_t)b.size();
        int n = (int)p.vals.size();
        if (c.opt.padTables) n = std::max(n, depth);
        for (int i = 0; i < n; i++) put(b, i < (int)p.vals.size() ? p.vals[i] : 0, entryBytes, big);
        im.sections.push_back(SectionInfo{name, off, (uint32_t)(n * entryBytes), n, entryBytes, depth});
        offs.push_back(off);
    };
    im.sections.push_back(SectionInfo{"code", 0, lo.codeSize, 0, 1, pf.maxCodeBytes});
    if (mainD) {
        addTable(PS_MAIN_MOVE, "MainMoveDesc_32b", move, 4, pf.mainMoveDepth);
        addTable(PS_MAIN_BRANCH, "MainBranchDesc_16b", br, 2, pf.mainBranchDepth);
        addTable(PS_MAIN_CSR1, "MainCsrOneByteDesc_16b", csr1, 2, pf.mainCsr1Depth);
        addTable(PS_MAIN_CSRM, "MainCsrMultiByteDesc_16b", csrm, 2, pf.mainCsrMDepth);
        addTable(PS_MAIN_ALU, "MainAluDesc_8b", alu, 1, pf.mainAluDepth);
        padTo(alignFor(PS_MAIN_CONST, 1));
        uint32_t off = (uint32_t)b.size();
        b.insert(b.end(), c.constPool.begin(), c.constPool.end());
        im.sections.push_back(SectionInfo{"MAIN_CONST_POOL", off, (uint32_t)c.constPool.size(), (int)c.constPool.size(), 1, pf.constPoolMax});
        offs.push_back(off);
    } else {
        addTable(PS_PERI_XFER1, "PeriXferOneByteDesc_16b", xfer1, 2, pf.periXfer1Depth);
        addTable(PS_PERI_XFERM, "PeriXferMultiByteDesc_16b", xferm, 2, pf.periXferMDepth);
        addTable(PS_PERI_PUB, "PeriPublishDesc_16b", pub, 2, pf.periPubDepth);
        addTable(PS_PERI_BRANCH, "PeriBranchDesc_16b", br, 2, pf.periBranchDepth);
        addTable(PS_PERI_CSR1, "PeriCsrOneByteDesc_16b", csr1, 2, pf.periCsr1Depth);
        padTo(alignFor(PS_PERI_DEVMAP, pf.deviceMapEntryBytes));
        uint32_t off = (uint32_t)b.size();
        int top = -1;
        for (int i = 0; i < 8; i++) if (c.dev[i].defined) top = i;
        // DEVICE_MAP_ENTRY_BYTES wide entries; bus_type (1 bit) and i2c_addr_7b (7 bits) sit where the profile says (spec v31 9.1 / handoff 2.8)
        const int eb = pf.deviceMapEntryBytes, bits = 8 * eb;
        if (pf.deviceMapBusShift + 1 > bits || pf.deviceMapAddrShift + 7 > bits || (pf.deviceMapBusShift >= pf.deviceMapAddrShift && pf.deviceMapBusShift < pf.deviceMapAddrShift + 7))
            lo.err(SrcLoc{}, "E313", "the DeviceMap entry packing does not fit: bus_type at bit " + std::to_string(pf.deviceMapBusShift) + " and i2c_addr_7b at bits " + std::to_string(pf.deviceMapAddrShift) + ".." +
                                         std::to_string(pf.deviceMapAddrShift + 6) + " must lie inside " + std::to_string(eb) + " byte(s) and not overlap (profile device_map_entry_bytes, device_map_bus_shift, device_map_addr_shift)");
        for (int i = 0; i <= top; i++) {
            uint32_t v = 0;
            if (!c.dev[i].defined) c.warn(SrcLoc{}, "W404", "DeviceMap[" + std::to_string(i) + "] is not defined; the entry is emitted as 0 (I2C address 0)");
            else { codec::DevEntry de; de.busType = c.dev[i].bus; de.addr7 = c.dev[i].addr; v = codec::packDev(pf, de); }
            put(b, v, eb, big);
        }
        im.sections.push_back(SectionInfo{"DeviceMap", off, (uint32_t)((top + 1) * eb), top + 1, eb, pf.deviceMapDepth});
        offs.push_back(off);
    }
    // the sideband fields have a width (profile layout_offset_bits): every offset must fit
    {
        const char* const* names = layoutFieldNames(d);
        for (size_t i = 0; i < offs.size(); i++)
            if (pf.layoutOffsetBits < 32 && offs[i] >= (1u << pf.layoutOffsetBits))
                lo.err(SrcLoc{}, "E314", std::string(domainName(d)) + " ImageLayout field " + names[i] + " = " + std::to_string(offs[i]) + " does not fit the " + std::to_string(pf.layoutOffsetBits) +
                                             "-bit sideband field (profile layout_offset_bits)");
    }
    // the image and the window of the profile: capacity, and the 32-bit words it is loaded as (the last word is padded; images never share a word)
    {
        const int cap = mainD ? pf.mainCapacityBytes : pf.periCapacityBytes;
        if ((int64_t)b.size() > cap)
            lo.err(SrcLoc{}, "E312", std::string(domainName(d)) + " image is " + std::to_string(b.size()) + " bytes but its window holds " + std::to_string(cap) + " (profile " + (mainD ? "main" : "peri") +
                                         "_capacity_bytes): shorten the program or enlarge the window");
        im.baseWord = (uint32_t)(mainD ? pf.mainImageBaseWord : pf.periImageBaseWord);
        im.reservedWords = (uint32_t)((cap + 3) / 4);
        im.words = packWords(b, (uint8_t)pf.bootPadByte);
    }

    // ---- debug info
    for (const Op& o : ops) {
        if (o.kind == OpKind::Label) continue;
        InstrInfo ii;
        ii.domain = d;
        ii.pc = o.pc;
        ii.length = (uint32_t)o.length();
        ii.loc = o.loc;
        ii.text = o.text;
        ii.frames = o.frames;
        ii.synthetic = o.synthetic;
        ii.note = o.note;
        ii.cycles = o.cycles;
        auto dr = descOf.find(&o);
        if (dr != descOf.end()) { ii.table = dr->second.table; ii.descIndex = dr->second.index; }
        if (o.kind == OpKind::MBr || o.kind == OpKind::MCascB || o.kind == OpKind::MCascE || o.kind == OpKind::PBr) {
            const bool eret = (o.kind == OpKind::MBr && o.brType == MB_ERET) || (o.kind == OpKind::PBr && o.brType == PB_ERET);
            ii.isBranch = !eret;                                  // BR.ERET returns to a run-time resume PC: it has no static target
            ii.target = target.count(&o) ? target[&o] : 0;
            ii.fallsThrough = o.kind != OpKind::MCascB && !o.uncond;
        }
        c.res.instrs.push_back(ii);
    }
    for (auto& pp : c.pendingProcs) if (pp.info.domain == d) {
        ProcInfo pi = pp.info;
        pi.startPc = lo.lab.count(pp.beginLabel) ? lo.lab[pp.beginLabel] : 0;
        pi.endPc = lo.lab.count(pp.endLabel) ? lo.lab[pp.endLabel] : pi.startPc;
        c.res.procs.push_back(pi);
    }
    for (auto& pc : c.pendingCalls) if (pc.info.domain == d) {
        CallSiteInfo ci = pc.info;
        ci.transferPc = lo.lab.count(pc.beginLabel) ? lo.lab[pc.beginLabel] : 0;
        ci.returnPc = lo.lab.count(pc.endLabel) ? lo.lab[pc.endLabel] : 0;
        if (mainD) {
            WcetResult w = wcetQuery(c, ops, target, "", pc.beginLabel, pc.endLabel);
            ci.wcetBounded = w.bounded;
            ci.wcetCycles = w.cycles;
        }
        c.res.callSites.push_back(ci);
    }
    if (mainD)                                                // a procedure's worst case is its worst call site
        for (ProcInfo& pi : c.res.procs) {
            if (pi.domain != d) continue;
            bool any = false, bounded = true;
            int worst = 0;
            for (const CallSiteInfo& cs : c.res.callSites) if (cs.domain == d && cs.callee == pi.name) {
                any = true;
                bounded = bounded && cs.wcetBounded;
                worst = std::max(worst, cs.wcetCycles);
            }
            pi.wcetBounded = any && bounded;
            pi.wcetCycles = any ? worst : 0;
        }
    for (const Op& o : ops) if (o.kind == OpKind::Label && o.label.compare(0, 2, "__") != 0 && o.label.find('#') == std::string::npos) {
        SymbolInfo s;
        s.name = (mainD ? "main:" : "peri:") + o.label;
        s.kind = "label";
        s.value = o.pc;
        s.loc = o.loc;
        c.res.symbols.push_back(s);
    }
    if (mainD) {
        std::vector<WcetResult>& w = c.res.wcet;
        for (auto& q : c.opt.wcetQueries) {
            size_t colon = q.find(':');
            if (colon == std::string::npos) { c.error(SrcLoc{}, "E102", "--wcet expects start_label:end_label, got '" + q + "'"); continue; }
            w.push_back(wcetQuery(c, ops, target, q, q.substr(0, colon), q.substr(colon + 1)));
        }
        for (auto& dcl : c.wcetDecls) {
            WcetResult r = wcetQuery(c, ops, target, dcl.name, dcl.from, dcl.to);
            if (r.note == "label not found in Main") c.error(dcl.loc, "E301", ".wcet " + dcl.name + ": label '" + dcl.from + "' or '" + dcl.to + "' not found in Main");
            w.push_back(r);
        }
    }
}

// ---- shared program SRAM: the windows of the two images (spec v31 3.1) -------------------------------------------------------------------
// Both reserved windows (not just what the current programs happen to occupy) must lie inside the SRAM and must not overlap: Main's last
// word is never Peri's first (every image is padded to whole words, windows are counted in whole words, rounded up).
static void validatePlacement(Ctx& c) {
    const Profile& pf = c.opt.profile;
    struct W { const char* name; int64_t base, words; };
    const W w[2] = {{"Main", pf.mainImageBaseWord, (pf.mainCapacityBytes + 3) / 4}, {"Peri", pf.periImageBaseWord, (pf.periCapacityBytes + 3) / 4}};
    for (const W& x : w)
        if (x.base + x.words > pf.physicalSramDepthWords)
            c.error(SrcLoc{}, "E311", std::string("the ") + x.name + " window [word " + std::to_string(x.base) + ", " + std::to_string(x.base + x.words) + ") does not fit the " + std::to_string(pf.physicalSramDepthWords) +
                                          "-word SRAM (profile sram_depth_words, " + (x.name[0] == 'M' ? "main" : "peri") + "_image_base_word, " + (x.name[0] == 'M' ? "main" : "peri") + "_capacity_bytes). Image base addresses are WORD addresses");
    if (w[0].base < w[1].base + w[1].words && w[1].base < w[0].base + w[0].words)
        c.error(SrcLoc{}, "E311", "the Main window [word " + std::to_string(w[0].base) + ", " + std::to_string(w[0].base + w[0].words) + ") and the Peri window [word " + std::to_string(w[1].base) + ", " +
                                      std::to_string(w[1].base + w[1].words) + ") overlap: move main_image_base_word / peri_image_base_word apart or reduce the capacities. The two images must never share a physical word");
}

// ---- driver --------------------------------------------------------------------------------------------------------------------
int Result::errorCount() const { int n = 0; for (auto& d : diags) if (d.sev == Diagnostic::Error) n++; return n; }
int Result::warningCount() const { int n = 0; for (auto& d : diags) if (d.sev == Diagnostic::Warning) n++; return n; }

Result assemble(const std::vector<Source>& sources, const IncludeResolver& resolver, const Options& opt) {
    Ctx c;
    c.opt = opt;
    c.resolver = resolver;
    parseSources(c, sources);
    if (!c.prog[0].present && !c.prog[1].present && c.res.errorCount() == 0) c.error(SrcLoc{}, "E105", "no '.main' or '.peri' section in the input");
    if (c.res.errorCount() == 0) {
        std::vector<Op> lowered[2];
        bool have[2] = {false, false};
        for (Domain d : {Domain::Main, Domain::Peri}) {
            if (!c.prog[domIdx(d)].present) continue;
            std::vector<ProcInfo> procs;
            bool ok = lowerDomain(c, d, lowered[domIdx(d)], procs);
            for (auto& p : procs) c.res.procs.push_back(p);
            if (ok) { buildImage(c, d, lowered[domIdx(d)]); have[domIdx(d)] = true; }
        }
        validatePlacement(c);
        if (c.res.errorCount() == 0) runDataflow(c, have[0] ? &lowered[0] : nullptr, have[1] ? &lowered[1] : nullptr);
    }
    for (auto& kv : c.syms) {
        SymbolInfo s;
        s.name = kv.first;
        s.kind = kv.second.kind;
        s.value = kv.second.value;
        s.loc = kv.second.loc;
        c.res.symbols.push_back(s);
    }
    // read top to bottom: by file, then by line (diagnostics without a location come first)
    std::stable_sort(c.res.diags.begin(), c.res.diags.end(), [](const Diagnostic& a, const Diagnostic& b) { return a.loc.file != b.loc.file ? a.loc.file < b.loc.file : a.loc.line < b.loc.line; });
    c.res.callModel = opt.callModel == CallModel::SoftRet ? "softret" : "inline";
    c.res.profile = opt.profile;
    c.res.ok = c.res.errorCount() == 0 && !(opt.werror && c.res.warningCount() > 0);
    return std::move(c.res);
}

}  // namespace asmcore
