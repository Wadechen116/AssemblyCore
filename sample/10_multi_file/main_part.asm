; 10_multi_file / main_part.asm - the Main side. The project lists this file and peri_part.asm as separate sources.
; (shared.inc is listed first in the project, so its definitions are visible here without an .include)

.main
wait:
    BR.COND PERI_RESULT_VALID, check
    JMP wait
check:
    MOV PERI_RESULT_META[0..1] -> ALU_OPA
    MOV CONST(SVC_TEMP, 2) -> MAIN_SCRATCHPAD[20..21]
    MOV MAIN_SCRATCHPAD[20..21] -> ALU_OPB
    ALU CMP.EQ unsigned
    BR.COND ALU_CMP_TRUE, handle_temp
    MOV PERI_RESULT_SLOT[0..0] -> MAIN_SCRATCHPAD[31]      ; unknown service: consume and drop
    JMP wait
handle_temp:
    MOV PERI_RESULT_SLOT[0..1] -> MAIN_SCRATCHPAD[0..1]    ; the contract: 2 bytes, low byte first (no swap needed)
    EXEC_CSR WRITE_SP TEMP_OUT, len=2, ptr=0
done:
    JMP done
