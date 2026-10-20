#pragma once
// Cycle costs shared by the assembler (listing, WCET) and the simulator, so the two cannot drift apart: spec 14.6 requires
// "Tool and RTL latency model must be consistent".
//
// The defaults are the PLANNING model of spec 14.3 / 14.4. The spec states that the final table is an implementation
// decision (7.4.5), so every number is a parameter; replace them through a profile file (asmc --profile / [latency] keys).
#include <cstdint>

namespace asmcore {

struct Latency {
    int brCond = 1;               // BR.COND, taken or not
    int cascade = 1;              // CASCADE_BEGIN / CASCADE_END
    int alu = 1;                  // ADD / SUB / CMP / LOGIC / SHIFT
    int mul = 2;                  // MUL_CYC (planning estimate)
    int movSetup = 1, movByte = 1;          // MOV: 1 + len
    int slotConsumeSetup = 2;               // MOV from PERI_RESULT_SLOT: 2 + len
    int csrAccess = 2;            // CSR_ACCESS_CYC; one-byte EXEC_CSR = 1 + csrAccess (RMW: read + write)
    int csrByte = 2, csrDone = 1; // multi-byte EXEC_CSR = 1 + len * csrByte + csrDone
    int pubSetup = 2, pubByte = 1;          // EXEC_PUB: 2 + len
    int eventDispatch = 2, eret = 2;        // redirect to the handler vector / back to the resume PC (planning numbers; v31 16.9 needs RTL bounds)
    int xferIssue = 2;            // EXEC_XFER issue overhead, before the external bus time
    int cpuHz = 27000000;         // ISA clock (spec 14.1)
    int i2cHz = 400000;           // I2C clock; 400 kHz = 67.5 ISA cycles per SCL pulse (spec 14.4)

    int mov(int len, bool consumesSlot) const { return (consumesSlot ? slotConsumeSetup : movSetup) + movByte * len; }
    int csrOne(int csrOp) const {                       // csrOp: 0 WRITE8, 1 RMW_SET8, 2 RMW_CLR8, 3 READ8
        return (csrOp == 1 || csrOp == 2) ? 1 + 2 * csrAccess : 1 + csrAccess;
    }
    int csrMulti(int len) const { return 1 + len * csrByte + csrDone; }
    int aluOp(bool isMul) const { return isMul ? mul : alu; }
    int pub(int len) const { return pubSetup + pubByte * len; }
    // ISA cycles of `phases` I2C byte phases (9 SCL pulses each: 8 data bits + ACK), rounded up.
    int64_t i2cCycles(int phases) const {
        if (i2cHz <= 0) return 0;
        return ((int64_t)9 * phases * cpuHz + i2cHz - 1) / i2cHz;
    }
    // Byte phases of one register transaction: address+W, register address bytes, [address+R], data bytes.
    static int xferPhases(bool read, int addrLen, int dataLen) {
        if (read) return (addrLen > 0 ? 1 + addrLen : 0) + 1 + dataLen;
        return 1 + addrLen + dataLen;
    }
    int64_t xfer(bool read, int addrLen, int dataLen) const { return xferIssue + i2cCycles(xferPhases(read, addrLen, dataLen)); }
};

}  // namespace asmcore
