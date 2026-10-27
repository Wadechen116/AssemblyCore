; 13_block_editor - a small program to practise the block editor on (see README).
.include "defs.inc"
    LIMIT EQU 5    ; an 8051-style constant: a line the blocks do not cover is kept as text (a grey block)

.main
start:
    EXEC_CSR WRITE8 AE_CTRL, 0x01    ; switch the block on
    MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]
    MOV CONST(1, 2) -> MAIN_SCRATCHPAD[2..3]
    MOV CONST(0, 2) -> MAIN_SCRATCHPAD[26..27]
    MOV CONST(1, 2) -> MAIN_SCRATCHPAD[28..29]
    MOV CONST(5, 2) -> MAIN_SCRATCHPAD[30..31]
loop_b39:
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA
    MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB
    ALU ADD unsigned
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[0..1]
    MOV MAIN_SCRATCHPAD[26..27] -> ALU_OPA
    MOV MAIN_SCRATCHPAD[28..29] -> ALU_OPB
    ALU ADD unsigned
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[26..27]
    MOV MAIN_SCRATCHPAD[26..27] -> ALU_OPA
    MOV MAIN_SCRATCHPAD[30..31] -> ALU_OPB
    ALU CMP.EQ unsigned
    BR.COND ALU_CMP_TRUE, loop_b39_end
    JMP loop_b39
loop_b39_end:
done:
    JMP done
