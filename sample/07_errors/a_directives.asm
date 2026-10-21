; 07a - errors in the source structure (found first, while reading the file). Nothing is built.
; Open the Problems panel, press F8 for the next problem, hover a squiggle, try `asmc --explain E105`.
.include "defs.inc"
.include "no_such_file.inc"                       ; E106  the include cannot be read
.org 0x10                                         ; E105  the PC always starts at 0

.main
start:
    EXEC_CSR WRITE8 AE_CTRL, 1
start:                                            ; E104  label defined twice
    JMP start
