; 04_alu_cascade - every ALU family, the constant pool, and a CASCADE (fixed-length multi-block sequence).
.include "defs.inc"

.csr RESULT = 0x140                       ; 16 result bytes are written here with WRITE_SP
.const K_A dw 300                         ; a 16-bit constant in MAIN_CONST_POOL (referenced with @K_A)
.const K_B dw 25

.main
start:
    MOV MAIN_CONST_POOL[@K_A] -> MAIN_SCRATCHPAD[0..1]       ; A = 300
    MOV MAIN_CONST_POOL[@K_B] -> MAIN_SCRATCHPAD[2..3]       ; B = 25
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA
    MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB
    ALU ADD unsigned
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[8..9]               ; 325
    ALU SUB unsigned
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[10..11]             ; 275
    ALU MUL unsigned
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[12..13]             ; 7500 (low 16 bits of the product)
    ALU AND
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[14..15]             ; 300 & 25  = 8
    ALU OR
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[16..17]             ; 300 | 25  = 317
    ALU XOR
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[18..19]             ; 300 ^ 25  = 309
    ALU SHL 2 unsigned
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[20..21]             ; 300 << 2  = 1200
    ALU SHR 3 unsigned
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[22..23]             ; 300 >> 3  = 37
    ALU CMP.LT unsigned                                      ; 300 < 25 ? (CMP leaves ALU_OUT alone, it only sets the flag)
    BR.COND ALU_CMP_TRUE, less
    ALU CMP.GE unsigned                                      ; 300 >= 25 ?
    BR.COND ALU_CMP_TRUE, greater
    JMP done
less:
    EXEC_CSR WRITE8 AE_CTRL, 0xEE                            ; must not happen
    JMP done
greater:
    EXEC_CSR WRITE8 AE_CTRL, 0x01
    CASCADE_BEGIN 2, block0                                  ; run blocks 0, 1 and 2 in order (selector 2)
block0:
    EXEC_CSR SET8 AE_CTRL, 0x02
    CASCADE_END 0, after
block1:
    EXEC_CSR SET8 AE_CTRL, 0x04
    CASCADE_END 1, after
block2:
    EXEC_CSR SET8 AE_CTRL, 0x08
    CASCADE_END 2, after
after:
    EXEC_CSR WRITE_SP RESULT, len=16, ptr=8                  ; all the results to the CSR window in one transaction
done:
    JMP done
