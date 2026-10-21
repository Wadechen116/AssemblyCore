; 03_call_models - CALL / RET with 8051-style procedures. Build it with both call models (Build menu) and compare the
; code size, the .map and the WCET:  inline expands every CALL, softret keeps one copy and returns by comparing a return id.
.include "defs.inc"

.main
.proc leaf                       ; a leaf procedure
    EXEC_CSR WRITE8 AE_CTRL, 0x11
    RET
.endp

.proc mid                        ; calls another procedure
    EXEC_CSR WRITE8 AE_CTRL, 0x21
    CALL leaf
    EXEC_CSR WRITE8 AE_CTRL, 0x22
    RET
.endp

start:
    CALL leaf
    CALL mid
    CALL leaf                    ; leaf now has three call sites
    EXEC_CSR WRITE8 AE_CTRL, 0x33
done:
    JMP done

.wcet one_pass, start, done      ; worst-case cycles between two labels (placeholder latencies)
