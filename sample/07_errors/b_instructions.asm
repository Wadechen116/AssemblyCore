; 07b - errors in single instructions (found once the file reads cleanly). Nothing is built.
; Fix them one at a time and watch the list shrink. The expected code is in each comment.
.include "defs.inc"

.main
start:
    FROBNICATE 1                                  ; E101  unknown instruction
    EXEC_CSR WRITE8 AE_CTRL                       ; E102  the immediate is missing
    MOV CONST(NOT_DEFINED, 2) -> MAIN_SCRATCHPAD[0..1]    ; E103  undefined symbol
    EXEC_CSR WRITE8 AE_CTRL, 300                  ; E201  300 does not fit a byte
    MOV CONST(1, 2) -> ALU_OPA                    ; E202  v31: a constant goes to the scratchpad first
    MOV MAIN_SCRATCHPAD[0..1] -> MAIN_CSR_DATA_BUF[0..1]    ; E202  MAIN_CSR_DATA_BUF is read-only for MOV
    MOV MAIN_SCRATCHPAD[0..3] -> MAIN_SCRATCHPAD[2..5]      ; E202  overlapping copy inside one region
    ALU EQ signed                                 ; E203  EQ / NE have no signed mode
    ALU SHL 9                                     ; E201  the shift amount is 1..8
    EXEC_CSR WRITE_BUF GYRO_X_CORR_L, len=2, ptr=0          ; E204  renamed WRITE_SP in v31 (the source is the scratchpad)
    BR.COND NO_SUCH_CONDITION, start              ; E205  not a ConditionMap name
    EXEC_PUB service=1, src=0, len=1              ; E307  EXEC_PUB belongs to Peri
    CASCADE_BEGIN 0, blk, safe                    ; E208  event_safe is not allowed on cascade instructions
blk:
    CASCADE_END 0, start
    JMP start

.peri
p:
    EXEC_XFER WRITE dev=0, len=2, ptr=0           ; E206  a multi-byte WRITE is illegal in the baseline
    EXEC_PUB service=0, src=0, len=1              ; E201  service_id 0 is reserved (1..15)
    JMP p
