; Default target profile (the baseline of ISA v1.6 r10: full arg-addressable table depths).
; Copy it, change what the RTL implements, and pass it with --profile.
; The [latency] values below are the PLANNING model of spec 14.3 / 14.4. The spec leaves the final latency table to the
; implementation (7.4.5), so replace them with the RTL's numbers; the assembler (WCET, listing) and the simulator share them.
; Print the effective values with: asmc --print-profile
main_move_depth = 64
main_branch_depth = 64
main_csr1_depth = 32
main_csrm_depth = 32
main_alu_depth = 64
peri_xfer1_depth = 32
peri_xferm_depth = 32
peri_pub_depth = 64
peri_branch_depth = 64
peri_csr1_depth = 32
device_map_depth = 8
max_code_bytes = 256
const_pool_max = 256
profile_cond0 = false
ret_slot_base = 24
ret_stage_base = 30
profile_id = planning-default
same_region_mov = true             ; v31 5.2: only OVERLAPPING same-region MOV is forbidden (confirm with RTL)
layout_offset_bits = 16            ; width of each ImageLayout sideband field (offsets are logical BYTES)

; shared program SRAM (v31 A1): one 32-bit SRAM, both images, windows must not overlap
sram_depth_words = 1024
main_image_base_word = 0
peri_image_base_word = 512
main_capacity_bytes = 2048
peri_capacity_bytes = 2048
main_pc_width = 8                  ; the 8-bit BR target field still limits ordinary branches to 0..255
peri_pc_width = 8
boot_pad_byte = 0

; DeviceMap entry packing
device_map_entry_bytes = 1
device_map_bus_shift = 7
device_map_addr_shift = 0
; per-section alignment: align_main_move, align_main_branch, align_main_csr1, align_main_csrm, align_main_alu, align_main_const,
; align_peri_xfer1, align_peri_xferm, align_peri_pub, align_peri_branch, align_peri_csr1, align_peri_devmap  (-1 = entry width)
; ALU sub-operation numbers: alu_arith_add ... alu_cmp_eq ... alu_logic_and ...  (see asmc --print-profile)
big_endian = false

; latency model (cycles)
br_cond = 1
cascade = 1
alu = 1
mul = 2
mov_setup = 1
mov_byte = 1
slot_consume_setup = 2
csr_access = 2
csr_byte = 2
csr_done = 1
pub_setup = 2
pub_byte = 1
xfer_issue = 2
cpu_hz = 27000000
i2c_hz = 400000
