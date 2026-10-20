; CALL / RET with 8051-style procedures. Assemble it twice and compare:
;   asmc --call-model inline  --list --map examples/call_models.asm -o out_inline
;   asmc --call-model softret --list --map examples/call_models.asm -o out_softret
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
done:
    JMP done

.wcet one_pass, start, done      ; worst-case cycles between two labels (placeholder latencies)
