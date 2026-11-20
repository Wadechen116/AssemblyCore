// The decoder's view of the program: what it makes of the bytes it fetches (from the EEPROM through the region registers), compared with what the
// assembler produced. This is what the hardware runs; the source text is what was meant.
#include <cstdio>
#include "asmcore/readiness.h"
#include <cstdlib>

#include "asmcore/disasm.h"
#include "asmcore/sram.h"
#include "../codec.h"
#include "../isa.h"
#include "impl.h"

namespace asmcore {
namespace sim {

namespace {

struct Reads {
    std::vector<std::pair<int, uint32_t>> entries;     // (table, index) the decoder looked up
};

// Decode the instruction at pc of an image, through the shared decoders of the disassembler (one bit layout for all three tools).
DecodedInstr decodeOne(const ImageView& iv, Domain d, uint32_t pc, const Profile& pf, const std::function<bool(int)>& condDefined, uint32_t constSize, Reads& reads) {
    static const DecoderSet decoders = DecoderSet::defaults();
    DecodeContext c;
    c.domain = d;
    c.pc = pc;
    c.alu = &pf.alu;
    c.codeLimit = iv.codeLimit;
    const uint32_t b = iv.u8(pc);
    c.opcode = (int)(b >> 6);
    c.arg = (int)(b & 63);
    c.byteAt = [&](uint32_t p) { return iv.u8(p); };
    c.entry = [&](int t, uint32_t idx, int eb, uint32_t& v) { reads.entries.push_back({t * 1000 + eb, idx}); return iv.entry(t, idx, eb, v); };
    c.deviceMap = [&](uint32_t dev, uint32_t& v) { reads.entries.push_back({PT_DEVMAP * 1000 + pf.deviceMapEntryBytes, dev}); return iv.entry(PT_DEVMAP, dev, pf.deviceMapEntryBytes, v); };
    c.profile = &pf;
    if (d == Domain::Main) c.constSize = constSize;                    // the simulator's: the const pool section (not the padding after it)
    c.conditionDefined = condDefined;
    const DecodeFn& fn = d == Domain::Main ? decoders.main[c.opcode] : decoders.peri[c.opcode];
    if (!fn) { DecodedInstr x; x.valid = false; x.problem = "no decoder for opcode " + std::to_string(c.opcode); x.text = "; ?"; return x; }
    return fn(c);
}

std::string withLabel(const DecodedInstr& x, const std::map<uint32_t, std::string>& labels) {
    std::string t = x.text;
    const size_t at = t.find("{L}");
    if (at != std::string::npos) {
        auto it = labels.find(x.target);
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%02X", x.target);
        t.replace(at, 3, it != labels.end() ? it->second : std::string(buf));
    }
    return t;
}

// The const pool bytes a MOV reads ("MAIN_CONST_POOL[a..b]" in its text): they belong to what the instruction fetches.
bool constRange(const std::string& t, uint32_t& a, uint32_t& b) {
    const size_t p = t.find("MAIN_CONST_POOL[");
    if (p == std::string::npos) return false;
    const char* q = t.c_str() + p + 16;
    char* end = nullptr;
    const unsigned long x = std::strtoul(q, &end, 10);
    if (end == q || end[0] != '.' || end[1] != '.') return false;
    q = end + 2;
    const unsigned long y = std::strtoul(q, &end, 10);
    if (end == q) return false;
    a = (uint32_t)x; b = (uint32_t)y;
    return true;
}

// A MOV from the const pool: the constant as fetched, after the instruction ("; const 34 12", in the order of the pool) -- what the debugger wants to see.
std::string withConst(const std::string& t, const ImageView& iv) {
    uint32_t a = 0, b = 0;
    if (!constRange(t, a, b) || b < a || b - a > 31) return t;
    std::string out = t + "   ; const";
    char buf[8];
    for (uint32_t k = a; k <= b; k++) { std::snprintf(buf, sizeof buf, " %02X", iv.u8(iv.off[MT_CONST] + k)); out += buf; }
    return out;
}

// ---- what an entry of a table says, field by field (the bit layouts are codec.h's; the names are the spec's) ----------------------------------------
const char* tableName(int d, int t) {
    static const char* mainN[6] = {"MainMoveDesc_32b", "MainBranchDesc_16b", "MainCsrOneByteDesc_16b", "MainCsrMultiByteDesc_16b", "MainAluDesc_8b", "MAIN_CONST_POOL"};
    static const char* periN[6] = {"PeriXferOneByteDesc_16b", "PeriXferMultiByteDesc_16b", "PeriPublishDesc_16b", "PeriBranchDesc_16b", "PeriCsrOneByteDesc_16b", "DeviceMap"};
    return t < 0 || t > 5 ? "?" : d == 0 ? mainN[t] : periN[t];
}
const char* offsetName(int d, int t) {
    const char* const* n = layoutFieldNames(d == 0 ? Domain::Main : Domain::Peri);
    return t < 0 || t > 5 || !n ? "offset" : n[t];
}
std::string num(long long v) { return std::to_string(v); }
std::string hexN(uint32_t v, int digits) { char b[16]; std::snprintf(b, sizeof b, "0x%0*X", digits, v); return b; }
std::string named(long long v, const std::string& name) { return num(v) + (name.empty() ? "" : " (" + name + ")"); }

std::vector<std::pair<std::string, std::string>> describeEntry(int d, int t, uint32_t v, const Profile& pf, const std::map<uint32_t, std::string>& labels) {
    std::vector<std::pair<std::string, std::string>> f;
    auto region = [](int id) { for (const isa::RegionInfo& r : isa::kRegions) if (r.id == id) return std::string(r.name); return std::string("reserved"); };
    auto target = [&](uint32_t pc) { auto it = labels.find(pc); return hexN(pc, 2) + (it == labels.end() ? "" : " (" + it->second + ")"); };
    auto rsv = [&](uint32_t bits) { f.push_back({"reserved", num(bits) + (bits ? "  <- must be 0" : "")}); };
    auto csr1 = [&] {
        static const char* ops[] = {"WRITE8_IMM", "RMW_SET8_IMM", "RMW_CLR8_IMM", "READ8"};
        const codec::Csr1 c = codec::unpackCsr1(v);
        f.push_back({"op [15:14]", named(c.op, ops[c.op & 3])});
        rsv(c.rsv);
        f.push_back({"csr_addr [11:0]", hexN(c.addr, 3)});
        f.push_back({"(inline byte)", "the value / mask / destination offset follows the instruction byte"});
    };
    if (d == 0) switch (t) {
        case MT_MOVE: {
            const codec::Move m = codec::unpackMove(v);
            f.push_back({"src_region [31:28]", named(m.srcRegion, region(m.srcRegion))});
            f.push_back({"dst_region [27:24]", named(m.dstRegion, region(m.dstRegion))});
            f.push_back({"src_off [23:16]", num(m.srcOff)});
            f.push_back({"dst_off [15:8]", num(m.dstOff)});
            rsv(m.rsv);
            f.push_back({"len_m1 [4:0]", num(m.len - 1) + "  -> " + num(m.len) + " byte(s)"});
            break;
        }
        case MT_BRANCH: {
            static const char* types[] = {"COND", "CASCADE_BEGIN", "CASCADE_END", "ERET"};
            const codec::MainBranch b = codec::unpackMainBranch(v);
            f.push_back({"br_type [15:14]", named(b.type, types[b.type & 3])});
            std::string what;
            if (b.type == isa::MB_COND) { for (const isa::NamedCode& c : isa::kMainConds) if (c.code == b.arg) what = c.name; if (what.empty()) what = "reserved condition"; }
            else if (b.type == isa::MB_CASCADE_BEGIN) what = "selector";
            else if (b.type == isa::MB_CASCADE_END) what = "block index";
            f.push_back({b.type == isa::MB_COND ? "cond_sel [13:10]" : "arg [13:10]", named(b.arg, what)});
            f.push_back({"event_safe [9]", num(b.eventSafe)});
            rsv(b.rsv);
            f.push_back({"target_pc [7:0]", target(b.target)});
            break;
        }
        case MT_CSR1: csr1(); break;
        case MT_CSRM: {
            const codec::CsrM c = codec::unpackCsrM(v);
            f.push_back({"op [15:14]", named(c.op, c.op == isa::CSRM_WRITE_SP ? "WRITE_SP" : c.op == isa::CSRM_READ_BUF ? "READ_BUF" : "reserved")});
            f.push_back({"csr_addr[11:8] [13:10]", num(c.page) + "  (csr_addr[7:0] is the inline byte)"});
            f.push_back({"len_m1 [9:5]", num(c.len - 1) + "  -> " + num(c.len) + " byte(s)"});
            f.push_back({"ptr [4:0]", num(c.ptr) + (c.op == isa::CSRM_WRITE_SP ? "  (MAIN_SCRATCHPAD source)" : "  (MAIN_CSR_DATA_BUF destination)")});
            break;
        }
        case MT_ALU: {
            static const char* cls[] = {"ARITH", "CMP", "LOGIC", "SHIFT"};
            static const char* arith[] = {"ADD", "SUB", "MUL", "INC", "DEC"};
            static const char* cmp[] = {"EQ", "NE", "LT", "GE"};
            static const char* logic[] = {"AND", "OR", "XOR", "NOT"};
            const codec::Alu a = codec::unpackAlu(v, pf.alu);
            f.push_back({"class [7:6]", named(a.cls, cls[a.cls & 3])});
            if (a.cls != isa::A_LOGIC) f.push_back({"signed [5]", named(a.signedMode, a.signedMode ? "signed" : "unsigned")});
            if (a.cls == isa::A_ARITH) f.push_back({"arith_op [4:0]", a.op < 0 ? "reserved" : std::string(arith[a.op])});
            if (a.cls == isa::A_CMP) { f.push_back({"cmp_op [4:3]", a.op < 0 ? "reserved" : std::string(cmp[a.op])}); rsv(a.rsv); }
            if (a.cls == isa::A_LOGIC) { f.push_back({"logic_op [5:3]", a.op < 0 ? "reserved" : std::string(logic[a.op])}); rsv(a.rsv); }
            if (a.cls == isa::A_SHIFT) { f.push_back({"dir [4]", named(a.dir, a.dir ? "SHR" : "SHL")}); f.push_back({"amount [3:0]", num(a.amt)}); }
            break;
        }
        default: break;
    } else switch (t) {
        case PT_XFER1: {
            const codec::Xfer1 x = codec::unpackXfer1(v);
            f.push_back({"rw [15]", named(x.rw, x.rw ? "READ" : "WRITE")});
            f.push_back({"dev [14:12]", num(x.dev) + "  (DeviceMap entry)"});
            f.push_back({"addr_len [11:10]", num(x.addrLen) + (x.addrLen == 3 ? "  <- reserved" : " byte(s)")});
            f.push_back({"addr [9:0]", hexN(x.addr, 3)});
            break;
        }
        case PT_XFERM: {
            const codec::XferM x = codec::unpackXferM(v);
            f.push_back({"rw [15]", named(x.rw, x.rw ? "READ" : "WRITE")});
            f.push_back({"dev [14:12]", num(x.dev) + "  (DeviceMap entry)"});
            f.push_back({"addr_len [11:10]", num(x.addrLen) + (x.addrLen == 3 ? "  <- reserved" : " byte(s)")});
            f.push_back({"len_m1 [9:5]", num(x.len - 1) + "  -> " + num(x.len) + " byte(s)"});
            f.push_back({"ptr [4:0]", num(x.ptr)});
            break;
        }
        case PT_PUB: {
            const codec::Pub p = codec::unpackPub(v);
            f.push_back({"service_id [15:12]", num(p.service)});
            f.push_back({"src_ptr [11:7]", num(p.src)});
            f.push_back({"len_m1 [6:2]", num(p.len - 1) + "  -> " + num(p.len) + " byte(s)"});
            rsv(p.rsv);
            break;
        }
        case PT_BRANCH: {
            const codec::PeriBranch b = codec::unpackPeriBranch(v);
            std::string what = b.type == isa::PB_ERET ? "ERET" : "";
            for (const isa::NamedCode& c : isa::kPeriConds) if (c.code == b.type) what = c.name;
            f.push_back({"br_type [15:12]", named(b.type, what.empty() ? "reserved" : what)});
            f.push_back({"event_safe [11]", num(b.eventSafe)});
            rsv(b.rsv);
            f.push_back({"target_pc [7:0]", target(b.target)});
            break;
        }
        case PT_CSR1: csr1(); break;
        case PT_DEVMAP: {
            const codec::DevEntry e = codec::unpackDev(pf, v);
            f.push_back({"bus_type", named(e.busType, e.busType == isa::BUS_I2C ? "I2C" : "reserved")});
            f.push_back({"i2c_addr_7b", hexN((uint32_t)e.addr7, 2)});
            break;
        }
        default: break;
    }
    return f;
}

}  // namespace

DecodedLine Simulator::Impl::decode(int d, uint32_t pc) const {
    const_cast<Impl*>(this)->syncFetch();
    // the cache holds as long as the bytes and the registers are the same
    std::vector<uint64_t> key{(uint64_t)(uintptr_t)(fetchEeprom && st.eeprom ? st.eeprom.get() : nullptr)};
    for (int i = 0; i < 2; i++) key.push_back((uint64_t)(uintptr_t)(fetchFrontend && st.sram[i] ? st.sram[i].get() : nullptr));
    for (int64_t v : {(int64_t)fetchFrontend, (int64_t)sramLatency, (int64_t)sramPolicy, (int64_t)feHold, (int64_t)feDec, (int64_t)feFire}) key.push_back((uint64_t)v);
    for (int i = 0; i < 2; i++) for (int k = 0; k < 3; k++) key.push_back(img[i].regionBase[k]);
    for (int64_t v : {(int64_t)fetchMode, (int64_t)fetchOverhead, (int64_t)fetchEeprom, (int64_t)lat.i2cHz, (int64_t)lat.cpuHz, (int64_t)i2cAck, (int64_t)i2cStretch}) key.push_back((uint64_t)v);      // (the fetch time)
    if (key != decodeKey) { decodeKey = key; decodeCache[0].clear(); decodeCache[1].clear(); }
    auto hit = decodeCache[d].find(pc);
    if (hit != decodeCache[d].end()) return hit->second;

    const Domain dom = d == 0 ? Domain::Main : Domain::Peri;
    const ImageView& iv = img[d];
    DecodedLine L;
    L.domain = dom;
    L.pc = pc;
    Reads reads;
    // the conditions as this simulator has them (the defaults, or the environment file's [condition ...])
    const std::function<bool(int)> condDefined = [this, d](int code) { return code >= 0 && code < (d == 0 ? 16 : 8) && cond[d][code].defined; };
    const DecodedInstr x = decodeOne(iv, dom, pc, pf, condDefined, constSize, reads);
    L.length = std::max<uint32_t>(1, x.length);
    L.valid = x.valid;
    L.problem = x.problem;
    L.specProblem = x.specProblem;
    L.text = d == 0 ? withConst(withLabel(x, labelAt[d]), iv) : withLabel(x, labelAt[d]);
    L.eepromAddr = iv.eeprom || iv.loaded ? iv.eepromAddressOf(pc) : pc;
    for (uint32_t i = 0; i < L.length; i++) L.bytes.push_back((uint8_t)iv.u8(pc + i));
    L.fetchBytes.push_back(L.length);                                     // the code bytes (opcode and inline bytes)
    for (const auto& e : reads.entries) L.fetchBytes.push_back((uint32_t)(e.first % 1000));      // each descriptor / DeviceMap entry
    if (d == 1 && (iv.u8(pc) >> 6) == 3) {                                // EXEC_XFER: the hardware also reads its DeviceMap entry (the disassembler does not need it)
        bool have = false;
        for (const auto& e : reads.entries) have = have || e.first / 1000 == PT_DEVMAP;
        if (!have) L.fetchBytes.push_back((uint32_t)pf.deviceMapEntryBytes);
    }
    {
        uint32_t c0 = 0, c1 = 0;
        if (d == 0 && constRange(L.text, c0, c1) && c1 >= c0) L.fetchBytes.push_back(c1 - c0 + 1);     // the constants
    }
    // ---- the same reads, each with what it is and what it holds
    {
        static const char* mainOp[4] = {"EXEC_CSR", "BR", "MOV", "ALU"};
        static const char* periOp[4] = {"EXEC_CSR", "BR", "EXEC_PUB", "EXEC_XFER"};
        const uint32_t ib = iv.u8(pc);
        L.opcode = (int)(ib >> 6);
        L.arg = (int)(ib & 63);
        L.opcodeName = d == 0 ? mainOp[L.opcode] : periOp[L.opcode];
        auto place = [&](DecodeRead& r, uint32_t logical, uint32_t n) {
            r.logicalAddr = logical;
            r.eepromAddr = iv.eeprom || iv.loaded ? iv.eepromAddressOf(logical) : logical;
            const PhysicalByte a = physicalByte(pf, dom, logical), b = physicalByte(pf, dom, logical + (n ? n - 1 : 0));
            r.sramWordFirst = a.word; r.sramWordLast = b.word; r.sramLane = a.lane;
            for (uint32_t i = 0; i < n; i++) r.bytes.push_back((uint8_t)iv.u8(logical + i));
        };
        DecodeRead op;
        op.kind = "opcode"; op.state = "CMD_OP_GET";
        place(op, pc, 1);
        op.value = ib;
        L.reads.push_back(op);
        if (L.length > 1) {
            DecodeRead in;
            in.kind = "inline"; in.state = "CMD_DESC_GET";
            place(in, pc + 1, L.length - 1);
            L.reads.push_back(in);
        }
        auto entry = [&](int t, int width, uint32_t idx) {
            DecodeRead r;
            const bool dev = d == 1 && t == PT_DEVMAP;
            r.kind = dev ? "devmap" : "descriptor"; r.state = "CMD_DESC_GET";
            r.table = tableName(d, t);
            r.index = (int)idx;
            r.addressRule = std::string(offsetName(d, t)) + " + " + (dev ? "dev" : "index") + " * " + std::to_string(width);
            uint32_t v = 0;
            r.present = iv.entry(t, idx, width, v);
            place(r, iv.off[t] + idx * (uint32_t)width, r.present ? (uint32_t)width : 0);
            r.value = v;
            if (r.present) r.fields = describeEntry(d, t, v, pf, labelAt[d]);
            L.reads.push_back(r);
            return v;
        };
        bool haveDev = false;
        uint32_t xferDesc = 0;
        int xferTable = -1;
        for (const auto& e : reads.entries) {
            const int t = e.first / 1000, w = e.first % 1000;
            const uint32_t v = entry(t, w, e.second);
            if (d == 1 && t == PT_DEVMAP) haveDev = true;
            if (d == 1 && (t == PT_XFER1 || t == PT_XFERM)) { xferDesc = v; xferTable = t; }
        }
        if (d == 1 && L.opcode == 3 && !haveDev && xferTable >= 0)             // EXEC_XFER: the DeviceMap entry its descriptor names
            entry(PT_DEVMAP, pf.deviceMapEntryBytes, (uint32_t)(xferTable == PT_XFER1 ? codec::unpackXfer1(xferDesc).dev : codec::unpackXferM(xferDesc).dev));
        uint32_t c0 = 0, c1 = 0;
        if (d == 0 && constRange(L.text, c0, c1) && c1 >= c0) {
            DecodeRead r;
            r.kind = "const"; r.state = "CMD_DESC_GET";
            r.table = tableName(0, MT_CONST);
            r.index = (int)c0;
            r.addressRule = std::string(offsetName(0, MT_CONST)) + " + src_off";
            place(r, iv.off[MT_CONST] + c0, c1 - c0 + 1);
            L.reads.push_back(r);
        }
    }
    L.fetchCycles = fetchTimed() ? fetchCost(L.fetchBytes) : 0;
    if (fetchFrontend) {                                                  // through the Frontend, with nobody else at the SRAM port: CMD_HOLD .. CMD_FIRE
        decodeCache[d][pc] = L;                                           // (the plan reads the line: let it find it)
        const State::FeRun plan = const_cast<Impl*>(this)->planFrontend(d, pc, 0, false);
        L.fetchCycles = (uint64_t)plan.exec;
    }
    auto src = instrAt[d].find(pc);
    if (src != instrAt[d].end()) L.source = src->second->text;
    if (iv.eeprom || iv.loaded) {                                        // what the assembler's image holds at the same place, decoded the same way
        ImageView plain = iv;
        plain.eeprom = nullptr;
        plain.loaded = nullptr;
        Reads r2;
        const DecodedInstr y = decodeOne(plain, dom, pc, pf, condDefined, constSize, r2);
        L.expected = d == 0 ? withConst(withLabel(y, labelAt[d]), plain) : withLabel(y, labelAt[d]);
        bool same = L.expected == L.text && y.length == x.length;
        for (uint32_t i = 0; same && i < L.length; i++) same = plain.u8(pc + i) == iv.u8(pc + i);
        for (const auto& e : reads.entries) {                            // every descriptor (and DeviceMap entry) it looked up
            if (!same) break;
            uint32_t a = 0, b = 0;
            const int t = e.first / 1000, w = e.first % 1000;
            const bool oa = iv.entry(t, e.second, w, a), ob = plain.entry(t, e.second, w, b);
            same = oa == ob && a == b;
        }
        uint32_t c0 = 0, c1 = 0;
        if (same && d == 0 && constRange(L.text, c0, c1))
            for (uint32_t k = c0; same && k <= c1; k++) same = iv.u8(iv.off[MT_CONST] + k) == plain.u8(plain.off[MT_CONST] + k);
        L.matches = same;
    } else {
        L.expected = L.text;
    }
    decodeCache[d][pc] = L;
    return L;
}

// An instruction starts: without a source its text is what the decoder made of the bytes; with one, a difference is said.
void Simulator::Impl::annotateIssue(Event& e) const {
    if (e.domain != 0 && e.domain != 1) return;
    const bool haveSource = instrAt[e.domain].count(e.pc) != 0;
    if (haveSource && !(fetchEeprom && st.eeprom)) return;               // (the image itself: the source is what runs)
    const DecodedLine L = decode(e.domain, e.pc);
    if (!haveSource) e.what = L.text;
    else if (!L.matches) e.text = "fetched from the EEPROM: " + L.text + (L.valid ? std::string() : " (" + L.problem + ")");
}

// The bus time of reading these pieces from the EEPROM: each a random read (address+W, the memory address, address+R, the data), or one per byte.
uint64_t Simulator::Impl::fetchCost(const std::vector<uint32_t>& bytes) const {
    if (fetchMode == 2) return 0;
    const int ab = std::max(1, emap.addrBytes);
    auto txn = [&](uint32_t n) {
        const int phases = 2 + ab + (int)n;
        return (uint64_t)lat.i2cCycles(phases) + (uint64_t)phases * (uint64_t)(i2cAck + i2cStretch) + (uint64_t)fetchOverhead;
    };
    uint64_t total = 0;
    for (uint32_t n : bytes) {
        if (!n) continue;
        if (fetchMode == 0) total += txn(n);
        else total += n * txn(1);
    }
    return total;
}

// The decoder reads the instruction at its pc from the EEPROM over the I2C bus. The bus is shared (the other decoder, EXEC_XFER): taken, it waits.
bool Simulator::Impl::beginFetch(int who) {
    if (fetchFrontend) return beginFrontend(who);
    const uint32_t pc = who == 0 ? st.m.pc : st.p.pc;
    Cycle& ready = who == 0 ? st.m.readyAt : st.p.readyAt;
    if (!img[who].present || pc >= img[who].codeLimit) { st.fetched[who] = true; return false; }       // (the step faults on it)
    const DecodedLine L = decode(who, pc);
    const uint64_t cost = fetchCost(L.fetchBytes);
    if (!cost) { st.fetched[who] = true; return false; }
    const Cycle until = claim(st.bus, who, 0);
    if (until) {
        ready = until;
        st.busWaits[who]++;
        st.busWaitCycles[who] += until - st.now;
        emit([&](Event& e) { e.type = EventType::Stall; e.domain = who; e.pc = pc; e.what = "i2c"; e.a = (int64_t)(until - st.now); });
        return true;
    }
    st.bus.freeAt = st.now + (Cycle)cost;
    st.bus.last = who;
    ready = st.now + (Cycle)cost;
    st.fetched[who] = true;
    st.fetches[who]++;
    st.fetchCycles[who] += cost;
    uint32_t total = 0;
    for (uint32_t n : L.fetchBytes) total += n;
    emit([&](Event& e) { e.type = EventType::Bus; e.domain = who; e.pc = pc; e.what = "eeprom.fetch"; e.a = emap.devAddr; e.b = 1; e.c = total; e.text = std::to_string(cost) + " cycles"; });
    return true;
}

DecodedLine Simulator::decodedAt(Domain d, uint32_t pc) const { return p_->decode(di(d), pc); }

std::vector<DecodedLine> Simulator::decodedProgram(Domain d) const {
    std::vector<DecodedLine> v;
    const int k = di(d);
    if (!p_->img[k].present) return v;
    for (uint32_t pc = 0; pc < p_->img[k].codeLimit;) {
        DecodedLine L = p_->decode(k, pc);
        pc += L.length;
        v.push_back(std::move(L));
    }
    return v;
}

FetchTimes fetchTimes(const Result& program, const SimConfig& cfg) {
    FetchTimes t;
    if (!program.ok) return t;
    Simulator sim(program, cfg);
    const Simulator::Impl& I = *sim.p_;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const int k = d == Domain::Main ? 0 : 1;
        for (const DecodedLine& L : sim.decodedProgram(d)) {
            const uint64_t c = L.fetchCycles + (uint64_t)std::max(0, I.fetchExtra[k]);
            if (c) t.cycles[{k, L.pc}] = (int)std::min<uint64_t>(c, 0x3fffffff);
        }
    }
    if (!t.any()) return t;
    std::string m;
    if (I.fetchFrontend) m = "program SRAM through the Frontend (SRAM latency " + std::to_string(I.sramLatency) + ", the frontend.* planning values)";
    else if (I.fetchTimed())
        m = std::string("I2C EEPROM, ") + (I.fetchMode == 0 ? "one read per region" : "one read per byte") + ", " + std::to_string(I.lat.i2cHz) + " Hz";
    if (I.fetchExtra[0] || I.fetchExtra[1]) m += std::string(m.empty() ? "" : "; ") + "main.fetch_cycles / peri.fetch_cycles";
    t.model = m;
    return t;
}

Result assembleWithFetch(const std::vector<Source>& sources, const IncludeResolver& resolver, const Options& opt, SimConfig cfg, FetchTimes* used) {
    if (used) *used = FetchTimes();
    Result first = assemble(sources, resolver, opt);
    first.readiness = readiness(opt.profile, &cfg.params);              // built with the hardware parameters: they are part of what the level is decided by
    first.readinessSet = true;
    if (!first.ok) return first;
    cfg.profile = opt.profile;
    FetchTimes t = fetchTimes(first, cfg);
    if (!t.any()) return first;
    Options o2 = opt;
    o2.fetchCycles = [&t](Domain d, uint32_t pc) { return t.at(d, pc); };
    Result second = assemble(sources, resolver, o2);
    second.readiness = first.readiness;
    second.readinessSet = true;
    if (used) *used = t;
    return second;
}

EepromVerifyReport verifyEepromImage(const Result& program, const SimConfig& cfgIn, const std::vector<uint8_t>& image, const EepromMap* imageMap) {
    EepromVerifyReport rep;
    std::vector<uint8_t> want;
    EepromMap map;
    std::string err;
    if (!buildEeprom(program, cfgIn.profile, want, map, err)) { rep.problems.push_back("the program's EEPROM image cannot be made: " + err); return rep; }
    if (imageMap) {                                                         // the map that came with the image: does it place the regions where the program does?
        for (int i = 0; i < ER_COUNT; i++) {
            const EepromRegion &a = map.region[i], &b = imageMap->region[i];
            if (a.addr != b.addr || a.size != b.size)
                rep.problems.push_back(std::string(eepromRegionName(i)) + ": the image's map has it at 0x" + [&] { char t[16]; std::snprintf(t, sizeof t, "%04X", b.addr); return std::string(t); }() +
                                       ", " + std::to_string(b.size) + " bytes; the program needs 0x" + [&] { char t[16]; std::snprintf(t, sizeof t, "%04X", a.addr); return std::string(t); }() + ", " + std::to_string(a.size) + " bytes");
        }
        for (int d = 0; d < 2; d++)
            if (imageMap->present[d] && map.present[d] && imageMap->layout[d] != map.layout[d]) rep.problems.push_back(std::string(d == 0 ? "Main" : "Peri") + ": the image's map has another ImageLayout (descriptor table offsets)");
    }
    for (int i = 0; i < ER_COUNT; i++)
        for (uint32_t k = 0; k < map.region[i].size; k++) {
            const uint32_t a = map.region[i].addr + k;
            rep.bytesCompared++;
            const uint8_t w = want[a];
            if (a >= image.size()) { rep.diffs.push_back({i, a, w, (uint8_t)map.fill, true}); continue; }
            if (image[a] != w) rep.diffs.push_back({i, a, w, image[a], false});
        }
    {   // what the decoders make of it, instruction by instruction (the regions where the program places them)
        SimConfig c = cfgIn;
        c.eepromImage = image;
        c.eepromMap = map;
        std::string e;
        c.params.set("fetch.source", "eeprom", e);
        c.params.set("fetch.read_mode", "instant", e);
        Simulator sim(program, c);
        for (Domain d : {Domain::Main, Domain::Peri})
            for (const DecodedLine& L : sim.decodedProgram(d)) if (!L.matches || !L.valid) rep.instructions.push_back(L);
    }
    bool missing = false;
    for (const EepromByteDiff& x : rep.diffs) missing = missing || x.missing;
    if (missing) rep.problems.push_back("the image ends at " + std::to_string(image.size()) + " bytes: the program needs " + std::to_string(want.size()));
    rep.ok = rep.problems.empty() && rep.diffs.empty() && rep.instructions.empty();
    return rep;
}

}  // namespace sim
}  // namespace asmcore
