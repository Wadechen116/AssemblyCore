; The result CSRs of the conformance tests: 128 addresses the tests write what they computed to. They must not be real registers of the
; board that do something when written; tests that read back (c01 SET8 / CLR8, c09) need them to return what was written.
; Change the base with:  make_conformance.py --res-base 0x...   (do not edit: generated)
.csr RES      = 0x600
.csr RES_DONE = 0x67F          ; the last write of every test: 0xA5
