; shared.inc - definitions for both domains. It is the FIRST source of the project, so main_part.asm and peri_part.asm see it. Rename SVC_TEMP here (F2) and see every use change.
.service SVC_TEMP = 3, len=2, little          ; a 16-bit temperature, low byte first
.devmap 0, I2C, 0x48, TEMP_SENSOR
.csr TEMP_OUT = 0x150
.equ TEMP_REG = 0x00
