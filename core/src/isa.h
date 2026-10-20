#pragma once
// ISA encoding tables for Sensor Micro-Sequencer ISA v1.6 r10.
//
// Everything the spec text pins down is encoded as written. Everything it does NOT pin down is marked ASSUMED and collected
// here, in one place, so a correction is a one-line change:
//
//   ASSUMED-1  numeric codes of arith_op / cmp_op / logic_op / shift_dir (spec 6.3 lists the names, not the values):
//              taken in the order the spec lists them.
//   ASSUMED-2  byte order of multi-byte descriptors in the image (little-endian, like every other multi-byte item in the spec).
//   ASSUMED-3  width of each ImageLayout offset field (profile.layoutOffsetBytes, default 2) and the order of the fields (the order
//              in which spec 3.1 lists them).
//   ASSUMED-4  DeviceMap entry byte: bit 7 = bus_type, bits 6:0 = i2c_addr_7b (spec 9.1 gives the widths, not the packing).
//   ASSUMED-5  PERI_RESULT_META / ALU_STATUS bit and byte layouts are not needed by the assembler.
#include <cstdint>
#include <string>

namespace asmcore {
namespace isa {

// ---- opcodes ([7:6] of the instruction byte) --------------------------------------------------------------------------------
enum MainOpcode { M_CSR = 0, M_BR = 1, M_MOV = 2, M_ALU = 3 };
enum PeriOpcode { P_CSR = 0, P_BR = 1, P_PUB = 2, P_XFER = 3 };

// ---- Main MOV regions (spec 5.1) ---------------------------------------------------------------------------------------------
enum Region {
    R_SCRATCH = 0x0, R_CSRBUF = 0x1, R_SLOT = 0x2, R_OPA = 0x3, R_OPB = 0x4, R_OUT = 0x5, R_STATUS = 0x6, R_CONST = 0x7, R_META = 0x8
};
struct RegionInfo {
    const char* name;
    int id;
    bool readable, writable;
    int size;   // bytes addressable by an offset; 0 = determined elsewhere (const pool)
};
const RegionInfo kRegions[] = {
    {"MAIN_SCRATCHPAD", R_SCRATCH, true, true, 32},
    {"MAIN_CSR_DATA_BUF", R_CSRBUF, true, false, 32},        // v31 5.1: MOV source only; written by EXEC_CSR READ8 / READ_BUF
    {"PERI_RESULT_SLOT", R_SLOT, true, false, 32},
    {"ALU_OPA", R_OPA, false, true, 2},
    {"ALU_OPB", R_OPB, false, true, 2},
    {"ALU_OUT", R_OUT, true, false, 4},
    {"ALU_STATUS", R_STATUS, true, false, 4},
    {"MAIN_CONST_POOL", R_CONST, true, false, 0},
    {"PERI_RESULT_META", R_META, true, false, 32},
};
inline const RegionInfo* regionByName(const std::string& upperName) {
    static const struct { const char* alias; int id; } kAlias[] = {
        {"SCRATCHPAD", R_SCRATCH}, {"SCRATCH", R_SCRATCH}, {"CSR_DATA_BUF", R_CSRBUF}, {"CSRBUF", R_CSRBUF},
        {"RESULT_SLOT", R_SLOT}, {"SLOT", R_SLOT}, {"CONST_POOL", R_CONST}, {"CONST", R_CONST}, {"RESULT_META", R_META}, {"META", R_META},
        {"OPA", R_OPA}, {"OPB", R_OPB}, {"OUT", R_OUT}, {"STATUS", R_STATUS},
    };
    for (const RegionInfo& r : kRegions) if (upperName == r.name) return &r;
    for (auto& a : kAlias) if (upperName == a.alias) return &kRegions[a.id];
    return nullptr;
}

// ---- Main ALU (spec 6.3) -----------------------------------------------------------------------------------------------------
enum AluClass { A_ARITH = 0, A_CMP = 1, A_LOGIC = 2, A_SHIFT = 3 };
enum ArithOp { ARITH_ADD = 0, ARITH_SUB = 1, ARITH_MUL = 2, ARITH_INC = 3, ARITH_DEC = 4 };   // ASSUMED-1
enum CmpOp { CMP_EQ = 0, CMP_NE = 1, CMP_LT = 2, CMP_GE = 3 };                                 // ASSUMED-1
enum LogicOp { LOGIC_AND = 0, LOGIC_OR = 1, LOGIC_XOR = 2, LOGIC_NOT = 3 };                    // ASSUMED-1
enum ShiftDir { SHIFT_SHL = 0, SHIFT_SHR = 1 };                                                // ASSUMED-1

// ---- Main BR (spec 7.2, 7.3) -------------------------------------------------------------------------------------------------
enum MainBrType { MB_COND = 0, MB_CASCADE_BEGIN = 1, MB_CASCADE_END = 2, MB_ERET = 3 };   // v31 7.2: 3 = BR.ERET
const int PB_ERET = 8;                                                                  // v31 11.2: Peri br_type 8 = BR.ERET
struct NamedCode { const char* name; int code; };
const NamedCode kMainConds[] = {
    {"ALWAYS", 0x0}, {"PERI_RESULT_VALID", 0x1}, {"PERI_RESULT_OVERRUN", 0x2}, {"PERI_ERROR", 0x3}, {"PERI_NO_ERROR", 0x4},
    {"ALU_CMP_TRUE", 0x5}, {"ALU_OUT_OV16", 0x6}, {"MAIN_TIMER_DONE", 0x7}, {"FRAME_END_EVT", 0x8}, {"VBLANK_OPEN", 0x9},
    {"HBLANK_OPEN", 0xA}, {"PGL_COMMIT_ELIGIBLE", 0xB}, {"AE_COMMIT_ELIGIBLE", 0xC}, {"LINE_COMMIT_ELIGIBLE", 0xD},
    {"PROFILE_COND0", 0xE},
};
// ---- Peri BR (spec 11.2) -----------------------------------------------------------------------------------------------------
const NamedCode kPeriConds[] = {
    {"ALWAYS", 0x0}, {"IF_READY", 0x1}, {"IF_NOT_READY", 0x2}, {"IF_TIMEOUT", 0x3}, {"IF_ERROR", 0x4}, {"IF_NO_ERROR", 0x5},
    {"IF_TIMER_DONE", 0x6}, {"IF_TIMER_NOT_DONE", 0x7},
};

// ---- CSR (spec 4) ------------------------------------------------------------------------------------------------------------
enum CsrOneOp { CSR1_WRITE8 = 0, CSR1_SET8 = 1, CSR1_CLR8 = 2, CSR1_READ8 = 3 };
enum CsrMultiOp { CSRM_WRITE_SP = 0, CSRM_READ_BUF = 3 };    // v31 4.3: WRITE_SP reads MAIN_SCRATCHPAD (v24 WRITE_BUF read MAIN_CSR_DATA_BUF)

// ---- DeviceMap (spec 9) ------------------------------------------------------------------------------------------------------
enum BusType { BUS_I2C = 0 };

}  // namespace isa
}  // namespace asmcore
