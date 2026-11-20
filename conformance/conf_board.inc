; The board's own CSRs used by the layer-2 tests (b01, b02, b05): the timers, the Poll Unit, done / error.
; EXAMPLE addresses and bits: the spec names these registers but does not number them. PUT YOUR BOARD'S VALUES HERE before running on the
; hardware, then run make_conformance.py again (this file is kept when it exists; delete it to get the examples back).
.csr POLL_MASK   = 0x300        ; which bits of the status byte mean "ready"
.csr POLL_CTRL   = 0x301        ; write 1 to: bit 0 clear the poll latches, bit 1 arm the Poll Unit
.csr PERI_CTRL   = 0x302        ; write 1 to: bit 0 set done, bit 1 set error, bit 2 clear the poll / error / done latches, bit 3 start the Peri timer
.csr PERI_TPER   = 0x310        ; Peri timer period in ticks, low byte
.csr PERI_TPER_H = 0x311        ; ... high byte
.csr MAIN_TPER   = 0x320        ; Main timer period in ticks, low byte
.csr MAIN_TPER_H = 0x321        ; ... high byte
.csr MAIN_CTRL   = 0x322        ; write 1 to: bit 0 start the Main timer

.equ POLL_CLEAR_W1P   = 0x01
.equ POLL_ARM_W1P     = 0x02
.equ PERI_DONE        = 0x01
.equ PERI_ERROR_SET   = 0x02
.equ PERI_RESET       = 0x04
.equ PERI_TIMER_START = 0x08
.equ MAIN_TIMER_START = 0x01

; what the bits do, for the assembler's flow checks (timer started before it is tested, poll armed before it is read)
.csrbit POLL_CTRL[0] = poll_clear
.csrbit POLL_CTRL[1] = poll_arm
.csrbit PERI_CTRL[3] = timer_start
