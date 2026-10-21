; 10_multi_file / peri_part.asm - the Peri side.
; (shared.inc is listed first in the project, so its definitions are visible here without an .include)

.peri
read_temp:
    EXEC_XFER READ dev=TEMP_SENSOR, len=2, ptr=0, ext=[TEMP_REG]    ; the DeviceMap entry is named in shared.inc
    EXEC_PUB service=SVC_TEMP, src=0, len=2
halt_peri:
    JMP halt_peri
