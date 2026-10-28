; 14_main_peri_sim - Main and Peri together, for the Simulator page (see README).
;
;   Peri  is TRIGGERED BY ITS TIMER: it starts the timer once, waits for it to run out, then reads the sensor over I2C (EXEC_XFER) and publishes the
;         6 bytes (EXEC_PUB) into the result slot.  It does this again and again.
;   Main  waits for the result slot (PERI_RESULT_VALID), takes the sample, settles for a while on ITS OWN TIMER, and then waits for the FRAME TIMING:
;         it commits the corrected value to a CSR only inside the blanking window with enough time left for the commit (PGL_COMMIT_ELIGIBLE,
;         which compares the window with the Frame Timing WCET).
;
; Everything that decides WHEN things happen is a hardware parameter (timer clock, fetch cycles, CSR access, I2C path, WCET ...): change them on the
; Simulator page and watch the two lanes move.
.include "defs.inc"

.csr MAIN_TPER   = 0x320                    ; the Main timer's period (2 bytes) and its start bit: example addresses, like the ones of defs.inc
.csr MAIN_TPER_H = 0x321
.csr MAIN_CTRL   = 0x322

.main
.const GYRO_BIAS dw 0

start:                                      ; wait for what Peri publishes
    BR.COND PERI_RESULT_VALID, dispatch
    JMP     start

dispatch:                                   ; which service?
    MOV PERI_RESULT_META[0..1] -> ALU_OPA
    MOV CONST(SID_IMU_GYRO_RAW) -> MAIN_SCRATCHPAD[20..21]
    MOV MAIN_SCRATCHPAD[20..21] -> ALU_OPB
    ALU CMP.EQ unsigned
    BR.COND ALU_CMP_TRUE, take
    JMP     drop

take:                                       ; consume: X_hi X_lo Y_hi Y_lo Z_hi Z_lo, the order on the bus
    MOV PERI_RESULT_SLOT[0..5]  -> MAIN_SCRATCHPAD[0..5]
    MOV MAIN_SCRATCHPAD[1..1]   -> MAIN_SCRATCHPAD[8..8]      ; swap the two bytes of raw_x (the sensor sends the high byte first)
    MOV MAIN_SCRATCHPAD[0..0]   -> MAIN_SCRATCHPAD[9..9]
    EXEC_CSR WRITE8 MAIN_TPER, 40                              ; Main's own timer: settle for 40 ticks before the commit
    EXEC_CSR WRITE8 MAIN_TPER_H, 0
    EXEC_CSR SET8   MAIN_CTRL, 1                               ; start it (bit 0)
settle:
    BR.COND MAIN_TIMER_DONE, window
    JMP     settle

window:                                     ; the Frame Timing: only in the blanking window, with at least the commit's WCET left
    BR.COND PGL_COMMIT_ELIGIBLE, commit
    JMP     window

commit:
    MOV MAIN_SCRATCHPAD[8..9]   -> ALU_OPA                     ; raw_x
    MOV MAIN_CONST_POOL[@GYRO_BIAS] -> MAIN_SCRATCHPAD[16..17]
    MOV MAIN_SCRATCHPAD[16..17] -> ALU_OPB                     ; bias_x
    ALU SUB signed
    MOV ALU_OUT[0..1]           -> MAIN_SCRATCHPAD[10..11]     ; corrected_x
    EXEC_CSR WRITE_SP GYRO_X_CORR_L, len=2, ptr=10             ; the result: CSR 0x120 / 0x121
    JMP     start

drop:                                       ; an id Main does not know: consume and drop
    MOV PERI_RESULT_SLOT[0..0] -> MAIN_SCRATCHPAD[12..12]
    JMP     start

.peri
cycle:
    EXEC_CSR SET8 PERI_CTRL, PERI_RESET                        ; clear the latches of the previous round
    EXEC_CSR WRITE8 PERI_TPER, 0x58                            ; the trigger: 600 ticks (0x0258) ...
    EXEC_CSR WRITE8 PERI_TPER_H, 0x02
    EXEC_CSR SET8 PERI_CTRL, PERI_TIMER_START                  ; ... started ONCE, outside the wait loop
wait_trigger:
    BR.IF_TIMER_DONE sample
    JMP wait_trigger
sample:
    EXEC_XFER READ dev=DEV_IMU, len=6, ptr=0, ext=[0x43]       ; gyro X Y Z, 6 bytes from register 0x43
    EXEC_PUB service=SID_IMU_GYRO_RAW, src=0, len=6
    EXEC_CSR SET8 PERI_CTRL, PERI_DONE
    JMP cycle
