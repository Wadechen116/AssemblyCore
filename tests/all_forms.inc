// Shared by the disassembler tests and the decoder cross-check: one program with every instruction form of both domains.
static const char* kAll = R"(
.csr C1 = 0x120
.csr C2 = 0x240
.csr PM = 0x300
.service SID = 3
.devmap 0, I2C, 0x68
.devmap 1, I2C, 0x48
.const K1 dw 0x1234
.const K2 db 1, 2, 3
.main
top:
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA
    MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB
    ALU ADD unsigned
    ALU ADD signed
    ALU SUB unsigned
    ALU SUB signed
    ALU MUL unsigned
    ALU MUL signed
    ALU INC unsigned
    ALU DEC signed
    ALU CMP.EQ unsigned
    ALU CMP.NE unsigned
    ALU CMP.LT unsigned
    ALU CMP.LT signed
    ALU CMP.GE unsigned
    ALU CMP.GE signed
    ALU AND
    ALU OR
    ALU XOR
    ALU NOT
    ALU SHL 1 unsigned
    ALU SHL 8 signed
    ALU SHR 3 unsigned
    ALU SHR 8 signed
    MOV ALU_OUT[0..3] -> MAIN_SCRATCHPAD[8..11]
    MOV ALU_STATUS[0] -> MAIN_SCRATCHPAD[12]
    MOV PERI_RESULT_META[0..1] -> MAIN_SCRATCHPAD[0..1]
    MOV MAIN_CONST_POOL[@K1] -> MAIN_SCRATCHPAD[4..5]
    MOV MAIN_CONST_POOL[@K2] -> MAIN_SCRATCHPAD[6..8]
    MOV CONST(0xBEEF) -> MAIN_SCRATCHPAD[10..11]
    MOV PERI_RESULT_SLOT[0..5] -> MAIN_SCRATCHPAD[16..21]
    MOV MAIN_SCRATCHPAD[30..31] -> MAIN_SCRATCHPAD[22..23]
    MOV MAIN_CSR_DATA_BUF[0..31] -> MAIN_SCRATCHPAD[0..31]
    EXEC_CSR WRITE8 C1, 5
    EXEC_CSR SET8 C1, 0x80
    EXEC_CSR CLR8 C1, 1
    EXEC_CSR READ8 C1 -> MAIN_CSR_DATA_BUF[3]
    EXEC_CSR WRITE_SP C1, len=2, ptr=0
    EXEC_CSR READ_BUF C2, len=4, ptr=28
    BR.COND PERI_RESULT_VALID, c1
    BR.COND PERI_RESULT_OVERRUN, c1
    BR.COND PERI_ERROR, c1
    BR.COND PERI_NO_ERROR, c1
    BR.COND ALU_CMP_TRUE, c1
    BR.COND ALU_OUT_OV16, c1
    BR.COND MAIN_TIMER_DONE, c1
    BR.COND FRAME_END_EVT, c1
    BR.COND ALU_CMP_TRUE, c1, safe
    BR.COND VBLANK_OPEN, c1
    BR.COND HBLANK_OPEN, c1
    JMP c1, safe
c1:
    BR.COND PGL_COMMIT_ELIGIBLE, c2
    BR.COND AE_COMMIT_ELIGIBLE, c2
    BR.COND LINE_COMMIT_ELIGIBLE, c2
c2:
    CASCADE_BEGIN 2, b0
b0: EXEC_CSR WRITE8 C1, 1
    CASCADE_END 0, done
b1: EXEC_CSR WRITE8 C1, 2
    CASCADE_END 1, done
b2: EXEC_CSR WRITE8 C1, 3
    CASCADE_END 2, done
done:
    JMP top
.vector 0, hm
hm: BR.ERET
.peri
p0: EXEC_CSR WRITE8 PM, 1
    EXEC_CSR SET8 PM, 0x80
    EXEC_CSR CLR8 PM, 0x0F
    BR.IF_READY p1
    BR.IF_NOT_READY p1
    BR.IF_TIMEOUT p1
    BR.IF_ERROR p1
    BR.IF_NO_ERROR p1
    BR.IF_TIMER_DONE p1
    BR.IF_TIMER_NOT_DONE p1
    BR.IF_READY p1, safe
    BR.IF_ERROR p1, safe
    JMP p1, safe
p1: EXEC_PUB service=SID, src=0, len=6
    EXEC_PUB service=15, src=31, len=1
    EXEC_PUB service=1, src=0, len=32
    EXEC_XFER READ dev=0, addr=0x3A, addrlen=1, ptr=0
    EXEC_XFER WRITE dev=1, addr=0x6B, addrlen=1, data=0
    EXEC_XFER WRITE dev=0, data=0xFF
    EXEC_XFER READ dev=1, addr=0x123, addrlen=2, ptr=5
    EXEC_XFER READ dev=0, len=6, ptr=0, ext=[0x43]
    EXEC_XFER READ dev=1, len=3, ptr=8, ext=[0x12, 0x34]
    EXEC_XFER READ dev=0, len=32, ptr=0
    JMP p0
.vector 3, hp
hp: BR.ERET
)";
