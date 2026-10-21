; 07c - errors in the control flow, the DeviceMap and the events (found when the program is laid out). Nothing is built.
.include "defs.inc"

.main
.vector 0, handler_a                              ; E315  this handler never reaches BR.ERET
.vector 1, nowhere                                ; E315  undefined vector label
start:
    BR.COND ALWAYS, missing                       ; E301  undefined label
    BR.ERET                                       ; E315  BR.ERET reachable from the start of the program
top:
    CASCADE_BEGIN 1, c0
c0:
    EXEC_CSR WRITE8 AE_CTRL, 1
    CASCADE_END 0, out
c1:
    EXEC_CSR WRITE8 AE_CTRL, 2
    CASCADE_END 5, out                            ; E304  the block index must be 1 here
out:
    JMP top
handler_a:
    JMP handler_a

.peri
p:
    EXEC_XFER READ dev=3, addr=1, addrlen=1, ptr=0   ; E309  DeviceMap[3] is not defined (defs.inc defines entry 0 only)
    JMP p
