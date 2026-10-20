; Shared definitions for the examples. The CSR addresses are EXAMPLE values: the spec names the CSRs but does not number them.
.csr AE_CTRL         = 0x010
.csr GYRO_X_CORR_L   = 0x120
.csr POLL_MASK       = 0x300
.csr POLL_CTRL       = 0x301
.csr PERI_CTRL       = 0x302

.equ POLL_CLEAR_W1P  = 0x01
.equ POLL_ARM_W1P    = 0x02
.equ PERI_DONE       = 0x01
.equ PERI_ERROR_SET  = 0x02
.equ PERI_RESET      = 0x04      ; clear poll / error / done latches at the start of a service (spec 13.2)

; service result contract ids: one allocation plan shared by Main and Peri (spec 10.3)
.service SID_IMU_GYRO_RAW = 1
.service SID_TEMP_RAW     = 2

; DeviceMap: physical endpoints used by EXEC_XFER.dev_sel
.devmap 0, I2C, 0x68, DEV_IMU
