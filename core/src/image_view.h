#pragma once
// The program image of one domain as the hardware sees it (spec 3.1): executable bytecode from offset 0, descriptor tables, the
// constant pool or DeviceMap, and the ImageLayout tail whose fields locate the tables. Shared by the simulator and the disassembler.
#include <cstdint>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"

namespace asmcore {

struct ImageView {
    std::vector<uint8_t> bytes;
    uint32_t off[6] = {0, 0, 0, 0, 0, 0};     // ImageLayout fields, in the order of spec 3.1
    uint32_t end[6] = {0, 0, 0, 0, 0, 0};     // end of each table (start of the next one)
    uint32_t codeLimit = 0;
    bool present = false;
    bool big = false;

    // Where the bytes come from. Normally `bytes`. The simulator can point it at the I2C EEPROM instead (what the decoders really fetch): logical
    // offset o of region k (0 code, 1 descriptor tables, 2 const pool / DeviceMap; k is the last region whose regionStart <= o) is read at
    // regionBase[k] + (o - regionStart[k]) of the EEPROM, wrapping at its size. The table offsets (off/end) keep their logical meaning.
    const std::vector<uint8_t>* eeprom = nullptr;
    uint32_t regionStart[3] = {0, 0, 0}, regionBase[3] = {0, 0, 0};

    bool load(const DomainImage& im, const Profile& pf, std::string& error);
    uint32_t u8(uint32_t o) const {
        if (o >= bytes.size()) return 0;
        if (!eeprom) return bytes[o];
        const int k = o >= regionStart[2] ? 2 : o >= regionStart[1] ? 1 : 0;
        const size_t n = eeprom->size();
        return n ? (*eeprom)[(regionBase[k] + (o - regionStart[k])) % n] : 0;
    }
    uint32_t eepromAddressOf(uint32_t o) const { const int k = o >= regionStart[2] ? 2 : o >= regionStart[1] ? 1 : 0; return regionBase[k] + (o - regionStart[k]); }
    // Entry `idx` of table `t` (entryBytes wide); false if it lies outside the table.
    bool entry(int t, uint32_t idx, int entryBytes, uint32_t& v) const;
};
// Table order of ImageLayout (spec 3.1)
enum MainTable { MT_MOVE = 0, MT_BRANCH = 1, MT_CSR1 = 2, MT_CSRM = 3, MT_ALU = 4, MT_CONST = 5 };
enum PeriTable { PT_XFER1 = 0, PT_XFERM = 1, PT_PUB = 2, PT_BRANCH = 3, PT_CSR1 = 4, PT_DEVMAP = 5 };


}  // namespace asmcore
