// Disassembler. See include/asmcore/disasm.h.
#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

#include "asmcore/disasm.h"
#include "codec.h"
#include "image_view.h"
#include "isa.h"
#include "util.h"

namespace asmcore {

namespace {

using namespace isa;

// ImageLayout table indices (spec 3.1 order), named as the decoders use them
const int MT_CSR1_ = MT_CSR1, MT_CSRM_ = MT_CSRM, MT_BRANCH_ = MT_BRANCH, MT_MOVE_ = MT_MOVE, MT_ALU_ = MT_ALU;
const int PT_CSR1_ = PT_CSR1, PT_BRANCH_ = PT_BRANCH, PT_PUB_ = PT_PUB, PT_XFER1_ = PT_XFER1, PT_XFERM_ = PT_XFERM;

std::string hx(uint32_t v, int digits = 2) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%0*X", digits, v);
    return b;
}
std::string num(int v) { return std::to_string(v); }

DecodedInstr invalid(uint32_t length, const std::string& what, const std::string& problem) {
    DecodedInstr d;
    d.length = length;
    d.valid = false;
    d.text = "; " + what;
    d.problem = problem;
    return d;
}
DecodedInstr plain(uint32_t length, const std::string& text) {
    DecodedInstr d;
    d.length = length;
    d.text = text;
    return d;
}

std::string regionText(int id, int off, int len) {
    const char* name = kRegions[id].name;
    if (id == R_OPA || id == R_OPB) return name;
    if (len == 1) return std::string(name) + "[" + num(off) + "]";
    return std::string(name) + "[" + num(off) + ".." + num(off + len - 1) + "]";
}

// ---- Main -------------------------------------------------------------------------------------------------------------------------
DecodedInstr mainCsr(const DecodeContext& c) {
    const bool multi = codec::isMulti(c.arg);
    uint32_t d = 0;
    if (!c.entry(multi ? MT_CSRM_ : MT_CSR1_, (uint32_t)codec::templateIndex(c.arg), 2, d))
        return invalid(2, "EXEC_CSR", "descriptor " + num(codec::templateIndex(c.arg)) + " is outside its table");
    if (c.pc + 1 >= c.codeLimit) return invalid(1, "EXEC_CSR", "the instruction is cut off by the end of the code");
    const uint32_t imm = c.byteAt(c.pc + 1);
    if (!multi) {
        codec::Csr1 x = codec::unpackCsr1(d);
        if (x.rsv) return invalid(2, "EXEC_CSR", "CsrOneByteDesc reserved bits are not zero");
        static const char* nm[] = {"WRITE8", "SET8", "CLR8"};
        if (x.op == CSR1_READ8) {
            if (imm & 0xE0) return invalid(2, "EXEC_CSR READ8", "destination pointer has bits 7:5 set");
            return plain(2, "EXEC_CSR READ8 " + hx(x.addr, 3) + " -> MAIN_CSR_DATA_BUF[" + num((int)imm) + "]");
        }
        return plain(2, std::string("EXEC_CSR ") + nm[x.op] + " " + hx(x.addr, 3) + ", " + hx(imm));
    }
    codec::CsrM x = codec::unpackCsrM(d);
    if (x.op != CSRM_WRITE_SP && x.op != CSRM_READ_BUF) return invalid(2, "EXEC_CSR", "CsrMultiByte csr_op " + num(x.op) + " is reserved");
    if (x.ptr + x.len > 32) return invalid(2, "EXEC_CSR", std::string("ptr + len exceeds ") + (x.op == CSRM_WRITE_SP ? "MAIN_SCRATCHPAD" : "MAIN_CSR_DATA_BUF"));
    return plain(2, std::string("EXEC_CSR ") + (x.op == CSRM_WRITE_SP ? "WRITE_SP " : "READ_BUF ") + hx(((uint32_t)x.page << 8) | imm, 3) + ", len=" + num(x.len) + ", ptr=" + num(x.ptr));
}

std::string mainCondName(int code) {
    for (const NamedCode& n : kMainConds) if (n.code == code) return n.name;
    return "";
}

DecodedInstr mainBr(const DecodeContext& c) {
    uint32_t d = 0;
    if (!c.entry(MT_BRANCH_, (uint32_t)c.arg, 2, d)) return invalid(1, "BR", "descriptor " + num(c.arg) + " is outside its table");
    codec::MainBranch b = codec::unpackMainBranch(d);
    if (b.rsv) return invalid(1, "BR", "MainBranchDesc reserved bits are not zero");
    DecodedInstr r;
    r.branch = true;
    r.target = b.target;
    const std::string safe = b.eventSafe ? ", safe" : "";
    if (b.type == MB_ERET) {                                      // v31 16.5: arg = 0, event_safe = 0, target = 0
        if (b.arg || b.eventSafe || b.target) return invalid(1, "BR.ERET", "a BR.ERET descriptor must have arg, event_safe and target_pc_byte all 0");
        r.branch = false;                                         // no static target: it returns to the saved resume PC
        r.uncond = true;
        r.eret = true;
        r.text = "BR.ERET";
        return r;
    }
    if (b.type == MB_COND) {
        std::string n = mainCondName(b.arg);
        if (n.empty()) return invalid(1, "BR.COND", "condition " + hx((uint32_t)b.arg, 1) + " is reserved");
        r.uncond = b.arg == 0;
        r.text = b.arg == 0 ? "JMP {L}" + safe : "BR.COND " + n + ", {L}" + safe;
        return r;
    }
    if (b.eventSafe) return invalid(1, "BR", "event_safe is set on a CASCADE descriptor (only an ordinary BR.COND may be an event checkpoint)");
    if (b.type == MB_CASCADE_BEGIN) { r.text = "CASCADE_BEGIN " + num(b.arg) + ", {L}"; return r; }
    return [&] { r.text = "CASCADE_END " + num(b.arg) + ", {L}"; return r; }();
}

DecodedInstr mainMov(const DecodeContext& c) {
    uint32_t d = 0;
    if (!c.entry(MT_MOVE_, (uint32_t)c.arg, 4, d)) return invalid(1, "MOV", "descriptor " + num(c.arg) + " is outside its table");
    codec::Move m = codec::unpackMove(d);
    if (m.rsv) return invalid(1, "MOV", "MainMoveDesc reserved bits are not zero");
    if (m.srcRegion > R_META || m.dstRegion > R_META) return invalid(1, "MOV", "unknown region id");
    if ((m.dstRegion == R_OPA || m.dstRegion == R_OPB) && (m.dstOff != 0 || m.len != 2)) return invalid(1, "MOV", "ALU operands are loaded with offset 0 and length 2");
    if (m.srcRegion == R_OPA || m.srcRegion == R_OPB) return invalid(1, "MOV", "ALU_OPA / ALU_OPB cannot be a source");
    if (m.dstRegion == R_CSRBUF) return invalid(1, "MOV", "MAIN_CSR_DATA_BUF is read-only for MOV (spec v31 5.1)");
    if (m.srcRegion == R_CONST && m.dstRegion != R_SCRATCH) return invalid(1, "MOV", "MAIN_CONST_POOL can only be moved to MAIN_SCRATCHPAD (spec v31 5.1)");
    return plain(1, "MOV " + regionText(m.srcRegion, m.srcOff, m.len) + " -> " + regionText(m.dstRegion, m.dstOff, m.len));
}

DecodedInstr mainAlu(const DecodeContext& c) {
    uint32_t d = 0;
    if (!c.entry(MT_ALU_, (uint32_t)c.arg, 1, d)) return invalid(1, "ALU", "descriptor " + num(c.arg) + " is outside its table");
    const AluCodes defCodes;
    codec::Alu a = codec::unpackAlu(d, c.alu ? *c.alu : defCodes);
    if (a.cls != 3 && a.op < 0) return invalid(1, "ALU", "the numeric sub-operation code is not assigned by the target profile (reserved encoding)");
    const char* sg = a.signedMode ? " signed" : " unsigned";
    switch (a.cls) {
        case A_ARITH: {
            static const char* nm[] = {"ADD", "SUB", "MUL", "INC", "DEC"};
            if (a.op > 4) return invalid(1, "ALU", "arith_op " + num(a.op) + " is reserved");
            return plain(1, std::string("ALU ") + nm[a.op] + sg);
        }
        case A_CMP: {
            static const char* nm[] = {"EQ", "NE", "LT", "GE"};
            if (a.rsv) return invalid(1, "ALU", "CMP descriptor reserved bits are not zero");
            if (a.op <= 1 && a.signedMode) return invalid(1, "ALU", "EQ / NE require signed_mode = 0");
            return plain(1, std::string("ALU CMP.") + nm[a.op] + sg);
        }
        case A_LOGIC: {
            static const char* nm[] = {"AND", "OR", "XOR", "NOT"};
            if (a.rsv) return invalid(1, "ALU", "LOGIC descriptor reserved bits are not zero");
            if (a.op > 3) return invalid(1, "ALU", "logic_op " + num(a.op) + " is reserved");
            return plain(1, std::string("ALU ") + nm[a.op]);
        }
        default:
            if (a.amt < 1 || a.amt > 8) return invalid(1, "ALU", "shift amount " + num(a.amt) + " is outside 1..8");
            return plain(1, std::string("ALU ") + (a.dir ? "SHR " : "SHL ") + num(a.amt) + sg);
    }
}

// ---- Peri -------------------------------------------------------------------------------------------------------------------------
DecodedInstr periCsr(const DecodeContext& c) {
    if (codec::isMulti(c.arg)) return invalid(2, "EXEC_CSR", "Peri EXEC_CSR MultiByte is illegal in the baseline");
    uint32_t d = 0;
    if (!c.entry(PT_CSR1_, (uint32_t)codec::templateIndex(c.arg), 2, d)) return invalid(2, "EXEC_CSR", "descriptor " + num(codec::templateIndex(c.arg)) + " is outside its table");
    if (c.pc + 1 >= c.codeLimit) return invalid(1, "EXEC_CSR", "the instruction is cut off by the end of the code");
    codec::Csr1 x = codec::unpackCsr1(d);
    if (x.rsv) return invalid(2, "EXEC_CSR", "CsrOneByteDesc reserved bits are not zero");
    if (x.op == CSR1_READ8) return invalid(2, "EXEC_CSR", "Peri READ8 is illegal in the baseline");
    static const char* nm[] = {"WRITE8", "SET8", "CLR8"};
    return plain(2, std::string("EXEC_CSR ") + nm[x.op] + " " + hx(x.addr, 3) + ", " + hx(c.byteAt(c.pc + 1)));
}

DecodedInstr periBr(const DecodeContext& c) {
    uint32_t d = 0;
    if (!c.entry(PT_BRANCH_, (uint32_t)c.arg, 2, d)) return invalid(1, "BR", "descriptor " + num(c.arg) + " is outside its table");
    codec::PeriBranch b = codec::unpackPeriBranch(d);
    if (b.rsv) return invalid(1, "BR", "PeriBranchDesc reserved bits are not zero");
    DecodedInstr r;
    if (b.type == PB_ERET) {
        if (b.eventSafe || b.target) return invalid(1, "BR.ERET", "a BR.ERET descriptor must have event_safe and target_pc_byte both 0");
        r.uncond = true;
        r.eret = true;
        r.text = "BR.ERET";
        return r;
    }
    if (b.type > 7) return invalid(1, "BR", "condition " + num(b.type) + " is reserved");
    r.branch = true;
    r.target = b.target;
    r.uncond = b.type == 0;
    const std::string safe = b.eventSafe ? ", safe" : "";
    r.text = b.type == 0 ? "JMP {L}" + safe : std::string("BR.") + kPeriConds[b.type].name + " {L}" + safe;
    return r;
}

DecodedInstr periPub(const DecodeContext& c) {
    uint32_t d = 0;
    if (!c.entry(PT_PUB_, (uint32_t)c.arg, 2, d)) return invalid(1, "EXEC_PUB", "descriptor " + num(c.arg) + " is outside its table");
    codec::Pub p = codec::unpackPub(d);
    if (p.rsv) return invalid(1, "EXEC_PUB", "PeriPublishDesc reserved bits are not zero");
    if (p.service == 0) return invalid(1, "EXEC_PUB", "service_id 0 is reserved");
    if (p.src + p.len > 32) return invalid(1, "EXEC_PUB", "src + len exceeds PERI_RX_BUF");
    return plain(1, "EXEC_PUB service=" + num(p.service) + ", src=" + num(p.src) + ", len=" + num(p.len));
}

DecodedInstr periXfer(const DecodeContext& c) {
    const bool multi = codec::isMulti(c.arg);
    uint32_t d = 0;
    if (!c.entry(multi ? PT_XFERM_ : PT_XFER1_, (uint32_t)codec::templateIndex(c.arg), 2, d)) return invalid(1, "EXEC_XFER", "descriptor " + num(codec::templateIndex(c.arg)) + " is outside its table");
    if (!multi) {
        codec::Xfer1 x = codec::unpackXfer1(d);
        if (c.pc + 1 >= c.codeLimit) return invalid(1, "EXEC_XFER", "the instruction is cut off by the end of the code");
        if (x.addrLen == 3) return invalid(2, "EXEC_XFER", "addr_len 3 is reserved");
        uint32_t b1 = c.byteAt(c.pc + 1);
        std::string t = std::string("EXEC_XFER ") + (x.rw ? "READ" : "WRITE") + " dev=" + num(x.dev);
        if (x.addrLen > 0) t += ", addr=" + hx(x.addr, 3) + ", addrlen=" + num(x.addrLen);
        else t += ", addrlen=0";
        if (x.rw) {
            if (b1 & 0xE0) return invalid(2, "EXEC_XFER", "READ destination pointer has bits 7:5 set");
            t += ", ptr=" + num((int)b1);
        } else t += ", data=" + hx(b1);
        DecodedInstr r = plain(2, t);
        if (x.addrLen == 0 && x.addr != 0) r.problem = "xfer_addr is ignored when addr_len = 0 but is not zero; the text drops it";
        return r;
    }
    codec::XferM x = codec::unpackXferM(d);
    if (!x.rw) return invalid(1 + (uint32_t)(x.addrLen & 3), "EXEC_XFER", "MultiByte WRITE is illegal in the baseline");
    if (x.addrLen == 3) return invalid(1, "EXEC_XFER", "addr_len 3 is reserved");
    if (x.ptr + x.len > 32) return invalid(1 + (uint32_t)x.addrLen, "EXEC_XFER", "ptr + len exceeds PERI_RX_BUF");
    const uint32_t length = 1 + (uint32_t)x.addrLen;
    if (c.pc + length > c.codeLimit) return invalid(1, "EXEC_XFER", "the address bytes are cut off by the end of the code");
    std::string t = "EXEC_XFER READ dev=" + num(x.dev) + ", len=" + num(x.len) + ", ptr=" + num(x.ptr);
    if (x.addrLen > 0) {
        t += ", ext=[";
        for (int i = 0; i < x.addrLen; i++) t += (i ? ", " : "") + hx(c.byteAt(c.pc + 1 + (uint32_t)i));
        t += "]";
    }
    return plain(length, t);
}

}  // namespace

DecoderSet DecoderSet::defaults() {
    DecoderSet s;
    s.main[M_CSR] = mainCsr;
    s.main[M_BR] = mainBr;
    s.main[M_MOV] = mainMov;
    s.main[M_ALU] = mainAlu;
    s.peri[P_CSR] = periCsr;
    s.peri[P_BR] = periBr;
    s.peri[P_PUB] = periPub;
    s.peri[P_XFER] = periXfer;
    return s;
}

std::map<uint32_t, std::string> labelNames(const Result& r, Domain d) {
    std::map<uint32_t, std::string> m;
    const std::string prefix = d == Domain::Main ? "main:" : "peri:";
    for (const SymbolInfo& s : r.symbols) {
        if (s.kind != "label" || s.name.compare(0, prefix.size(), prefix) != 0) continue;
        std::string n = s.name.substr(prefix.size());
        if (!isIdent(n) || m.count((uint32_t)s.value)) continue;
        m[(uint32_t)s.value] = n;
    }
    return m;
}

DisasmResult disassemble(const DomainImage& image, const Profile& pf, const DisasmOptions& opt) {
    DisasmResult res;
    res.domain = image.domain;
    const bool mainD = image.domain == Domain::Main;
    auto issue = [&](uint32_t pc, const char* code, const std::string& msg, bool fatal = true) {
        DisasmIssue i;
        i.pc = pc; i.code = code; i.msg = msg; i.fatal = fatal;
        res.issues.push_back(i);
    };
    ImageView iv;
    std::string err;
    if (!image.present) { issue(0, "D101", "the domain has no image"); return res; }
    if (!iv.load(image, pf, err)) { issue(0, "D102", err); return res; }
    const DecoderSet set = opt.decoders ? *opt.decoders : DecoderSet::defaults();

    // descriptor entries the decoders touch (to report entries nothing uses)
    std::set<std::pair<int, uint32_t>> used;
    DecodeContext base;
    base.domain = image.domain;
    base.alu = &pf.alu;
    base.byteAt = [&](uint32_t p) { return iv.u8(p); };
    base.entry = [&](int t, uint32_t idx, int eb, uint32_t& v) {
        bool ok = iv.entry(t, idx, eb, v);
        if (ok) used.insert({t, idx});
        return ok;
    };
    base.deviceMap = [&](uint32_t dev, uint32_t& v) {
        bool ok = iv.entry(PT_DEVMAP, dev, pf.deviceMapEntryBytes, v);
        if (ok) used.insert({PT_DEVMAP, dev});
        return ok;
    };

    // every branch target named by the BranchDesc table: tells where the code really ends
    std::set<uint32_t> targets;
    {
        const int bt = mainD ? (int)MT_BRANCH : (int)PT_BRANCH;
        for (uint32_t i = 0; iv.off[bt] + (i + 1) * 2 <= iv.end[bt]; i++) {
            uint32_t d = 0;
            if (!iv.entry(bt, i, 2, d)) break;
            if (mainD ? codec::unpackMainBranch(d).type == MB_ERET : codec::unpackPeriBranch(d).type == PB_ERET) continue;     // an ERET has no target
            targets.insert(mainD ? codec::unpackMainBranch(d).target : codec::unpackPeriBranch(d).target);
        }
    }

    const bool exact = opt.codeSize != 0 || image.codeSize != 0;
    // never read past the image: a code size larger than the image (truncated file, wrong size) must not make the sweep run on
    const uint32_t imgSize = (uint32_t)iv.bytes.size();
    const uint32_t limit = std::min<uint32_t>(opt.codeSize ? opt.codeSize : iv.codeLimit, imgSize);
    base.codeLimit = limit;
    uint32_t pc = 0;
    std::set<uint32_t> starts;
    std::vector<DecodedInstr> decoded;            // parallel to res.lines
    while (pc < limit) {
        DecodeContext c = base;
        c.pc = pc;
        const uint8_t b = (uint8_t)iv.u8(pc);
        c.opcode = codec::opcode(b);
        c.arg = codec::arg(b);
        const DecodeFn& fn = mainD ? set.main[c.opcode] : set.peri[c.opcode];
        DecodedInstr di = fn ? fn(c) : invalid(1, "opcode", "no decoder for opcode " + num(c.opcode));
        if (di.length == 0) di.length = 1;
        DisasmLine ln;
        ln.pc = pc;
        ln.length = std::max<uint32_t>(1, std::min<uint32_t>(di.length, imgSize - pc));      // pc < limit <= imgSize: always advances
        for (uint32_t k = 0; k < ln.length; k++) ln.bytes.push_back((uint8_t)iv.u8(pc + k));
        ln.text = di.text;
        ln.valid = di.valid;
        starts.insert(pc);
        if (!di.valid) issue(pc, "D201", di.problem);
        else if (!di.problem.empty()) issue(pc, "D202", di.problem, false);
        res.lines.push_back(ln);
        decoded.push_back(di);
        pc += ln.length;
        if (!exact && di.valid && di.uncond && !targets.count(pc)) {
            // nothing branches to the next byte; the code still goes on if what lies between here and the first table is not just padding
            // (code after the main flow, e.g. event handlers that only a vector enters). Padding that happens to be the pad byte is the
            // limit of this heuristic: give the exact size (layout.json "code" section, --code-size) when it matters.
            const uint32_t firstTable = std::min(std::min(std::min(iv.off[0], iv.off[1]), std::min(iv.off[2], iv.off[3])), std::min(iv.off[4], iv.off[5]));
            bool more = false;
            for (uint32_t q = pc; q < firstTable && q < imgSize; q++) if ((uint8_t)iv.u8(q) != (uint8_t)pf.bootPadByte) more = true;
            if (!more) break;
        }
    }
    res.codeSize = exact ? limit : pc;

    // labels: branch targets, plus the names the caller knows
    std::set<uint32_t> labelPcs;
    std::map<uint32_t, std::string> labelText;
    for (size_t i = 0; i < res.lines.size(); i++) {
        DisasmLine& ln = res.lines[i];
        if (!decoded[i].branch) continue;
        const uint32_t t = decoded[i].target;
        if (!starts.count(t)) { issue(ln.pc, "D203", "branch target " + hx(t, 4) + " is not the start of an instruction"); ln.valid = false; ln.text = "; " + ln.text; decoded[i].branch = false; continue; }
        labelPcs.insert(t);
    }
    if (opt.names) for (auto& kv : *opt.names) if (starts.count(kv.first)) labelPcs.insert(kv.first);
    std::vector<std::pair<int, uint32_t>> vecs;
    for (auto& v : opt.vectors) {
        if (!starts.count(v.second)) { issue(v.second, "D204", "event " + num(v.first) + " has its vector at " + hx(v.second, 4) + ", which is not the start of an instruction"); continue; }
        labelPcs.insert(v.second);
        vecs.push_back(v);
    }
    for (uint32_t p : labelPcs) {
        char b[16];
        std::snprintf(b, sizeof b, "%04X", p);
        auto it = opt.names ? opt.names->find(p) : std::map<uint32_t, std::string>::const_iterator();
        labelText[p] = (opt.names && it != opt.names->end()) ? it->second : opt.labelPrefix + b;
    }
    // substitute {L}, attach labels
    for (size_t i = 0; i < res.lines.size(); i++) {
        DisasmLine& ln = res.lines[i];
        size_t at = ln.text.find("{L}");
        if (at != std::string::npos) {
            auto lt = labelText.find(decoded[i].target);
            ln.text.replace(at, 3, lt != labelText.end() ? lt->second : hx(decoded[i].target, 4));
        }
        auto lb = labelText.find(ln.pc);
        if (lb != labelText.end()) ln.label = lb->second;
        if (opt.useJmp == false && ln.text.compare(0, 4, "JMP ") == 0) ln.text = (mainD ? "BR.COND ALWAYS, " : "BR.ALWAYS ") + ln.text.substr(4);
        if (opt.commentHook) ln.comment = opt.commentHook(ln.pc, ln.text);
    }

    // definitions that the instructions refer to
    for (auto& v : vecs) res.definitions.push_back(".vector " + num(v.first) + ", " + labelText[v.second]);
    if (mainD) {
        std::string pool;
        uint32_t n = iv.end[MT_CONST] > iv.off[MT_CONST] ? iv.end[MT_CONST] - iv.off[MT_CONST] : 0;
        for (uint32_t i = 0; i < n; i++) pool += (i ? ", " : "") + hx(iv.u8(iv.off[MT_CONST] + i));
        if (n) res.definitions.push_back(".const __pool db " + pool);
    } else {
        const uint32_t eb = (uint32_t)pf.deviceMapEntryBytes;
        uint32_t n = (iv.end[PT_DEVMAP] > iv.off[PT_DEVMAP] ? iv.end[PT_DEVMAP] - iv.off[PT_DEVMAP] : 0) / eb;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t raw = 0;
            iv.entry(PT_DEVMAP, i, (int)eb, raw);
            codec::DevEntry e = codec::unpackDev(pf, raw);
            if (e.busType != 0) { issue(0, "D301", "DeviceMap[" + num((int)i) + "] has a reserved bus_type"); res.definitions.push_back("; DeviceMap[" + num((int)i) + "]: reserved bus_type"); continue; }
            res.definitions.push_back(".devmap " + num((int)i) + ", I2C, " + hx((uint32_t)e.addr7));
        }
    }
    // table entries no instruction refers to cannot come back from the text
    static const char* mainT[] = {"MainMoveDesc", "MainBranchDesc", "MainCsrOneByteDesc", "MainCsrMultiByteDesc", "MainAluDesc"};
    static const char* periT[] = {"PeriXferOneByteDesc", "PeriXferMultiByteDesc", "PeriPublishDesc", "PeriBranchDesc", "PeriCsrOneByteDesc"};
    static const int mainW[] = {4, 2, 2, 2, 1}, periW[] = {2, 2, 2, 2, 2};
    for (int t = 0; t < 5; t++) {
        int w = mainD ? mainW[t] : periW[t];
        uint32_t n = (iv.end[t] > iv.off[t] ? iv.end[t] - iv.off[t] : 0) / (uint32_t)w;
        uint32_t unused = 0;
        for (uint32_t i = 0; i < n; i++) if (!used.count({t, i})) unused++;
        if (unused) issue(0, "D302", std::string(mainD ? mainT[t] : periT[t]) + " has " + std::to_string(unused) + " entr" + (unused == 1 ? "y" : "ies") + " that no instruction uses; they are not reproduced", false);
    }
    res.ok = true;
    for (const DisasmIssue& i : res.issues) if (i.fatal) res.ok = false;
    return res;
}

std::string DisasmResult::issuesText() const {
    std::ostringstream o;
    for (const DisasmIssue& i : issues) {
        char b[32];
        std::snprintf(b, sizeof b, "%04X", i.pc);
        o << domainName(domain) << " " << b << ": " << (i.fatal ? "error " : "note ") << i.code << ": " << i.msg << "\n";
    }
    return o.str();
}

std::string DisasmResult::source(bool annotate) const {
    std::ostringstream o;
    o << "; asmcore disassembly: " << domainName(domain) << ", " << codeSize << " bytes of code\n";
    o << (domain == Domain::Main ? ".main\n" : ".peri\n");
    for (const std::string& d : definitions) o << d << "\n";
    for (const DisasmLine& ln : lines) {
        if (!ln.label.empty()) o << ln.label << ":\n";
        o << "    " << ln.text;
        std::string c;
        if (annotate) {
            char b[16];
            std::snprintf(b, sizeof b, "%04X:", ln.pc);
            c = b;
            for (uint8_t x : ln.bytes) { std::snprintf(b, sizeof b, " %02X", x); c += b; }
        }
        if (!ln.comment.empty()) c += (c.empty() ? "" : "  ") + ln.comment;
        if (!c.empty()) o << "    ; " << c;
        o << "\n";
    }
    return o.str();
}

std::string disassembleResult(const Result& r, const Profile& pf, std::vector<DisasmIssue>* issues, bool annotate) {
    std::string out;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = r.image(d);
        if (!im.present) continue;
        std::map<uint32_t, std::string> names = labelNames(r, d);
        DisasmOptions o;
        o.names = &names;
        for (const VectorInfo& v : r.vectors) if (v.domain == d) o.vectors.push_back({v.id, v.pc});
        DisasmResult dr = disassemble(im, pf, o);
        out += dr.source(annotate) + "\n";
        if (issues) for (const DisasmIssue& i : dr.issues) issues->push_back(i);
    }
    return out;
}

}  // namespace asmcore
