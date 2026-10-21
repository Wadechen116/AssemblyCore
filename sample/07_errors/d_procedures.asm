; 07d - a procedure error. Recursion is rejected because the call depth and the WCET must be bounded. Nothing is built.
.include "defs.inc"

.main
.proc a
    CALL b
    RET
.endp
.proc b
    CALL a                                        ; E305  recursion: a -> b -> a
    RET
.endp
start:
    CALL a
done:
    JMP done
