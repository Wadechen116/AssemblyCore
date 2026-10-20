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

    bool load(const DomainImage& im, const Profile& pf, std::string& error);
    uint32_t u8(uint32_t o) const { return o < bytes.size() ? bytes[o] : 0; }
    // Entry `idx` of table `t` (entryBytes wide); false if it lies outside the table.
    bool entry(int t, uint32_t idx, int entryBytes, uint32_t& v) const;
};
// Table order of ImageLayout (spec 3.1)
enum MainTable { MT_MOVE = 0, MT_BRANCH = 1, MT_CSR1 = 2, MT_CSRM = 3, MT_ALU = 4, MT_CONST = 5 };
enum PeriTable { PT_XFER1 = 0, PT_XFERM = 1, PT_PUB = 2, PT_BRANCH = 3, PT_CSR1 = 4, PT_DEVMAP = 5 };


}  // namespace asmcore
