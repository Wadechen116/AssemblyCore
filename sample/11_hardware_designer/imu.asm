; 11_hardware_designer - the IMU gyro program of 02_imu_gyro, but with NO definitions of its own.
; Every CSR, device and service name used below (AE_CTRL, DEV_IMU, SID_IMU_GYRO_RAW, PERI_CTRL ...) is defined by board.achw (the Hardware Designer);
; the generated board.gen.inc is the first source of the project and board.gen.env.cfg is its simulator environment.
; Change the board in the designer (Hardware tab) and watch this program's diagnostics react.

.main
.const GYRO_BIAS dw 0

start:
    BR.COND PERI_RESULT_VALID, dispatch
    JMP     start

dispatch:                                   ; which service? (BR does not compare ids: ALU does, spec 7.3.2)
    MOV PERI_RESULT_META[0..1] -> ALU_OPA
    MOV CONST(SID_IMU_GYRO_RAW) -> MAIN_SCRATCHPAD[20..21]    ; v31: constants go to the scratchpad first
    MOV MAIN_SCRATCHPAD[20..21] -> ALU_OPB
    ALU CMP.EQ unsigned
    BR.COND ALU_CMP_TRUE, handle_gyro
    JMP     unknown_service

handle_gyro:
    MOV PERI_RESULT_SLOT[0..5]  -> MAIN_SCRATCHPAD[0..5]      ; consume: X_hi X_lo Y_hi Y_lo Z_hi Z_lo (the order on the bus)
    MOV MAIN_SCRATCHPAD[1..1]   -> MAIN_SCRATCHPAD[8..8]      ; the sensor sends the HIGH byte first, ALU operands are little-endian
    MOV MAIN_SCRATCHPAD[0..0]   -> MAIN_SCRATCHPAD[9..9]      ; (byte 0 = LSB): swap the two bytes of raw_x into scratch[8..9]
    MOV MAIN_SCRATCHPAD[8..9]   -> ALU_OPA                    ; raw_x
    MOV MAIN_CONST_POOL[@GYRO_BIAS] -> MAIN_SCRATCHPAD[16..17]
    MOV MAIN_SCRATCHPAD[16..17] -> ALU_OPB                    ; bias_x
    ALU SUB signed
    MOV ALU_OUT[0..1]           -> MAIN_SCRATCHPAD[10..11]    ; corrected_x
    EXEC_CSR WRITE_SP GYRO_X_CORR_L, len=2, ptr=10            ; v31: the CSR write reads the scratchpad directly
    JMP     start

unknown_service:                            ; baseline: the unknown-id path must be explicit (spec 12.7)
    MOV PERI_RESULT_SLOT[0..0] -> MAIN_SCRATCHPAD[12..12]     ; consume and drop
    JMP     start

.peri
service_start:
    EXEC_CSR SET8 PERI_CTRL, PERI_RESET                ; clear the latches of the previous service
    EXEC_CSR WRITE8 POLL_MASK, 0x01                    ; ready bit is status bit 0
    EXEC_CSR WRITE8 PERI_TPER, 0xFF                    ; timeout timer: 0x7FFF cycles, started ONCE, outside the poll loop
    EXEC_CSR WRITE8 PERI_TPER_H, 0x7F
    EXEC_CSR SET8 PERI_CTRL, PERI_TIMER_START
poll_loop:
    EXEC_CSR SET8 POLL_CTRL, POLL_CLEAR_W1P            ; clear old poll latches
    EXEC_CSR SET8 POLL_CTRL, POLL_ARM_W1P              ; arm the next READ into PERI_RX_BUF[0]
    EXEC_XFER READ dev=DEV_IMU, addr=0x3A, addrlen=1, ptr=0
    BR.IF_READY       read_data
    BR.IF_TIMER_DONE  timeout
    JMP poll_loop
read_data:
    EXEC_XFER READ dev=DEV_IMU, len=6, ptr=0, ext=[0x43]   ; gyro XYZ, 6 bytes from register 0x43
    EXEC_PUB service=SID_IMU_GYRO_RAW, src=0, len=6
    EXEC_CSR SET8 PERI_CTRL, PERI_DONE
    JMP service_start
timeout:
    EXEC_CSR SET8 PERI_CTRL, PERI_ERROR_SET
    EXEC_CSR SET8 PERI_CTRL, PERI_DONE
    JMP service_start
