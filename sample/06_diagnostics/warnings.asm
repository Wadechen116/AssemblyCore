; 06_diagnostics - a program that BUILDS but triggers (almost) every warning, to test the Problems panel, the editor
; squiggles, hover text, "go to line", --no-warn / suppress, the data-flow switch and werror.
; Each warning is marked with its code in a comment. Open the Problems panel and compare. (asmc --explain CODE says more.)
.csr PERI_CTRL = 0x302
.csr POLL_CTRL = 0x301
.csr OUT_REG   = 0x120
.service GYRO  = 1, len=6, big               ; the payload contract: 6 bytes, high byte first
.service TEMP  = 2                           ; published by Peri but never handled by Main   -> W510
.csrbit PERI_CTRL[3] = timer_start
.csrbit POLL_CTRL[1] = poll_arm
.csrbit POLL_CTRL[0] = poll_clear
.devmap 0, I2C, 0x68, DEV_IMU
.devmap 2, I2C, 0x69, DEV_OTHER              ; entry 1 is missing                            -> W404

.main
.vector 0, on_event                          ; the foreground has a path around the loop that has no 'safe' branch -> W513 (no bound)

.proc never_called                           ; nobody calls it                                -> W401
    EXEC_CSR WRITE8 OUT_REG, 1
    RET
.endp

start:
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[30..31]   ; ALU_OUT read before any ALU operation produced it -> W503
    BR.COND ALU_CMP_TRUE, start              ; the compare flag is read, but no CMP ran        -> W506
    ALU ADD unsigned                         ; ALU_OPA / ALU_OPB were never loaded            -> W501
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[0..1]
    MOV MAIN_SCRATCHPAD[6..7] -> ALU_OPA     ; scratchpad bytes never written                 -> W502
    MOV ALU_STATUS[0..0] -> MAIN_SCRATCHPAD[2..2]   ; status read, but this path ran an ALU op, fine
    ALU AND signed                           ; LOGIC has no signed mode                       -> W410
    ALU ADD unsigned
    ALU ADD unsigned                         ; the first result is overwritten unread         -> W504
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[4..5]
    BR.COND ALU_OUT_OV16, start              ; reads a flag                                   (ok after the ALU ops above)
    MOV PERI_RESULT_SLOT[0..5] -> MAIN_SCRATCHPAD[8..13]   ; consumes the slot without testing PERI_RESULT_VALID -> W505
    MOV MAIN_SCRATCHPAD[8..9] -> ALU_OPA     ; GYRO is high byte first: byte-swapped in the ALU                   (W514 needs the service id known; see handler)
    BR.COND PERI_RESULT_VALID, handler
wait:
    BR.COND PERI_ERROR, wait, safe           ; a checkpoint: on_event below overwrites ALU_OPA, which 'start' reads again -> W512
    BR.COND PERI_RESULT_OVERRUN, cas
    JMP start
unreachable_here:
    EXEC_CSR WRITE8 OUT_REG, 2               ; nothing branches here                          -> W402
    JMP start

handler:
    MOV PERI_RESULT_META[0..1] -> ALU_OPA
    MOV CONST(GYRO, 2) -> MAIN_SCRATCHPAD[20..21]
    MOV MAIN_SCRATCHPAD[20..21] -> ALU_OPB
    ALU CMP.EQ unsigned
    BR.COND ALU_CMP_TRUE, handle_gyro
    JMP start
handle_gyro:
    MOV PERI_RESULT_SLOT[0..4] -> MAIN_SCRATCHPAD[0..4]    ; Peri publishes 4 or 6 bytes, the contract says 6, this consumes 5 -> W507, W517
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA     ; high byte first straight into the little-endian ALU                  -> W514
    JMP start

cas:
    CASCADE_BEGIN 1, cb0                     ; a cascade whose blocks leave to different labels -> W405
cb0:
    EXEC_CSR WRITE8 OUT_REG, 3
    CASCADE_END 0, ex1
cb1:
    EXEC_CSR WRITE8 OUT_REG, 4
    CASCADE_END 1, ex2
ex1:
    JMP start
ex2:
    JMP start

on_event:
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA     ; overwrites ALU_OPA, which the code after the safe branch at 'wait' reads again
    BR.ERET

.peri
.vector 0, peri_event
p_start:
    BR.IF_TIMER_DONE p_timeout               ; the timer was never started on this path       -> W515
    BR.IF_READY p_data                       ; poll result tested without arm + READ          -> W516
    EXEC_PUB service=GYRO, src=0, len=4      ; RX bytes never read in; contract says 6        -> W508, W517
    EXEC_PUB service=TEMP, src=0, len=2      ; no Main handler                                -> W510
    EXEC_PUB service=TEMP, src=0, len=3      ; same id, other length                          -> W511
p_data:
    EXEC_XFER READ dev=0, len=6, ptr=0, ext=[0x43]
    EXEC_PUB service=GYRO, src=0, len=6
    JMP p_start
p_timeout:
    JMP p_start
peri_event:
    EXEC_CSR SET8 POLL_CTRL, 1
    BR.ERET
