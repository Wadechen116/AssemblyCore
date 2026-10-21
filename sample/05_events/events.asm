; 05_events - hardware events (spec v31 16): .vector, event_safe branches ('safe'), handlers ending in BR.ERET.
; Main counts event 0 in scratch[10..11]; event 1 is an "error" handler. Peri has its own event 0.
; Inject events from the scenario (inject ... event main id=0) or from the Debug menu while running.
.include "defs.inc"

.csr PERI_EVT = 0x330

.main
.vector 0, on_frame                         ; event 0 of Main -> on_frame
.vector 1, on_error                         ; event 1 of Main -> on_error
start:
    MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]
fg_loop:
    BR.COND PERI_RESULT_VALID, got_result, safe    ; an ordinary BR.COND with event_safe: a pending event may be dispatched HERE
    JMP fg_loop
got_result:
    MOV PERI_RESULT_SLOT[0..0] -> MAIN_SCRATCHPAD[1..1]   ; (never taken in this sample: Peri publishes nothing)
    JMP fg_loop

on_frame:                                   ; handlers live after the main loop and are entered only through .vector
    MOV MAIN_SCRATCHPAD[10..11] -> ALU_OPA  ; hardware keeps no context: the handler uses state the foreground does not read
    MOV CONST(1, 2) -> MAIN_SCRATCHPAD[14..15]
    MOV MAIN_SCRATCHPAD[14..15] -> ALU_OPB
    ALU ADD unsigned
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[10..11]   ; events counted += 1
    BR.ERET                                 ; back to the interrupted flow, at the next PC the safe BR resolved

on_error:
    EXEC_CSR WRITE8 AE_CTRL, 0xFF
    BR.ERET

.peri
.vector 0, peri_evt
loop:
    EXEC_XFER READ dev=DEV_IMU, addr=0x3A, addrlen=1, ptr=0
    BR.IF_NO_ERROR loop, safe               ; the Peri checkpoint
    JMP loop

peri_evt:
    EXEC_CSR SET8 PERI_EVT, 0x01
    BR.ERET
