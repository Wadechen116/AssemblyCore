; 01_hello_csr - the smallest useful Main program.
; Counts a scratchpad counter up to 5 with the ALU, then writes the result to a CSR with EXEC_CSR WRITE_SP.
.include "defs.inc"

.main
start:
    EXEC_CSR WRITE8 AE_CTRL, 0x01         ; one-byte CSR forms: WRITE8 / SET8 / CLR8
    EXEC_CSR SET8   AE_CTRL, 0x10
    EXEC_CSR CLR8   AE_CTRL, 0x01
    MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]          ; counter = 0 (constants go to the scratchpad first)
    MOV CONST(1, 2) -> MAIN_SCRATCHPAD[2..3]          ; step = 1
    MOV CONST(5, 2) -> MAIN_SCRATCHPAD[4..5]          ; limit = 5
loop:
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA
    MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB
    ALU ADD unsigned
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[0..1]        ; counter += step
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA
    MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB
    ALU CMP.EQ unsigned                               ; counter == limit ?
    BR.COND ALU_CMP_TRUE, finish
    JMP loop
finish:
    EXEC_CSR WRITE_SP GYRO_X_CORR_L, len=2, ptr=0     ; v31: the CSR write reads the scratchpad directly
done:
    JMP done                                          ; a branch to itself = the program has finished (Main halts)
