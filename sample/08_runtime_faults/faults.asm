; 08_runtime_faults - a program that compiles (with warnings) but FAULTS at run time, to test the fault display, "Clear fault",
; the State panel and the debugger. The mode is read from the CSR MODE (0x050): set it before running, or while stopped.
;   mode 1: consume the result slot while it is empty            -> fault_slot_not_valid
;   mode 2: write a CSR that the environment does not define     -> fault_csr_unmapped   (environment: unmapped_csr = fault)
;   mode 3: MOV from CSR read-back bytes that were never read    -> fault_csrbuf_invalid (profile: csr_buf_valid = true)
;   other : no fault; AE_CTRL = 0x77 and the program halts
.include "defs.inc"

.csr MODE    = 0x050
.csr BAD_CSR = 0x7F0                               ; not defined in faults_env.cfg

.main
start:
    EXEC_CSR READ8 MODE -> MAIN_CSR_DATA_BUF[0]
    MOV MAIN_CSR_DATA_BUF[0..0] -> MAIN_SCRATCHPAD[0..0]
    MOV CONST(0, 1) -> MAIN_SCRATCHPAD[1..1]       ; zero-extend the mode byte to 16 bits
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA
    MOV CONST(1, 2) -> MAIN_SCRATCHPAD[4..5]
    MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB
    ALU CMP.EQ unsigned
    BR.COND ALU_CMP_TRUE, empty_slot               ; mode == 1
    MOV CONST(2, 2) -> MAIN_SCRATCHPAD[4..5]
    MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB
    ALU CMP.EQ unsigned
    BR.COND ALU_CMP_TRUE, unmapped_csr             ; mode == 2
    MOV CONST(3, 2) -> MAIN_SCRATCHPAD[4..5]
    MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB
    ALU CMP.EQ unsigned
    BR.COND ALU_CMP_TRUE, stale_readback           ; mode == 3
    JMP ok_path

empty_slot:
    MOV PERI_RESULT_SLOT[0..0] -> MAIN_SCRATCHPAD[8..8]     ; nothing was published (the assembler warns: W505)
    JMP ok_path
unmapped_csr:
    EXEC_CSR WRITE8 BAD_CSR, 1
    JMP ok_path
stale_readback:
    MOV MAIN_CSR_DATA_BUF[8..9] -> MAIN_SCRATCHPAD[10..11]  ; bytes 8..9 were never read by EXEC_CSR (the assembler warns: W502)
    JMP ok_path

ok_path:
    EXEC_CSR WRITE8 AE_CTRL, 0x77
done:
    JMP done
