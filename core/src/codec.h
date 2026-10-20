#pragma once
// The bit layouts of every instruction byte and descriptor, in ONE place.
//
// The assembler packs, the simulator and the disassembler unpack, and the listing decodes: all four go through this file, so a
// change of the spec's bit positions is a change here and nowhere else. Every struct is a plain bag of fields; `rsv` fields hold
// the bits the spec says must be zero, so a reader can report a violation without knowing where those bits are.
//
// pack(unpack(x)) == x for every 16/32-bit value (checked by tests/main.cpp), so a descriptor read from an image can always be
// written back unchanged.
#include <cstdint>

#include "asmcore/asmcore.h"

namespace asmcore {
namespace codec {

// ---- instruction byte (spec 2): [7:6] opcode, [5:0] arg ---------------------------------------------------------------------------
inline int opcode(uint8_t b) { return b >> 6; }
inline int arg(uint8_t b) { return b & 63; }
inline uint8_t instrByte(int opcode, int arg) { return (uint8_t)((opcode << 6) | (arg & 63)); }
// EXEC_CSR / EXEC_XFER: arg[5] selects the MultiByte form, arg[4:0] is the template index
inline bool isMulti(int arg) { return (arg & 32) != 0; }
inline int templateIndex(int arg) { return arg & 31; }
inline int multiArg(bool multi, int idx) { return (multi ? 32 : 0) | (idx & 31); }

// ---- MainMoveDesc_32b (spec 5.1) --------------------------------------------------------------------------------------------------
struct Move {
    int srcRegion = 0, dstRegion = 0, srcOff = 0, dstOff = 0;
    int len = 1;                  // bytes (the field holds len - 1)
    uint32_t rsv = 0;             // [7:5]
};
inline uint32_t pack(const Move& m) {
    return ((uint32_t)m.srcRegion << 28) | ((uint32_t)m.dstRegion << 24) | ((uint32_t)m.srcOff << 16) | ((uint32_t)m.dstOff << 8) | (m.rsv << 5) | (uint32_t)(m.len - 1);
}
inline Move unpackMove(uint32_t d) {
    Move m;
    m.srcRegion = (int)(d >> 28);
    m.dstRegion = (int)((d >> 24) & 15);
    m.srcOff = (int)((d >> 16) & 255);
    m.dstOff = (int)((d >> 8) & 255);
    m.rsv = (d >> 5) & 7;
    m.len = (int)(d & 31) + 1;
    return m;
}

// ---- Main ALU, MainAluDesc_8b (spec 6.3) --------------------------------------------------------------------------------------------
struct Alu {
    int cls = 0;                  // 0 ARITH, 1 CMP, 2 LOGIC, 3 SHIFT
    int signedMode = 0;           // ARITH, CMP, SHIFT
    int op = 0;                   // arith_op / cmp_op / logic_op
    int dir = 0;                  // SHIFT: 0 SHL, 1 SHR
    int amt = 0;                  // SHIFT amount
    uint32_t rsv = 0;             // CMP / LOGIC: [2:0]
};
// `op` below is always the LOGICAL operation (ARITH_ADD, CMP_EQ, LOGIC_AND ... of isa.h); the numeric code in the descriptor comes from the
// AluCodes of the target profile (spec v31 6.3 gives names and bit fields, not numbers).
inline uint32_t packArith(const AluCodes& m, int sg, int op) { return (uint32_t)((0 << 6) | (sg << 5) | (m.arith[op] & 31)); }
inline uint32_t packCmp(const AluCodes& m, int sg, int op) { return (uint32_t)((1 << 6) | (sg << 5) | ((m.cmp[op] & 3) << 3)); }
inline uint32_t packLogic(const AluCodes& m, int op) { return (uint32_t)((2 << 6) | ((m.logic[op] & 7) << 3)); }
inline uint32_t packShift(int sg, int dir, int amt) { return (uint32_t)((3 << 6) | (sg << 5) | (dir << 4) | (amt & 15)); }
// The logical operation for a numeric code, or -1 if the profile assigns no operation to it (a reserved encoding).
inline int logicalOf(const int* codes, int n, int numeric) {
    for (int i = 0; i < n; i++) if (codes[i] == numeric) return i;
    return -1;
}
inline Alu unpackAlu(uint32_t d, const AluCodes& m) {
    Alu a;
    a.cls = (int)((d >> 6) & 3);
    switch (a.cls) {
        case 0: a.signedMode = (int)((d >> 5) & 1); a.op = logicalOf(m.arith, 5, (int)(d & 31)); break;
        case 1: a.signedMode = (int)((d >> 5) & 1); a.op = logicalOf(m.cmp, 4, (int)((d >> 3) & 3)); a.rsv = d & 7; break;
        case 2: a.op = logicalOf(m.logic, 4, (int)((d >> 3) & 7)); a.rsv = d & 7; break;
        default: a.signedMode = (int)((d >> 5) & 1); a.dir = (int)((d >> 4) & 1); a.amt = (int)(d & 15); break;
    }
    return a;
}
inline uint32_t pack(const Alu& a, const AluCodes& m) {
    switch (a.cls) {
        case 0: return packArith(m, a.signedMode, a.op);
        case 1: return packCmp(m, a.signedMode, a.op) | a.rsv;
        case 2: return packLogic(m, a.op) | a.rsv;
        default: return packShift(a.signedMode, a.dir, a.amt);
    }
}

// ---- branches (spec 7.2, 11.2) --------------------------------------------------------------------------------------------------------
struct MainBranch {               // MainBranchDesc_16b (v31 7.2)
    int type = 0;                 // 0 COND, 1 CASCADE_BEGIN, 2 CASCADE_END, 3 ERET
    int arg = 0;                  // COND: cond_sel;  BEGIN: selector;  END: block index;  ERET: 0
    uint32_t eventSafe = 0;       // [9]  the branch is an event checkpoint (ordinary BR.COND only)
    uint32_t rsv = 0;             // [8]
    uint32_t target = 0;          // [7:0]
};
inline uint32_t pack(const MainBranch& b) { return ((uint32_t)b.type << 14) | ((uint32_t)b.arg << 10) | ((b.eventSafe & 1) << 9) | ((b.rsv & 1) << 8) | (b.target & 255); }
inline MainBranch unpackMainBranch(uint32_t d) {
    MainBranch b;
    b.type = (int)((d >> 14) & 3);
    b.arg = (int)((d >> 10) & 15);
    b.eventSafe = (d >> 9) & 1;
    b.rsv = (d >> 8) & 1;
    b.target = d & 255;
    return b;
}
struct PeriBranch {               // PeriBranchDesc_16b (v31 11.2)
    int type = 0;                 // 0 ALWAYS .. 7 IF_TIMER_NOT_DONE, 8 ERET
    uint32_t eventSafe = 0;       // [11]
    uint32_t rsv = 0;             // [10:8]
    uint32_t target = 0;
};
inline uint32_t pack(const PeriBranch& b) { return ((uint32_t)b.type << 12) | ((b.eventSafe & 1) << 11) | ((b.rsv & 7) << 8) | (b.target & 255); }
inline PeriBranch unpackPeriBranch(uint32_t d) {
    PeriBranch b;
    b.type = (int)((d >> 12) & 15);
    b.eventSafe = (d >> 11) & 1;
    b.rsv = (d >> 8) & 7;
    b.target = d & 255;
    return b;
}

// ---- EXEC_CSR (spec 4.2, 4.3) ---------------------------------------------------------------------------------------------------------
struct Csr1 {                     // CsrOneByteDesc_16b
    int op = 0;                   // 0 WRITE8_IMM, 1 RMW_SET8_IMM, 2 RMW_CLR8_IMM, 3 READ8
    uint32_t rsv = 0;             // [13:12]
    uint32_t addr = 0;            // [11:0]
};
inline uint32_t pack(const Csr1& c) { return ((uint32_t)c.op << 14) | (c.rsv << 12) | (c.addr & 0xFFF); }
inline Csr1 unpackCsr1(uint32_t d) {
    Csr1 c;
    c.op = (int)((d >> 14) & 3);
    c.rsv = (d >> 12) & 3;
    c.addr = d & 0xFFF;
    return c;
}
struct CsrM {                     // CsrMultiByteDesc_16b
    int op = 0;                   // 0 WRITE_SP, 3 READ_BUF (1, 2 reserved)
    int page = 0;                 // csr_addr[11:8]; csr_addr[7:0] is the byte after the instruction
    int len = 1;                  // bytes (the field holds len - 1)
    int ptr = 0;                  // WRITE_SP: MAIN_SCRATCHPAD source pointer;  READ_BUF: MAIN_CSR_DATA_BUF destination pointer
};
inline uint32_t pack(const CsrM& c) { return ((uint32_t)c.op << 14) | ((uint32_t)(c.page & 15) << 10) | ((uint32_t)(c.len - 1) << 5) | (uint32_t)(c.ptr & 31); }
inline CsrM unpackCsrM(uint32_t d) {
    CsrM c;
    c.op = (int)((d >> 14) & 3);
    c.page = (int)((d >> 10) & 15);
    c.len = (int)((d >> 5) & 31) + 1;
    c.ptr = (int)(d & 31);
    return c;
}

// ---- EXEC_PUB (spec 10.3) --------------------------------------------------------------------------------------------------------------
struct Pub {                      // PeriPublishDesc_16b
    int service = 0, src = 0;
    int len = 1;                  // bytes (the field holds len - 1)
    uint32_t rsv = 0;             // [1:0]
};
inline uint32_t pack(const Pub& p) { return ((uint32_t)p.service << 12) | ((uint32_t)p.src << 7) | ((uint32_t)(p.len - 1) << 2) | p.rsv; }
inline Pub unpackPub(uint32_t d) {
    Pub p;
    p.service = (int)((d >> 12) & 15);
    p.src = (int)((d >> 7) & 31);
    p.len = (int)((d >> 2) & 31) + 1;
    p.rsv = d & 3;
    return p;
}

// ---- EXEC_XFER (spec 8.3, 8.5) -----------------------------------------------------------------------------------------------------------
struct Xfer1 {                    // PeriXferOneByteDesc_16b
    int rw = 0;                   // 0 WRITE, 1 READ
    int dev = 0, addrLen = 0;     // addr_len 3 is reserved
    uint32_t addr = 0;            // [9:0]
};
inline uint32_t pack(const Xfer1& x) { return ((uint32_t)x.rw << 15) | ((uint32_t)x.dev << 12) | ((uint32_t)x.addrLen << 10) | (x.addr & 0x3FF); }
inline Xfer1 unpackXfer1(uint32_t d) {
    Xfer1 x;
    x.rw = (int)((d >> 15) & 1);
    x.dev = (int)((d >> 12) & 7);
    x.addrLen = (int)((d >> 10) & 3);
    x.addr = d & 0x3FF;
    return x;
}
struct XferM {                    // PeriXferMultiByteDesc_16b
    int rw = 1, dev = 0, addrLen = 0;
    int len = 1;                  // bytes (the field holds len - 1)
    int ptr = 0;
};
inline uint32_t pack(const XferM& x) { return ((uint32_t)x.rw << 15) | ((uint32_t)x.dev << 12) | ((uint32_t)x.addrLen << 10) | ((uint32_t)(x.len - 1) << 5) | (uint32_t)(x.ptr & 31); }
inline XferM unpackXferM(uint32_t d) {
    XferM x;
    x.rw = (int)((d >> 15) & 1);
    x.dev = (int)((d >> 12) & 7);
    x.addrLen = (int)((d >> 10) & 3);
    x.len = (int)((d >> 5) & 31) + 1;
    x.ptr = (int)(d & 31);
    return x;
}

// ---- DeviceMap entry (spec 9.1) ----------------------------------------------------------------------------------------------------------
struct DevEntry {
    int busType = 0;              // 0 = I2C
    int addr7 = 0;
};
// v31 9.1: only the LOGICAL fields are defined (bus_type, i2c_addr_7b); the entry width and the bit positions come from the target profile
// (device_map_entry_bytes, device_map_bus_shift, device_map_addr_shift). Defaults: 1 byte, bus_type at bit 7, address at bits 6:0 (ASSUMED).
inline uint32_t packDev(const Profile& pf, const DevEntry& e) {
    return ((uint32_t)(e.busType & 1) << pf.deviceMapBusShift) | ((uint32_t)(e.addr7 & 0x7F) << pf.deviceMapAddrShift);
}
inline DevEntry unpackDev(const Profile& pf, uint32_t v) {
    DevEntry e;
    e.busType = (int)((v >> pf.deviceMapBusShift) & 1);
    e.addr7 = (int)((v >> pf.deviceMapAddrShift) & 0x7F);
    return e;
}

}  // namespace codec
}  // namespace asmcore
