; 17_alu_busy - a loop that never ends and keeps the ALU busy: nine ALU operations a round, and nearly every other instruction is a MOV
; that carries an operand into the ALU or its result out. Made to WATCH: open the Simulator page and let it run.
;
;   every round:  n = n + 1          INC
;                 sum = sum + n      ADD
;                 diff = sum - n     SUB
;                 sq = n * n         MUL
;                 mix = sum ^ sq     XOR
;                 low = mix & 0xFF   AND
;                 half = sum >> 1    SHR
;                 quad = n << 2      SHL
;                 n >= 8 ?           CMP.GE  -> a new lap: n and sum start again, the lap counter goes to the CSR LAPS
;
; MAIN_SCRATCHPAD:  [0..1] n   [2..3] sum   [4..5] diff   [6..7] sq   [8..9] mix   [10..11] low   [12..13] half   [14..15] quad
;                   [16..17] laps   [18..19] a constant on its way to ALU_OPB (a constant cannot go to the ALU directly)

.csr LAPS = 0x150                         ; the lap counter (2 bytes), written once a lap

.main
start:
    MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]                 ; n = 0
    MOV CONST(0, 2) -> MAIN_SCRATCHPAD[2..3]                 ; sum = 0
    MOV CONST(0, 2) -> MAIN_SCRATCHPAD[16..17]               ; laps = 0
round:
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA
    ALU INC unsigned                                         ; n + 1
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[0..1]

    MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPA
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPB
    ALU ADD unsigned                                         ; sum + n
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[2..3]

    MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPA
    ALU SUB unsigned                                         ; sum - n  (ALU_OPB is still n)
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[4..5]

    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA
    ALU MUL unsigned                                         ; n * n
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[6..7]

    MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPA
    MOV MAIN_SCRATCHPAD[6..7] -> ALU_OPB
    ALU XOR                                                  ; sum ^ sq
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[8..9]

    MOV MAIN_SCRATCHPAD[8..9] -> ALU_OPA
    MOV CONST(0x00FF, 2) -> MAIN_SCRATCHPAD[18..19]
    MOV MAIN_SCRATCHPAD[18..19] -> ALU_OPB
    ALU AND                                                  ; mix & 0x00FF
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[10..11]

    MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPA
    ALU SHR 1 unsigned                                       ; sum >> 1
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[12..13]

    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA
    ALU SHL 2 unsigned                                       ; n << 2
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[14..15]

    MOV CONST(8, 2) -> MAIN_SCRATCHPAD[18..19]
    MOV MAIN_SCRATCHPAD[18..19] -> ALU_OPB
    ALU CMP.GE unsigned                                      ; n >= 8 ?  (ALU_OPA is still n; a compare leaves ALU_OUT alone)
    BR.COND ALU_CMP_TRUE, lap
    JMP round
lap:
    MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]                 ; n and sum start again
    MOV CONST(0, 2) -> MAIN_SCRATCHPAD[2..3]
    MOV MAIN_SCRATCHPAD[16..17] -> ALU_OPA
    ALU INC unsigned                                         ; laps + 1
    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[16..17]
    EXEC_CSR WRITE_SP LAPS, len=2, ptr=16                    ; show it: the arbiter and the CSR box flash once a lap
    JMP round
