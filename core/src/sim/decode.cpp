// The decoder's view of the program: what it makes of the bytes it fetches (from the EEPROM through the region registers), compared with what the
// assembler produced. This is what the hardware runs; the source text is what was meant.
#include <cstdio>
#include <cstdlib>

#include "asmcore/disasm.h"
#include "impl.h"

namespace asmcore {
namespace sim {

namespace {

struct Reads {
    std::vector<std::pair<int, uint32_t>> entries;     // (table, index) the decoder looked up
};

// Decode the instruction at pc of an image, through the shared decoders of the disassembler (one bit layout for all three tools).
DecodedInstr decodeOne(const ImageView& iv, Domain d, uint32_t pc, const Profile& pf, Reads& reads) {
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

}  // namespace

DecodedLine Simulator::Impl::decode(int d, uint32_t pc) const {
    const_cast<Impl*>(this)->syncFetch();
    // the cache holds as long as the bytes and the registers are the same
    std::vector<uint64_t> key{(uint64_t)(uintptr_t)(fetchEeprom && st.eeprom ? st.eeprom.get() : nullptr)};
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
    const DecodedInstr x = decodeOne(iv, dom, pc, pf, reads);
    L.length = std::max<uint32_t>(1, x.length);
    L.valid = x.valid;
    L.problem = x.problem;
    L.text = d == 0 ? withConst(withLabel(x, labelAt[d]), iv) : withLabel(x, labelAt[d]);
    L.eepromAddr = iv.eeprom ? iv.eepromAddressOf(pc) : pc;
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
    L.fetchCycles = fetchTimed() ? fetchCost(L.fetchBytes) : 0;
    auto src = instrAt[d].find(pc);
    if (src != instrAt[d].end()) L.source = src->second->text;
    if (iv.eeprom) {                                                     // what the assembler's image holds at the same place, decoded the same way
        ImageView plain = iv;
        plain.eeprom = nullptr;
        Reads r2;
        const DecodedInstr y = decodeOne(plain, dom, pc, pf, r2);
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
