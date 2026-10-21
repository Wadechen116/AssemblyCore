; A made-up target profile, to test everything the profile decides. NONE of these numbers is an approved hardware value.
profile_id = toy-demo

; --- shared program SRAM (spec v31 A1): one 32-bit SRAM, two non-overlapping windows
sram_depth_words = 512
main_image_base_word = 256         ; Main window: words 256..287
main_capacity_bytes = 128
peri_image_base_word = 288         ; Peri window: words 288..319
peri_capacity_bytes = 128
boot_pad_byte = 255                ; padding between sections and in the last word of an image

; --- ImageLayout (sideband latches) and section alignment
layout_offset_bits = 8             ; every offset must fit 8 bits
align_main_move = 8
align_main_const = 4
align_peri_devmap = 2

; --- DeviceMap entry: 2 bytes, bus_type at bit 15, I2C address at bits 10:4
device_map_entry_bytes = 2
device_map_bus_shift = 15
device_map_addr_shift = 4

; --- ALU sub-operation numbers (the spec names them but does not number them)
alu_arith_add = 5
alu_arith_sub = 6
alu_arith_mul = 7
alu_arith_inc = 8
alu_arith_dec = 9
alu_cmp_eq = 3
alu_cmp_ne = 2
alu_cmp_lt = 1
alu_cmp_ge = 0

; --- events and latency
event_count = 4
event_dispatch = 3
event_eret = 3
csr_access = 3
