; Shared definitions for the examples. The CSR addresses are EXAMPLE values: the spec names the CSRs but does not number them.
.csr AE_CTRL         = 0x010
.csr GYRO_X_CORR_L   = 0x120
.csr POLL_MASK       = 0x300
.csr POLL_CTRL       = 0x301
.csr PERI_CTRL       = 0x302
.csr PERI_TPER       = 0x310      ; timer period, 2 bytes (low, high)
.csr PERI_TPER_H     = 0x311

.equ POLL_CLEAR_W1P  = 0x01
.equ POLL_ARM_W1P    = 0x02
.equ PERI_DONE       = 0x01
.equ PERI_ERROR_SET  = 0x02
.equ PERI_TIMER_START = 0x08
.equ PERI_RESET      = 0x04      ; clear poll / error / done latches at the start of a service (spec 13.2)

; which CSR bits mean what (lets the assembler check timer-start and poll-arm flows, W515 / W516)
.csrbit POLL_CTRL[0] = poll_clear
.csrbit POLL_CTRL[1] = poll_arm
.csrbit PERI_CTRL[3] = timer_start

; service result contract ids: one allocation plan shared by Main and Peri (spec 10.3)
.service SID_IMU_GYRO_RAW = 1, len=6, big    ; X_hi X_lo Y_hi Y_lo Z_hi Z_lo: the sensor sends the high byte first
.service SID_TEMP_RAW     = 2

; DeviceMap: physical endpoints used by EXEC_XFER.dev_sel
.devmap 0, I2C, 0x68, DEV_IMU
