// Lowering: statements -> Ops. Operand parsing for every instruction, and the two CALL / RET models.
#include <algorithm>
#include <functional>

#include "codec.h"
#include "internal.h"

namespace asmcore {
namespace {

using namespace isa;

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> named;
};

Args parseArgs(const std::string& rest) {
    Args a;
    if (trim(rest).empty()) return a;
    for (const std::string& t : splitTop(rest)) {
        size_t eq = t.find('=');
        bool isNamed = false;
        if (eq != std::string::npos && eq > 0 && !(eq + 1 < t.size() && t[eq + 1] == '=')) {
            std::string k = trim(t.substr(0, eq));
            isNamed = isIdent(k);
            if (isNamed) a.named[lower(k)] = trim(t.substr(eq + 1));
        }
        if (!isNamed) a.pos.push_back(t);
    }
    return a;
}

struct Scope {
    const ProcDef* proc = nullptr;
    std::string prefix;       // prepended to the proc's local labels (makes each inline copy unique)
    std::string retLabel;     // inline: where RET goes
    bool soft = false;
};

struct RegRef {
    int region = -1, off = 0, len = 0;
};

struct Lowerer {
    Ctx& c;
    Domain dom;
    std::vector<Op>& ops;
    bool bad = false;
    int instance = 0;
    int callSeq = 0;

    // SoftRet bookkeeping
    struct SoftProc { int sites = 0, slot = -1; bool used = false; };
    std::map<std::string, SoftProc> soft;
    std::map<const Stmt*, std::pair<std::string, int>> siteOf;   // CALL statement -> (callee, k)

    Lowerer(Ctx& cc, Domain d, std::vector<Op>& o) : c(cc), dom(d), ops(o) {}

    void err(const SrcLoc& l, const char* code, const std::string& m) { c.error(l, code, m); bad = true; }

    // ---- small helpers -------------------------------------------------------------------------------------------------
    std::string scopedLabel(const Scope& sc, const std::string& n) const {
        if (sc.proc && sc.proc->localLabels.count(n)) return sc.prefix + n;
        return n;
    }
    Op make(OpKind k, const Stmt& st, const std::vector<Frame>& fr) {
        Op o;
        o.kind = k;
        o.loc = st.loc;
        o.text = trim(st.op + " " + st.rest);
        o.frames = fr;
        return o;
    }
    Op label(const std::string& name, const Stmt& st) {
        Op o;
        o.kind = OpKind::Label;
        o.label = name;
        o.loc = st.loc;
        return o;
    }
    bool value(const std::string& text, int64_t lo, int64_t hi, const char* what, const Stmt& st, int64_t& v) {
        if (!c.eval(text, v, st.loc)) { bad = true; return false; }
        if (v < lo || v > hi) { err(st.loc, "E201", std::string(what) + " " + std::to_string(v) + " is out of range (" + std::to_string(lo) + ".." + std::to_string(hi) + ")"); return false; }
        return true;
    }
    static bool pick(const Args& a, const char* key, size_t pos, std::string& out) {
        auto it = a.named.find(key);
        if (it != a.named.end()) { out = it->second; return true; }
        if (pos < a.pos.size()) { out = a.pos[pos]; return true; }
        return false;
    }

    // ---- MOV -----------------------------------------------------------------------------------------------------------
    bool parseRegion(const std::string& txt0, int wantLen, const Stmt& st, RegRef& r, bool isSrc) {
        std::string txt = trim(txt0);
        std::string up = upper(txt);
        if (up.compare(0, 6, "CONST(") == 0 && txt.back() == ')') {
            auto parts = splitTop(txt.substr(6, txt.size() - 7));
            int64_t v;
            if (parts.empty() || parts.size() > 2 || !c.eval(parts[0], v, st.loc)) { err(st.loc, "E102", "CONST(expression[, width])"); return false; }
            int64_t w = wantLen > 0 ? wantLen : 2;
            if (parts.size() == 2 && !value(parts[1], 1, 4, "constant width", st, w)) return false;
            int64_t lim = w >= 8 ? INT64_MAX : (int64_t(1) << (8 * w)) - 1;
            if (v < -(lim >> 1) - 1 || v > lim) { err(st.loc, "E201", "constant " + std::to_string(v) + " does not fit " + std::to_string(w) + " byte(s)"); return false; }
            int off = c.constOffsetFor(v, (int)w);
            if (off < 0) { err(st.loc, "E308", "MAIN_CONST_POOL is full"); return false; }
            r.region = R_CONST; r.off = off; r.len = (int)w;
            return true;
        }
        size_t lb = txt.find('[');
        std::string name = upper(trim(lb == std::string::npos ? txt : txt.substr(0, lb)));
        const RegionInfo* ri = regionByName(name);
        if (!ri) { err(st.loc, "E202", "unknown region '" + name + "'"); return false; }
        r.region = ri->id;
        if (lb == std::string::npos) {
            if (ri->id == R_OPA || ri->id == R_OPB) { r.off = 0; r.len = 2; return true; }
            err(st.loc, "E202", "region " + std::string(ri->name) + " needs a byte range, e.g. " + ri->name + "[0..1]");
            return false;
        }
        if (txt.back() != ']') { err(st.loc, "E102", "']' expected"); return false; }
        std::string in = trim(txt.substr(lb + 1, txt.size() - lb - 2));
        if (!in.empty() && in[0] == '@') {
            std::string cn = trim(in.substr(1));
            auto it = c.syms.find(cn);
            if (it == c.syms.end() || it->second.kind != "const") { err(st.loc, "E103", "'" + cn + "' is not a .const"); return false; }
            r.off = (int)it->second.value;
            r.len = it->second.size;
            return true;
        }
        size_t dd = in.find("..");
        int64_t a = 0, b = 0;
        if (dd == std::string::npos) {
            if (!c.eval(in, a, st.loc)) { bad = true; return false; }
            b = a;
        } else if (!c.eval(in.substr(0, dd), a, st.loc) || !c.eval(in.substr(dd + 2), b, st.loc)) {
            bad = true;
            return false;
        }
        if (a < 0 || b < a) { err(st.loc, "E201", "bad byte range in '" + txt + "'"); return false; }
        r.off = (int)a;
        r.len = (int)(b - a + 1);
        (void)isSrc;
        return true;
    }

    void doMov(const Stmt& st, const std::vector<Frame>& fr, bool synthetic = false) {
        size_t arrow = findTop(st.rest, "->");
        if (arrow == std::string::npos) { err(st.loc, "E102", "MOV source -> destination"); return; }
        std::string s = st.rest.substr(0, arrow), d = st.rest.substr(arrow + 2);
        RegRef src, dst;
        // destination first when the source is CONST(...): its width follows the destination
        bool srcConst = upper(trim(s)).compare(0, 6, "CONST(") == 0;
        if (srcConst) {
            if (!parseRegion(d, 0, st, dst, false)) return;
            if (!parseRegion(s, dst.len, st, src, true)) return;
        } else {
            if (!parseRegion(s, 0, st, src, true) || !parseRegion(d, 0, st, dst, false)) return;
        }
        movChecks(st, src, dst);
        if (bad) return;
        Op o = make(OpKind::Mov, st, fr);
        o.synthetic = synthetic;
        codec::Move mv;
        mv.srcRegion = src.region; mv.dstRegion = dst.region; mv.srcOff = src.off; mv.dstOff = dst.off; mv.len = src.len;
        o.desc = codec::pack(mv);
        if (!synthetic) {
            if (src.region == R_SCRATCH || src.region == R_CSRBUF) o.uses.push_back({src.region, src.off, src.len});
            if (dst.region == R_SCRATCH || dst.region == R_CSRBUF) o.uses.push_back({dst.region, dst.off, dst.len});
        }
        ops.push_back(o);
    }

    void movChecks(const Stmt& st, const RegRef& s, const RegRef& d) {
        const RegionInfo& sr = kRegions[s.region];
        const RegionInfo& dr = kRegions[d.region];
        if (!sr.readable) err(st.loc, "E202", std::string(sr.name) + " cannot be a MOV source");
        if (d.region == R_CSRBUF)
            err(st.loc, "E202", "MAIN_CSR_DATA_BUF cannot be a MOV destination: since spec v31 (5.1) it is a read-back buffer that only EXEC_CSR READ8 / READ_BUF writes. "
                                "Stage the bytes in MAIN_SCRATCHPAD instead (MOV ... -> MAIN_SCRATCHPAD[n], then EXEC_CSR WRITE_SP reads them from there)");
        else if (!dr.writable) err(st.loc, "E202", std::string(dr.name) + " cannot be a MOV destination");
        if (s.region == d.region) {
            const bool overlap = s.off < d.off + d.len && d.off < s.off + s.len;
            if (overlap) err(st.loc, "E202", "MOV between overlapping ranges of one region is illegal (spec v31 5.2: no memmove semantics). Use ranges that do not overlap, or copy through another region");
            else if (!c.opt.profile.sameRegionMov) err(st.loc, "E202", "this target profile does not allow MOV inside one region (same_region_mov = false): copy through another region");
        }
        if (s.region == R_CONST && d.region != R_SCRATCH && d.region != R_CSRBUF)
            err(st.loc, "E202", "MAIN_CONST_POOL can only be moved to MAIN_SCRATCHPAD (spec v31 5.1): load the constant into the scratchpad first, then MOV it on to ALU_OPA / ALU_OPB");
        if (s.len != d.len) err(st.loc, "E202", "source length " + std::to_string(s.len) + " differs from destination length " + std::to_string(d.len));
        if (s.len < 1 || s.len > 32) err(st.loc, "E201", "MOV length must be 1..32");
        if ((d.region == R_OPA || d.region == R_OPB) && (d.off != 0 || d.len != 2)) err(st.loc, "E202", "ALU operands are loaded with offset 0 and length 2 (spec 5.2)");
        if (s.off > 255 || d.off > 255) err(st.loc, "E201", "offset does not fit 8 bits");
        if (sr.size && s.off + s.len > sr.size) err(st.loc, "E201", std::string(sr.name) + " range " + std::to_string(s.off) + ".." + std::to_string(s.off + s.len - 1) + " is outside 0.." + std::to_string(sr.size - 1));
        if (dr.size && d.off + d.len > dr.size) err(st.loc, "E201", std::string(dr.name) + " range " + std::to_string(d.off) + ".." + std::to_string(d.off + d.len - 1) + " is outside 0.." + std::to_string(dr.size - 1));
    }

    // ---- ALU -----------------------------------------------------------------------------------------------------------
    void doAlu(const Stmt& st, const std::vector<Frame>& fr, bool synthetic = false) {
        std::vector<std::string> tok;
        {
            std::string t;
            for (char ch : st.rest + " ") {
                if (std::isspace((unsigned char)ch) || ch == ',') { if (!t.empty()) tok.push_back(t); t.clear(); }
                else t += ch;
            }
        }
        if (tok.empty()) { err(st.loc, "E102", "ALU operation expected"); return; }
        int signedMode = -1;
        std::vector<std::string> rest;
        for (size_t i = 1; i < tok.size(); i++) {
            std::string u = upper(tok[i]);
            if (u == "SIGNED" || u == "SIGNED_MODE=1") signedMode = 1;
            else if (u == "UNSIGNED" || u == "SIGNED_MODE=0") signedMode = 0;
            else rest.push_back(tok[i]);
        }
        std::string opn = upper(tok[0]);
        if (opn.compare(0, 4, "CMP.") == 0) opn = opn.substr(4);
        uint32_t d = 0;
        int sm = signedMode < 0 ? 0 : signedMode;
        if (opn == "ADD" || opn == "SUB" || opn == "MUL" || opn == "INC" || opn == "DEC") {
            int code = opn == "ADD" ? ARITH_ADD : opn == "SUB" ? ARITH_SUB : opn == "MUL" ? ARITH_MUL : opn == "INC" ? ARITH_INC : ARITH_DEC;
            d = codec::packArith(c.opt.profile.alu, sm, code);
        } else if (opn == "EQ" || opn == "NE" || opn == "LT" || opn == "GE") {
            int code = opn == "EQ" ? CMP_EQ : opn == "NE" ? CMP_NE : opn == "LT" ? CMP_LT : CMP_GE;
            if ((code == CMP_EQ || code == CMP_NE) && signedMode == 1) err(st.loc, "E203", "EQ / NE require signed_mode=0 (spec 6.5)");
            d = codec::packCmp(c.opt.profile.alu, code == CMP_EQ || code == CMP_NE ? 0 : sm, code);
        } else if (opn == "AND" || opn == "OR" || opn == "XOR" || opn == "NOT") {
            int code = opn == "AND" ? LOGIC_AND : opn == "OR" ? LOGIC_OR : opn == "XOR" ? LOGIC_XOR : LOGIC_NOT;
            if (signedMode >= 0) c.warn(st.loc, "W410", "signed / unsigned has no meaning for a LOGIC operation");
            d = codec::packLogic(c.opt.profile.alu, code);
        } else if (opn == "SHL" || opn == "SHR") {
            if (rest.size() != 1) { err(st.loc, "E102", "ALU " + opn + " amount [signed|unsigned]"); return; }
            int64_t amt;
            if (!value(rest[0], 1, 8, "shift amount", st, amt)) return;
            rest.clear();
            d = codec::packShift(sm, opn == "SHR" ? SHIFT_SHR : SHIFT_SHL, (int)amt);
        } else {
            err(st.loc, "E203", "unknown ALU operation '" + tok[0] + "'");
            return;
        }
        if (!rest.empty()) { err(st.loc, "E102", "unexpected operand '" + rest[0] + "' after ALU " + opn); return; }
        Op o = make(OpKind::Alu, st, fr);
        o.synthetic = synthetic;
        o.desc = d;
        o.uncond = false;
        ops.push_back(o);
    }

    // ---- EXEC_CSR ------------------------------------------------------------------------------------------------------
    void doCsr(const Stmt& st, const std::vector<Frame>& fr) {
        std::string rest = st.rest;
        std::string arrowDst;
        size_t arrow = findTop(rest, "->");
        if (arrow != std::string::npos) { arrowDst = trim(rest.substr(arrow + 2)); rest = rest.substr(0, arrow); }
        size_t sp = 0;
        rest = trim(rest);
        while (sp < rest.size() && !std::isspace((unsigned char)rest[sp])) sp++;
        std::string opn = upper(rest.substr(0, sp));
        Args a = parseArgs(trim(rest.substr(sp)));
        bool peri = dom == Domain::Peri;
        std::string addrTxt;
        if (!pick(a, "csr_addr", 0, addrTxt) && !pick(a, "csr", 0, addrTxt) && !pick(a, "addr", 0, addrTxt)) { err(st.loc, "E102", "EXEC_CSR operation csr_name, ..."); return; }
        if (a.named.count("csr_addr")) a.pos.insert(a.pos.begin(), addrTxt);   // keep positional numbering stable below
        int64_t addr;
        if (!value(addrTxt, 0, 0xFFF, "CSR address", st, addr)) return;
        if (opn == "WRITE_BUF") {
            err(st.loc, "E204", "EXEC_CSR WRITE_BUF no longer exists: spec v31 (4.3) renamed it WRITE_SP and changed its data source. Write 'EXEC_CSR WRITE_SP csr, len=N, ptr=P' "
                                "where ptr points into MAIN_SCRATCHPAD (v24 read MAIN_CSR_DATA_BUF, which MOV can no longer fill)");
            return;
        }
        bool multi = opn == "WRITE_SP" || opn == "READ_BUF";
        if (!multi) {
            int op;
            if (opn == "WRITE8" || opn == "WRITE8_IMM") op = CSR1_WRITE8;
            else if (opn == "SET8" || opn == "RMW_SET8_IMM") op = CSR1_SET8;
            else if (opn == "CLR8" || opn == "RMW_CLR8_IMM") op = CSR1_CLR8;
            else if (opn == "READ8") op = CSR1_READ8;
            else { err(st.loc, "E204", "unknown EXEC_CSR operation '" + opn + "'"); return; }
            if (op == CSR1_READ8 && peri) { err(st.loc, "E204", "Peri EXEC_CSR READ8 is illegal in the baseline (spec 4.2)"); return; }
            std::string immTxt;
            int64_t imm = 0;
            if (op == CSR1_READ8) {
                if (!arrowDst.empty()) {
                    RegRef r;
                    if (!parseRegion(arrowDst, 1, st, r, false)) return;
                    if (r.region != R_CSRBUF || r.len != 1) { err(st.loc, "E204", "READ8 destination must be one byte of MAIN_CSR_DATA_BUF"); return; }
                    imm = r.off;
                } else if (pick(a, "ptr", 1, immTxt)) {
                    if (!value(immTxt, 0, 31, "destination pointer", st, imm)) return;
                } else { err(st.loc, "E102", "EXEC_CSR READ8 csr -> MAIN_CSR_DATA_BUF[n]"); return; }
            } else {
                if (!pick(a, "imm", 1, immTxt) && !pick(a, "value", 1, immTxt) && !pick(a, "mask", 1, immTxt)) { err(st.loc, "E102", "EXEC_CSR " + opn + " csr, imm8"); return; }
                if (!value(immTxt, -128, 255, "immediate", st, imm)) return;
            }
            Op o = make(OpKind::Csr1, st, fr);
            codec::Csr1 c1;
            c1.op = op; c1.addr = (uint32_t)addr;
            o.desc = codec::pack(c1);
            o.extra.push_back((uint8_t)(imm & 0xFF));
            if (op == CSR1_READ8) o.uses.push_back({R_CSRBUF, (int)imm, 1});
            ops.push_back(o);
            return;
        }
        if (peri) { err(st.loc, "E204", "Peri EXEC_CSR MultiByte is illegal in the baseline (spec 4.1)"); return; }
        std::string lenTxt, ptrTxt;
        if (!pick(a, "len", 1, lenTxt) || !pick(a, "ptr", 2, ptrTxt)) { err(st.loc, "E102", "EXEC_CSR " + opn + " csr, len=N, ptr=P  (" + (opn == "WRITE_SP" ? "ptr: MAIN_SCRATCHPAD source" : "ptr: MAIN_CSR_DATA_BUF destination") + ")"); return; }
        int64_t len, ptr;
        if (!value(lenTxt, 1, 32, "length", st, len) || !value(ptrTxt, 0, 31, "ptr", st, ptr)) return;
        if (ptr + len > 32) { err(st.loc, "E204", std::string("ptr + len exceeds ") + (opn == "WRITE_SP" ? "MAIN_SCRATCHPAD" : "MAIN_CSR_DATA_BUF") + " (32 bytes)"); return; }
        Op o = make(OpKind::CsrM, st, fr);
        int op = opn == "WRITE_SP" ? CSRM_WRITE_SP : CSRM_READ_BUF;
        codec::CsrM cm;
        cm.op = op; cm.page = (int)((addr >> 8) & 0xF); cm.len = (int)len; cm.ptr = (int)ptr;
        o.desc = codec::pack(cm);
        o.extra.push_back((uint8_t)(addr & 0xFF));
        o.uses.push_back({op == CSRM_WRITE_SP ? (int)R_SCRATCH : (int)R_CSRBUF, (int)ptr, (int)len});
        ops.push_back(o);
    }

    // ---- branches ------------------------------------------------------------------------------------------------------
    void emitBranch(const Stmt& st, const std::vector<Frame>& fr, const Scope& sc, int type, int arg, const std::string& target, bool uncond, OpKind k, bool synthetic = false, bool eventSafe = false) {
        Op o = make(k, st, fr);
        o.synthetic = synthetic;
        o.brType = type;
        o.brArg = arg;
        o.eventSafe = eventSafe;
        o.label = synthetic ? target : scopedLabel(sc, target);
        o.uncond = uncond;
        ops.push_back(o);
    }

    int condCode(const std::string& name, const NamedCode* tab, size_t n, const Stmt& st, bool mainDomain) {
        std::string u = upper(name);
        for (size_t i = 0; i < n; i++) if (u == tab[i].name) {
            if (mainDomain && tab[i].code == 0xE && !c.opt.profile.profileCond0) { err(st.loc, "E205", "PROFILE_COND0 is not defined by this profile"); return -1; }
            return tab[i].code;
        }
        int64_t v;
        if (mainDomain && c.eval(name, v, st.loc, "E205") && v >= 0 && v < 0xF) return (int)v;
        err(st.loc, "E205", "unknown branch condition '" + name + "'");
        return -1;
    }

    // The attribute that makes an ordinary branch an event checkpoint (v31 16.3): a trailing `safe`, or event_safe=1.
    bool takeSafe(Args& a, const Stmt& st, bool& safe) {
        safe = false;
        if (!a.pos.empty() && upper(trim(a.pos.back())) == "SAFE") { safe = true; a.pos.pop_back(); }
        auto it = a.named.find("event_safe");
        if (it != a.named.end()) {
            std::string v = trim(it->second);
            if (v == "1" || upper(v) == "TRUE") safe = true;
            else if (v != "0" && upper(v) != "FALSE") { err(st.loc, "E208", "event_safe is 0 or 1 (or the word 'safe' after the label)"); return false; }
            a.named.erase(it);
        }
        return true;
    }

    void doBranch(const Stmt& st, const std::vector<Frame>& fr, const Scope& sc, const std::string& mn) {
        Args a = parseArgs(st.rest);
        std::string target;
        bool safe = false;
        if (!takeSafe(a, st, safe)) return;
        const bool eret = mn == "BR.ERET" || mn == "ERET";
        const bool cascade = mn == "BR.CASCADE_BEGIN" || mn == "CASCADE_BEGIN" || mn == "BR.CASCADE_END" || mn == "CASCADE_END";
        if (safe && (eret || cascade)) {
            err(st.loc, "E208", std::string("event_safe is only legal on an ordinary branch (spec v31 7.2, 16.7): ") + (eret ? "BR.ERET" : "CASCADE_BEGIN / CASCADE_END") +
                                    " is never an event checkpoint. Remove 'safe'; put it on a later ordinary BR.COND (or JMP)");
            return;
        }
        if (eret) {
            if (!a.pos.empty() || !a.named.empty()) { err(st.loc, "E102", "BR.ERET takes no operands (it returns to the saved resume PC)"); return; }
            // descriptor: arg = 0, event_safe = 0, target = 0 (main) / br_type 8 (peri); no label, never falls through
            Op o = make(dom == Domain::Main ? OpKind::MBr : OpKind::PBr, st, fr);
            o.brType = dom == Domain::Main ? MB_ERET : PB_ERET;
            o.brArg = 0;
            o.uncond = true;
            ops.push_back(o);
            return;
        }
        if (dom == Domain::Main) {
            if (mn == "JMP") {
                if (a.pos.size() != 1) { err(st.loc, "E102", "JMP label [, safe]"); return; }
                emitBranch(st, fr, sc, MB_COND, 0, a.pos[0], true, OpKind::MBr, false, safe);
                return;
            }
            std::string cond;
            if (mn == "BR.CASCADE_BEGIN" || mn == "CASCADE_BEGIN") {
                std::string selTxt;
                if (!pick(a, "sel", 0, selTxt) || !pick(a, "target", 1, target)) { err(st.loc, "E102", "CASCADE_BEGIN selector, block0_label"); return; }
                int64_t sel;
                if (!value(selTxt, 0, 15, "selector", st, sel)) return;
                emitBranch(st, fr, sc, MB_CASCADE_BEGIN, (int)sel, target, false, OpKind::MCascB);
                return;
            }
            if (mn == "BR.CASCADE_END" || mn == "CASCADE_END") {
                std::string idxTxt;
                if (!pick(a, "idx", 0, idxTxt) || !pick(a, "target", 1, target)) { err(st.loc, "E102", "CASCADE_END block_index, exit_label"); return; }
                int64_t idx;
                if (!value(idxTxt, 0, 15, "block index", st, idx)) return;
                emitBranch(st, fr, sc, MB_CASCADE_END, (int)idx, target, false, OpKind::MCascE);
                return;
            }
            // BR.COND cond, label
            if (!pick(a, "cond_sel", 0, cond) || !pick(a, "target", 1, target)) { err(st.loc, "E102", "BR.COND condition, label"); return; }
            int code = condCode(cond, kMainConds, sizeof(kMainConds) / sizeof(kMainConds[0]), st, true);
            if (code < 0) return;
            emitBranch(st, fr, sc, MB_COND, code, target, code == 0, OpKind::MBr, false, safe);
            return;
        }
        // Peri: BR.<COND> label | BR cond, label | JMP label
        if (mn == "JMP") {
            if (a.pos.size() != 1) { err(st.loc, "E102", "JMP label [, safe]"); return; }
            emitBranch(st, fr, sc, 0, 0, a.pos[0], true, OpKind::PBr, false, safe);
            return;
        }
        std::string cond;
        if (mn.compare(0, 3, "BR.") == 0) {
            cond = mn.substr(3);
            if (!pick(a, "target", 0, target)) { err(st.loc, "E102", mn + " label"); return; }
        } else if (!pick(a, "cond", 0, cond) || !pick(a, "target", 1, target)) {
            err(st.loc, "E102", "BR condition, label");
            return;
        }
        int code = condCode(cond, kPeriConds, sizeof(kPeriConds) / sizeof(kPeriConds[0]), st, false);
        if (code < 0) return;
        emitBranch(st, fr, sc, code, 0, target, code == 0, OpKind::PBr, false, safe);
    }

    // ---- Peri EXEC_PUB / EXEC_XFER -------------------------------------------------------------------------------------
    void doPub(const Stmt& st, const std::vector<Frame>& fr) {
        Args a = parseArgs(st.rest);
        std::string sv, src, len;
        if (!pick(a, "service", 0, sv) || !pick(a, "src", 1, src) || !pick(a, "len", 2, len)) { err(st.loc, "E102", "EXEC_PUB service=SID, src=N, len=L"); return; }
        int64_t s, p, l;
        if (!value(sv, 1, 15, "service_id", st, s) || !value(src, 0, 31, "src_ptr", st, p) || !value(len, 1, 32, "length", st, l)) return;
        if (p + l > 32) { err(st.loc, "E206", "src + len exceeds PERI_RX_BUF (32 bytes)"); return; }
        Op o = make(OpKind::Pub, st, fr);
        codec::Pub pb;
        pb.service = (int)s; pb.src = (int)p; pb.len = (int)l;
        o.desc = codec::pack(pb);
        ops.push_back(o);
    }

    void doXfer(const Stmt& st, const std::vector<Frame>& fr) {
        std::string body = trim(st.rest);
        size_t sp = 0;
        while (sp < body.size() && !std::isspace((unsigned char)body[sp]) && body[sp] != ',') sp++;
        if (body.empty()) { err(st.loc, "E102", "EXEC_XFER READ|WRITE dev=D ..."); return; }
        std::string dir = upper(body.substr(0, sp));
        Args a = parseArgs(trim(body.substr(sp)).size() && trim(body.substr(sp))[0] == ',' ? trim(trim(body.substr(sp)).substr(1)) : trim(body.substr(sp)));
        if (dir != "READ" && dir != "WRITE") { err(st.loc, "E206", "EXEC_XFER direction must be READ or WRITE"); return; }
        bool rd = dir == "READ";
        std::string t;
        int64_t dev = 0;
        if (!pick(a, "dev", 99, t)) { err(st.loc, "E102", "EXEC_XFER needs dev=<DeviceMap index>"); return; }
        if (!value(t, 0, 7, "dev_sel", st, dev)) return;
        int64_t addr = 0, addrLen = 0;
        bool hasAddr = pick(a, "addr", 99, t);
        if (hasAddr && !value(t, 0, 0xFFFF, "addr", st, addr)) return;
        bool hasAddrLen = pick(a, "addrlen", 99, t);
        if (hasAddrLen && !value(t, 0, 2, "addrlen", st, addrLen)) return;
        bool multi = a.named.count("len") != 0;
        if (!multi) {
            if (hasAddr && !hasAddrLen) addrLen = addr > 0xFF ? 2 : 1;
            if (addrLen == 0 && addr != 0) { err(st.loc, "E206", "addr given but addrlen is 0"); return; }
            if (addr > 0x3FF) { err(st.loc, "E206", "OneByte xfer_addr is 10 bits; use the MultiByte form (len=..., ext=[...]) for larger addresses"); return; }
            Op o = make(OpKind::Xfer1, st, fr);
            o.dev = (int)dev;
            codec::Xfer1 x1;
            x1.rw = rd ? 1 : 0; x1.dev = (int)dev; x1.addrLen = (int)addrLen; x1.addr = (uint32_t)(addr & 0x3FF);
            o.desc = codec::pack(x1);
            int64_t v = 0;
            if (rd) {
                if (!pick(a, "ptr", 99, t) || !value(t, 0, 31, "ptr", st, v)) { if (!bad) err(st.loc, "E102", "OneByte READ needs ptr=N (0..31)"); return; }
            } else {
                if (!pick(a, "data", 99, t) || !value(t, -128, 255, "data", st, v)) { if (!bad) err(st.loc, "E102", "OneByte WRITE needs data=V"); return; }
            }
            o.extra.push_back((uint8_t)(v & 0xFF));
            ops.push_back(o);
            return;
        }
        if (!rd) { err(st.loc, "E206", "MultiByte WRITE is illegal in the baseline (no PERI_TX_BUF, spec 8.5)"); return; }
        int64_t len, ptr;
        if (!pick(a, "len", 99, t) || !value(t, 1, 32, "length", st, len)) return;
        if (!pick(a, "ptr", 99, t) || !value(t, 0, 31, "ptr", st, ptr)) { if (!bad) err(st.loc, "E102", "MultiByte READ needs ptr=N"); return; }
        if (ptr + len > 32) { err(st.loc, "E206", "ptr + len exceeds PERI_RX_BUF (32 bytes)"); return; }
        std::vector<uint8_t> ext;
        if (pick(a, "ext", 99, t)) {
            std::string in = trim(t);
            if (in.size() < 2 || in.front() != '[' || in.back() != ']') { err(st.loc, "E102", "ext=[byte, byte]"); return; }
            for (const std::string& e : splitTop(in.substr(1, in.size() - 2))) {
                int64_t v;
                if (e.empty()) continue;
                if (!value(e, -128, 255, "external address byte", st, v)) return;
                ext.push_back((uint8_t)(v & 0xFF));
            }
        }
        if (ext.size() > 2) { err(st.loc, "E206", "at most 2 external address bytes (addr_len 0..2)"); return; }
        if (hasAddrLen && (size_t)addrLen != ext.size()) { err(st.loc, "E206", "addrlen=" + std::to_string(addrLen) + " does not match the number of ext bytes (" + std::to_string(ext.size()) + ")"); return; }
        if (hasAddr) { err(st.loc, "E206", "use ext=[...] for the MultiByte form; addr= is the OneByte immediate"); return; }
        Op o = make(OpKind::XferM, st, fr);
        o.dev = (int)dev;
        codec::XferM xm;
        xm.rw = 1; xm.dev = (int)dev; xm.addrLen = (int)ext.size(); xm.len = (int)len; xm.ptr = (int)ptr;
        o.desc = codec::pack(xm);
        o.extra = ext;
        ops.push_back(o);
    }

    // ---- one ordinary instruction ---------------------------------------------------------------------------------------
    void instr(const Stmt& st, const Scope& sc, const std::vector<Frame>& fr) {
        std::string mn = upper(st.op);
        bool mainD = dom == Domain::Main;
        if (mn == "MOV") { if (!mainD) err(st.loc, "E307", "Peri has no MOV (spec 1.1)"); else doMov(st, fr); }
        else if (mn == "ALU") { if (!mainD) err(st.loc, "E307", "Peri has no ALU (spec 1.1)"); else doAlu(st, fr); }
        else if (mn == "EXEC_CSR" || mn == "CSR") doCsr(st, fr);
        else if (mn == "EXEC_PUB") { if (mainD) err(st.loc, "E307", "EXEC_PUB is a Peri instruction"); else doPub(st, fr); }
        else if (mn == "EXEC_XFER") { if (mainD) err(st.loc, "E307", "EXEC_XFER is a Peri instruction"); else doXfer(st, fr); }
        else if (mn == "JMP" || mn == "BR" || mn.compare(0, 3, "BR.") == 0 || mn == "CASCADE_BEGIN" || mn == "CASCADE_END") {
            if (!mainD && (mn.find("CASCADE") != std::string::npos)) { err(st.loc, "E307", "BR.CASCADE is a Main instruction"); return; }
            doBranch(st, fr, sc, mn == "BR" ? "BR.COND" : mn);
        }
        else err(st.loc, "E101", "unknown instruction '" + st.op + "'");
    }

    // ---- CALL / RET --------------------------------------------------------------------------------------------------------
    const ProcDef* findProc(const std::string& n) const {
        for (const ProcDef& p : c.prog[domIdx(dom)].procs) if (p.name == n) return &p;
        return nullptr;
    }
    static bool isTransfer(const Stmt& s, bool mainD) {
        std::string m = upper(s.op);
        if (m == "RET" || m == "JMP") return true;
        if (m == "BR.COND" || m == "BR") {
            Args a = parseArgs(s.rest);
            std::string cond;
            if (!pick(a, "cond_sel", 0, cond)) return false;
            return upper(cond) == "ALWAYS" || cond == "0";
        }
        if (!mainD && m == "BR.ALWAYS") return true;
        return false;
    }
    static const Stmt* lastInstr(const std::vector<Stmt>& body) {
        for (size_t i = body.size(); i-- > 0;) if (!body[i].op.empty()) return &body[i];
        return nullptr;
    }

    void lowerSeq(const std::vector<Stmt>& seq, const Scope& sc, const std::vector<Frame>& fr, std::vector<std::string>& stack);
    void inlineCall(const Stmt& st, const Scope& sc, const std::vector<Frame>& fr, std::vector<std::string>& stack);
    void softCall(const Stmt& st, const Scope& sc, const std::vector<Frame>& fr);
    void softRet(const Stmt& st, const Scope& sc, const std::vector<Frame>& fr, bool isLast);
    void softDispatch(const ProcDef& p, const Stmt& anchor);
};

void Lowerer::lowerSeq(const std::vector<Stmt>& seq, const Scope& sc, const std::vector<Frame>& fr, std::vector<std::string>& stack) {
    const Stmt* last = lastInstr(seq);
    for (const Stmt& st : seq) {
        for (const std::string& l : st.labels) ops.push_back(label(scopedLabel(sc, l), st));
        if (st.op.empty()) continue;
        std::string mn = upper(st.op);
        if (mn == "CALL") {
            if (sc.soft) softCall(st, sc, fr);
            else inlineCall(st, sc, fr, stack);
        } else if (mn == "RET") {
            if (!sc.proc) { err(st.loc, "E305", "RET outside a procedure"); continue; }
            if (sc.soft) softRet(st, sc, fr, &st == last);
            else if (&st != last) emitBranch(st, fr, sc, 0, 0, sc.retLabel, true, dom == Domain::Main ? OpKind::MBr : OpKind::PBr, true);
        } else {
            instr(st, sc, fr);
        }
    }
}

void Lowerer::inlineCall(const Stmt& st, const Scope& sc, const std::vector<Frame>& fr, std::vector<std::string>& stack) {
    std::string callee = trim(st.rest);
    const ProcDef* p = findProc(callee);
    if (!p) { err(st.loc, "E305", "CALL to undefined procedure '" + callee + "'"); return; }
    if (std::find(stack.begin(), stack.end(), callee) != stack.end()) { err(st.loc, "E305", "recursive CALL of '" + callee + "' (the call graph must be acyclic: WCET and depth must be bounded)"); return; }
    int n = ++instance;
    int cs = ++callSeq;
    Scope ns;
    ns.proc = p;
    ns.prefix = callee + "#" + std::to_string(n) + ".";
    ns.retLabel = ns.prefix + "__ret";
    std::vector<Frame> nf = fr;
    nf.push_back(Frame{callee, st.loc});
    Ctx::PendingCall pc;
    pc.info.domain = dom;
    pc.info.callee = callee;
    pc.info.loc = st.loc;
    pc.info.caller = sc.proc ? sc.proc->name : "";
    pc.beginLabel = "__cs" + std::to_string(cs) + ".begin";
    pc.endLabel = "__cs" + std::to_string(cs) + ".end";
    c.pendingCalls.push_back(pc);
    ops.push_back(label(pc.beginLabel, st));
    stack.push_back(callee);
    lowerSeq(p->body, ns, nf, stack);
    stack.pop_back();
    ops.push_back(label(ns.retLabel, st));
    ops.push_back(label(pc.endLabel, st));
    (void)sc;
}

void Lowerer::softCall(const Stmt& st, const Scope& sc, const std::vector<Frame>& fr) {
    auto it = siteOf.find(&st);
    if (it == siteOf.end()) return;       // undefined callee: reported earlier
    const std::string& callee = it->second.first;
    int k = it->second.second;
    SoftProc& sp = soft[callee];
    int cs = ++callSeq;
    std::string retLabel = "__ret." + callee + "." + std::to_string(k);
    Ctx::PendingCall pc;
    pc.info.domain = dom;
    pc.info.callee = callee;
    pc.info.loc = st.loc;
    pc.info.caller = sc.proc ? sc.proc->name : "";
    pc.info.retId = sp.sites >= 2 ? k : 0;
    pc.beginLabel = "__cs" + std::to_string(cs) + ".xfer";
    pc.endLabel = retLabel;
    c.pendingCalls.push_back(pc);
    if (sp.sites >= 2) {
        // v31: the constant pool moves straight to the scratchpad, so storing the return id is ONE MOV: CONST(k) -> SCRATCHPAD[slot..slot+1]
        Stmt s1 = st;
        s1.op = "MOV";
        s1.rest = "CONST(" + std::to_string(k) + ", 2) -> MAIN_SCRATCHPAD[" + std::to_string(sp.slot) + ".." + std::to_string(sp.slot + 1) + "]";
        size_t before = ops.size();
        doMov(s1, fr, true);
        for (size_t i = before; i < ops.size(); i++) { ops[i].synthetic = true; ops[i].note = "CALL " + callee + ": store return id " + std::to_string(k); }
    }
    ops.push_back(label(pc.beginLabel, st));
    size_t before = ops.size();
    emitBranch(st, fr, sc, MB_COND, 0, callee, true, OpKind::MBr, true);
    ops[before].synthetic = true;
    ops[before].note = "CALL " + callee;
    ops[before].text = "BR.COND ALWAYS, " + callee;
    if (sp.sites >= 2) ops[before].pushLabel = retLabel;
    ops.push_back(label(retLabel, st));
}

void Lowerer::softRet(const Stmt& st, const Scope& sc, const std::vector<Frame>& fr, bool isLast) {
    SoftProc& sp = soft[sc.proc->name];
    if (sp.sites == 1) {
        emitBranch(st, fr, sc, MB_COND, 0, "__ret." + sc.proc->name + ".1", true, OpKind::MBr, true);
        ops.back().note = "RET (single call site)";
        return;
    }
    if (isLast) return;     // falls into the dispatch block that follows the body
    emitBranch(st, fr, sc, MB_COND, 0, sc.proc->name + ".__ret", true, OpKind::MBr, true);
    ops.back().note = "RET";
}

void Lowerer::softDispatch(const ProcDef& p, const Stmt& anchor) {
    SoftProc& sp = soft[p.name];
    if (sp.sites < 2) return;
    const Profile& pf = c.opt.profile;
    // the return id to compare with goes through two scratchpad bytes: the constant pool cannot be moved to ALU_OPB directly (v31 5.1)
    std::string stage = "MAIN_SCRATCHPAD[" + std::to_string(pf.retStageBase) + ".." + std::to_string(pf.retStageBase + 1) + "]";
    ops.push_back(label(p.name + ".__ret", anchor));
    size_t dispatchStart = ops.size();
    std::vector<Frame> none;
    auto addMov = [&](const std::string& text, const std::string& note) {
        Stmt s = anchor;
        s.op = "MOV";
        s.rest = text;
        size_t b = ops.size();
        doMov(s, none, true);
        for (size_t i = b; i < ops.size(); i++) { ops[i].synthetic = true; ops[i].note = note; }
    };
    addMov("MAIN_SCRATCHPAD[" + std::to_string(sp.slot) + ".." + std::to_string(sp.slot + 1) + "] -> ALU_OPA", "RET " + p.name + ": load return id");
    for (int k = 1; k < sp.sites; k++) {
        addMov("CONST(" + std::to_string(k) + ", 2) -> " + stage, "RET " + p.name + ": compare with " + std::to_string(k));
        addMov(stage + " -> ALU_OPB", "RET " + p.name + ": compare with " + std::to_string(k));
        Stmt a = anchor;
        a.op = "ALU";
        a.rest = "CMP.EQ unsigned";
        size_t b = ops.size();
        doAlu(a, none, true);
        for (size_t i = b; i < ops.size(); i++) { ops[i].synthetic = true; ops[i].note = "RET " + p.name + ": compare with " + std::to_string(k); }
        Stmt br = anchor;
        br.op = "BR.COND";
        br.rest = "ALU_CMP_TRUE, __ret." + p.name + "." + std::to_string(k);
        emitBranch(br, none, Scope(), MB_COND, 0x5, "__ret." + p.name + "." + std::to_string(k), false, OpKind::MBr, true);
        ops.back().text = "BR.COND ALU_CMP_TRUE, __ret." + p.name + "." + std::to_string(k);
        ops.back().note = "RET " + p.name + ": return to site " + std::to_string(k);
    }
    Stmt br = anchor;
    emitBranch(br, none, Scope(), MB_COND, 0, "__ret." + p.name + "." + std::to_string(sp.sites), true, OpKind::MBr, true);
    ops.back().text = "BR.COND ALWAYS, __ret." + p.name + "." + std::to_string(sp.sites);
    ops.back().note = "RET " + p.name + ": return to site " + std::to_string(sp.sites);
    for (size_t i = dispatchStart; i < ops.size(); i++) if (ops[i].kind != OpKind::Label) ops[i].dispatchOf = p.name;
}

}  // namespace

bool lowerDomain(Ctx& c, Domain d, std::vector<Op>& ops, std::vector<ProcInfo>& procsOut) {
    DomainProg& prog = c.prog[domIdx(d)];
    Lowerer lw(c, d, ops);
    bool mainD = d == Domain::Main;

    // ---- call graph checks -----------------------------------------------------------------------------------------------
    std::map<std::string, const ProcDef*> byName;
    for (const ProcDef& p : prog.procs) byName[p.name] = &p;
    auto calleesOf = [&](const std::vector<Stmt>& body) {
        std::vector<std::pair<std::string, const Stmt*>> v;
        for (const Stmt& s : body) if (upper(s.op) == "CALL") v.push_back({trim(s.rest), &s});
        return v;
    };
    for (const ProcDef& p : prog.procs) {
        for (auto& cs : calleesOf(p.body)) if (!byName.count(cs.first)) lw.err(cs.second->loc, "E305", "CALL to undefined procedure '" + cs.first + "'");
        const Stmt* last = Lowerer::lastInstr(p.body);
        if (!last) c.warn(p.loc, "W401", "procedure '" + p.name + "' is empty");
        else if (!Lowerer::isTransfer(*last, mainD)) lw.err(last->loc, "E305", "procedure '" + p.name + "' can run off its end: finish it with RET or an unconditional branch");
    }
    for (auto& cs : calleesOf(prog.flow)) if (!byName.count(cs.first)) lw.err(cs.second->loc, "E305", "CALL to undefined procedure '" + cs.first + "'");

    // reachability from the flow, recursion, depth
    std::set<std::string> used, onStack;
    int maxDepth = 0;
    std::function<void(const std::string&, int)> dfs = [&](const std::string& n, int depth) {
        maxDepth = std::max(maxDepth, depth);
        used.insert(n);
        auto it = byName.find(n);
        if (it == byName.end()) return;
        onStack.insert(n);
        for (auto& cs : calleesOf(it->second->body)) {
            if (!byName.count(cs.first)) continue;
            if (onStack.count(cs.first)) { lw.err(cs.second->loc, "E305", "recursive CALL of '" + cs.first + "' (the call graph must be acyclic: depth and WCET must be bounded)"); continue; }
            dfs(cs.first, depth + 1);
        }
        onStack.erase(n);
    };
    for (auto& cs : calleesOf(prog.flow)) if (byName.count(cs.first)) dfs(cs.first, 1);
    c.res.maxCallDepth = std::max(c.res.maxCallDepth, maxDepth);
    for (const ProcDef& p : prog.procs) if (!used.count(p.name)) c.warn(p.loc, "W401", "procedure '" + p.name + "' is never called; no code is generated for it");
    if (lw.bad) return false;

    // branches into a procedure from outside are not allowed (use CALL)
    {
        std::set<std::string> procNameSet;
        for (const ProcDef& p : prog.procs) procNameSet.insert(p.name);
        auto scanBr = [&](const std::vector<Stmt>& body, const ProcDef* owner) {
            for (const Stmt& s : body) {
                std::string m = upper(s.op);
                if (m == "JMP" || m == "BR" || m.compare(0, 3, "BR.") == 0 || m == "CASCADE_BEGIN" || m == "CASCADE_END") {
                    Args a = parseArgs(s.rest);
                    std::string t = a.pos.empty() ? "" : a.pos.back();
                    if (a.named.count("target")) t = a.named["target"];
                    if (!t.empty() && procNameSet.count(t) && !(owner && owner->name == t)) lw.err(s.loc, "E305", "'" + t + "' is a procedure: enter it with CALL, not a branch");
                }
            }
        };
        scanBr(prog.flow, nullptr);
        for (const ProcDef& p : prog.procs) scanBr(p.body, &p);
        if (lw.bad) return false;
    }

    bool softMode = c.opt.callModel == CallModel::SoftRet;
    if (softMode && !mainD) {
        if (!prog.procs.empty()) c.warn(prog.procs[0].loc, "W403", "SoftRet needs MOV and ALU, which Peri does not have: Peri procedures are always expanded inline");
        softMode = false;
    }

    std::vector<std::string> stack;
    Scope top;
    top.soft = softMode;
    if (!softMode) {
        lw.lowerSeq(prog.flow, top, {}, stack);
        for (const ProcDef& p : prog.procs) {
            ProcInfo pi;
            pi.domain = d;
            pi.name = p.name;
            pi.loc = p.loc;
            pi.emitted = false;
            for (auto& cs : c.pendingCalls) if (cs.info.domain == d && cs.info.callee == p.name) pi.callSites++;
            procsOut.push_back(pi);
        }
        return !lw.bad;
    }

    // ---- SoftRet -----------------------------------------------------------------------------------------------------------
    // number the call sites: flow first, then the used procedures in source order
    std::vector<const ProcDef*> emitOrder;
    for (const ProcDef& p : prog.procs) if (used.count(p.name)) emitOrder.push_back(&p);
    auto number = [&](const std::vector<Stmt>& body) {
        for (const Stmt& s : body) if (upper(s.op) == "CALL") {
            std::string callee = trim(s.rest);
            if (!byName.count(callee)) continue;
            int k = ++lw.soft[callee].sites;
            lw.siteOf[&s] = {callee, k};
        }
    };
    number(prog.flow);
    for (const ProcDef* p : emitOrder) number(p->body);
    const Profile& pf = c.opt.profile;
    int nextSlot = pf.retSlotBase;
    int slotBytes = 0;
    for (const ProcDef* p : emitOrder) {
        auto& sp = lw.soft[p->name];
        sp.used = true;
        if (sp.sites >= 2) {
            sp.slot = nextSlot;
            nextSlot += 2;
            slotBytes += 2;
        }
    }
    if (nextSlot > 32) { c.error(prog.procs.empty() ? SrcLoc{} : prog.procs[0].loc, "E306", "SoftRet needs " + std::to_string(slotBytes) + " bytes of return slots starting at MAIN_SCRATCHPAD[" + std::to_string(pf.retSlotBase) + "]; only " + std::to_string(32 - pf.retSlotBase) + " are left (lower ret_slot_base or call fewer procedures from several places)"); return false; }
    if (slotBytes && pf.retStageBase + 2 > 32) { c.error(SrcLoc{}, "E306", "ret_stage_base leaves no room for a 2-byte staging area in MAIN_SCRATCHPAD (use 0..30)"); return false; }
    if (slotBytes && pf.retStageBase < nextSlot && pf.retSlotBase < pf.retStageBase + 2)
        { c.error(SrcLoc{}, "E306", "the SoftRet staging bytes MAIN_SCRATCHPAD[" + std::to_string(pf.retStageBase) + ".." + std::to_string(pf.retStageBase + 1) + "] overlap the return slots [" + std::to_string(pf.retSlotBase) + ".." + std::to_string(nextSlot - 1) + "]: move ret_stage_base (profile) or call fewer procedures from several places"); return false; }
    lw.lowerSeq(prog.flow, top, {}, stack);
    for (const ProcDef* p : emitOrder) {
        auto& sp = lw.soft[p->name];
        Stmt anchor;
        anchor.loc = p->loc;
        Ctx::PendingProc pp;
        pp.info.domain = d;
        pp.info.name = p->name;
        pp.info.loc = p->loc;
        pp.info.emitted = true;
        pp.info.callSites = sp.sites;
        pp.info.slot = sp.slot;
        pp.beginLabel = p->name;
        pp.endLabel = "__procend." + p->name;
        c.pendingProcs.push_back(pp);
        lw.ops.push_back(lw.label(p->name, anchor));
        Scope ps;
        ps.proc = p;
        ps.prefix = p->name + ".";
        ps.soft = true;
        lw.lowerSeq(p->body, ps, {}, stack);
        lw.softDispatch(*p, anchor);
        lw.ops.push_back(lw.label(pp.endLabel, anchor));
    }
    for (const ProcDef& p : prog.procs) if (!used.count(p.name)) {
        ProcInfo pi;
        pi.domain = d;
        pi.name = p.name;
        pi.loc = p.loc;
        procsOut.push_back(pi);
    }
    return !lw.bad;
}

}  // namespace asmcore
