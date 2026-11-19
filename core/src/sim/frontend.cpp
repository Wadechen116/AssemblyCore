// fetch.source = frontend: the program store as spec v31 has it.
//
//   boot      Power-on: both sequencers are held. Boot reads the two logical images from the ROM (the I2C EEPROM, through the six region registers as they
//             are at reset), packs them into 32-bit words and writes them into the ONE shared program SRAM; then the write path is locked and both
//             sequencers are released. From then on the ROM does not matter: the Frontends read the SRAM.
//   Frontend  Every instruction goes through the seven states of Frontend-001:
//               CMD_HOLD -> CMD_OP_GET (the opcode word) -> CMD_DESC_GET (the inline bytes, the descriptor, the DeviceMap entry, the constants)
//               -> CMD_DEC (the checks, the normalized command) -> CMD_FIRE (handed to the execution engine) -> CMD_EXEC (until it completes)
//             and CMD_FAULT on an error. What is read in which state is the DecodedLine of the instruction (decode.cpp); here the reads are timed.
//   the SRAM  One port, clocked at twice the ISA clock: two SRAM cycles in an ISA cycle. Each word read takes frontend.sram_latency SRAM cycles; the two
//             Frontends share the port (frontend.sram_policy). The response is captured on the next ISA edge.
//
// The spec leaves the SRAM latency, the port scheduling and the length of the states to the RTL profile: the frontend.* parameters hold them, as planning
// values until the RTL gives them. The opcode buffer holds one word: bytes of the code that lie in it are not read again.
#include <algorithm>

#include "asmcore/sram.h"
#include "impl.h"

namespace asmcore {
namespace sim {

void Simulator::Impl::bootLoadSram() {
    uint64_t cost[2] = {0, 0};
    for (int d = 0; d < 2; d++) {
        st.sram[d].reset();
        if (!img[d].present) continue;
        ImageView from = img[d];
        from.loaded = nullptr;
        from.eeprom = st.eeprom ? st.eeprom.get() : nullptr;           // (no EEPROM configured: the assembler's image is what the ROM holds)
        if (from.eeprom)
            for (int k = 0; k < 3; k++) {
                const int id = regionSig[d * 3 + k];
                from.regionBase[k] = id >= 0 ? (uint32_t)signal(sigs[(size_t)id].name) : emap.region[d * 3 + k].addr;
            }
        std::vector<uint8_t> bytes(from.bytes.size());
        for (size_t o = 0; o < bytes.size(); o++) bytes[o] = (uint8_t)from.u8((uint32_t)o);
        st.sram[d] = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        if (bootLoad == 1 && st.eeprom) {                               // the bus time of reading its three regions, one sequential read each
            const int ab = std::max(1, emap.addrBytes);
            for (int k = 0; k < 3; k++) {
                const uint32_t n = emap.region[d * 3 + k].size;
                if (!n) continue;
                const int phases = 2 + ab + (int)n;
                cost[d] += (uint64_t)lat.i2cCycles(phases) + (uint64_t)phases * (uint64_t)(i2cAck + i2cStretch) + (uint64_t)fetchOverhead;
            }
        }
    }
    st.bootStart = (Cycle)bootCycles;
    st.bootMainEnd = st.bootStart + (Cycle)cost[0];
    st.bootPeriEnd = st.bootMainEnd + (Cycle)cost[1];
    st.m.readyAt = st.p.readyAt = st.bootPeriEnd;                       // both images valid, the write path locked: the sequencers are released together
    if (st.bootPeriEnd > st.bootStart) st.bus.freeAt = st.bootPeriEnd;  // (boot has the I2C bus until then)
    for (int d = 0; d < 2; d++) { st.fe[d] = State::FeRun(); st.feOpValid[d] = false; }
    syncFetch();
}

State::FeRun Simulator::Impl::planFrontend(int who, uint32_t pc, Cycle start, bool commit) {
    State::FeRun r;
    r.valid = true;
    r.pc = pc;
    r.hold = start;
    r.opGet = start + (Cycle)feHold;
    const DecodedLine L = decode(who, pc);
    const Domain dom = who == 0 ? Domain::Main : Domain::Peri;
    uint32_t opBuf = st.feOpWord[who];
    bool opValid = st.feOpValid[who];
    std::vector<uint64_t> taken = st.sramTaken;
    taken.erase(taken.begin(), std::lower_bound(taken.begin(), taken.end(), (uint64_t)st.now * 2));      // (what is over)
    uint64_t slotFree = st.sramSlotFree[who], reads = 0, waits = 0;
    // one word read: the SRAM cycle it is issued in (the port may be taken), the ISA cycle its response is captured in
    auto issue = [&](uint64_t earliest, FrontendWord& w) {
        uint64_t s = earliest;
        if (sramPolicy == 1) {                                           // its own slot of the ISA cycle: Main the first SRAM cycle, Peri the second
            if (commit) s = std::max(s, slotFree);
            if ((s & 1) != (uint64_t)who) s++;
            slotFree = s + 2;
        } else if (commit) {                                             // one port for both: the first SRAM cycle from then on that nobody has taken
            auto it = std::lower_bound(taken.begin(), taken.end(), s);
            while (it != taken.end() && *it == s) { ++s; ++it; }
            taken.insert(it, s);
        }
        w.waitedSramCycles = (uint32_t)(s - earliest);
        w.done = (Cycle)((s + (uint64_t)sramLatency + 1) / 2);            // captured on the ISA edge after the response
        reads++;
        waits += s - earliest;
        return s;
    };
    auto wordsOf = [&](const DecodeRead& x) {
        std::vector<uint32_t> v;
        for (size_t i = 0; i < x.bytes.size(); i++) {
            const uint32_t w = physicalByte(pf, dom, x.logicalAddr + (uint32_t)i).word;
            if (v.empty() || v.back() != w) v.push_back(w);
        }
        return v;
    };
    // ---- CMD_OP_GET: the word that holds the opcode byte (not read again if the opcode buffer holds it)
    Cycle end = r.opGet + 1;
    for (const DecodeRead& x : L.reads) {
        if (x.kind != "opcode") continue;
        for (uint32_t w : wordsOf(x)) {
            FrontendWord fw;
            fw.word = w;
            fw.forOpcode = true;
            fw.requested = r.opGet;
            if (opValid && opBuf == w) { fw.buffered = true; fw.done = r.opGet; }
            else { issue((uint64_t)r.opGet * 2, fw); end = std::max(end, fw.done); opBuf = w; opValid = true; }
            r.words.push_back(fw);
        }
    }
    r.descGet = end;
    // ---- CMD_DESC_GET: everything else, one word after the other. It is left only when all of it is there (also when there is nothing to read).
    end = r.descGet + 1;
    uint64_t at = (uint64_t)r.descGet * 2;
    std::vector<uint32_t> have;                                          // words already read for this instruction
    for (const DecodeRead& x : L.reads) {
        if (x.kind == "opcode") continue;
        for (uint32_t w : wordsOf(x)) {
            if (std::find(have.begin(), have.end(), w) != have.end()) continue;
            FrontendWord fw;
            fw.word = w;
            fw.requested = (Cycle)(at / 2);
            if (x.kind == "inline" && opValid && opBuf == w) { fw.buffered = true; fw.done = r.descGet; }
            else {
                const uint64_t s = issue(at, fw);
                at = s + 1;
                end = std::max(end, fw.done);
                have.push_back(w);
                if (x.kind == "inline") opBuf = w;                       // (the code goes on in that word)
            }
            r.words.push_back(fw);
        }
    }
    r.dec = end;
    r.fire = r.dec + (Cycle)feDec;
    r.exec = r.fire + (Cycle)feFire;
    if (commit) {
        st.feOpWord[who] = opBuf;
        st.feOpValid[who] = opValid;
        st.sramTaken = std::move(taken);
        st.sramSlotFree[who] = slotFree;
        st.sramReads[who] += reads;
        st.sramWaits[who] += waits;
    }
    return r;
}

// The instruction at the pc starts through the Frontend: its states are laid out from now, and it executes when CMD_FIRE has handed it over.
bool Simulator::Impl::beginFrontend(int who) {
    const uint32_t pc = who == 0 ? st.m.pc : st.p.pc;
    Cycle& ready = who == 0 ? st.m.readyAt : st.p.readyAt;
    if (!img[who].present || pc >= img[who].codeLimit) { st.fetched[who] = true; return false; }       // (the step faults on it)
    st.fe[who] = planFrontend(who, pc, st.now, true);
    st.fetched[who] = true;
    st.fetches[who]++;
    st.fetchCycles[who] += (uint64_t)(st.fe[who].exec - st.now);
    if (st.fe[who].exec <= st.now) return false;                         // (every state has no length: it executes now)
    ready = st.fe[who].exec;
    return true;
}

FrontendStatus Simulator::frontendStatus(Domain d) const {
    const Impl& I = *p_;
    const int k = d == Domain::Main ? 0 : 1;
    FrontendStatus s;
    s.active = I.fetchFrontend;
    if (!s.active) return s;
    const State::FeRun& f = I.st.fe[k];
    const Cycle now = I.st.now;
    const bool faulted = k == 0 ? I.st.m.faulted : I.st.p.faulted;
    s.sramReads = I.st.sramReads[k];
    s.sramWaitCycles = I.st.sramWaits[k];
    if (f.valid) {
        s.forming = true;
        s.pc = f.pc;
        s.hold = f.hold; s.opGet = f.opGet; s.descGet = f.descGet; s.dec = f.dec; s.fire = f.fire; s.exec = f.exec; s.done = f.done;
        s.words = f.words;
    }
    if (faulted) s.state = "CMD_FAULT";
    else if (!f.valid) s.state = now < I.st.bootPeriEnd ? "RESET" : "CMD_HOLD";
    else if (now < f.opGet) s.state = "CMD_HOLD";
    else if (now < f.descGet) s.state = "CMD_OP_GET";
    else if (now < f.dec) s.state = "CMD_DESC_GET";
    else if (now < f.fire) s.state = "CMD_DEC";
    else if (now < f.exec || (f.done == 0 && now == f.exec)) s.state = "CMD_FIRE";      // (handed over at f.exec: until the engine has taken it, it is still fired)
    else if (f.done == 0 || now < f.done) s.state = "CMD_EXEC";
    else { s.state = "CMD_HOLD"; s.forming = false; }                                       // completed: waiting for the next one
    return s;
}

BootStatus Simulator::bootStatus() const {
    const Impl& I = *p_;
    BootStatus b;
    b.active = I.fetchFrontend;
    if (!b.active) return b;
    b.start = I.st.bootStart; b.mainEnd = I.st.bootMainEnd; b.periEnd = I.st.bootPeriEnd;
    b.mainWords = I.st.sram[0] ? (uint32_t)((I.st.sram[0]->size() + 3) / 4) : 0;
    b.periWords = I.st.sram[1] ? (uint32_t)((I.st.sram[1]->size() + 3) / 4) : 0;
    const Cycle now = I.st.now;
    auto part = [](Cycle now, Cycle from, Cycle to, uint32_t words) { return to <= from || now >= to ? words : now <= from ? 0u : (uint32_t)((uint64_t)words * (uint64_t)(now - from) / (uint64_t)(to - from)); };
    b.done = now >= b.periEnd;
    b.wordsWritten = part(now, b.start, b.mainEnd, b.mainWords) + (now >= b.mainEnd ? part(now, b.mainEnd, b.periEnd, b.periWords) : 0u);
    b.phase = b.done ? "locked, running" : now < b.mainEnd ? "loading Main image" : "loading Peri image";
    return b;
}

}  // namespace sim
}  // namespace asmcore
