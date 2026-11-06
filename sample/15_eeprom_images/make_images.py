#!/usr/bin/env python3
"""Makes the EEPROM images of this sample: the burned image of example 14 (good) and copies of it with one fault each (bad_*).

usage: make_images.py [--bin-dir DIR]      (DIR holds asmc; default ../../build)

Every image comes with its map (.eeprom.json) and its names (.eeprom.sym); good_no_names has no symbol file.
images.txt says what each one must give: asmdis --eeprom --check (and --strict), and asmsim --verify-eeprom against example 14.
"""
import json, os, shutil, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
bin_dir = os.path.join(here, "..", "..", "build")
if "--bin-dir" in sys.argv:
    bin_dir = sys.argv[sys.argv.index("--bin-dir") + 1]
asmc = os.path.join(bin_dir, "asmc.exe" if os.path.exists(os.path.join(bin_dir, "asmc.exe")) else "asmc")
src_dir = os.path.join(here, "..", "14_main_peri_sim")

# 1. the good image: example 14, assembled with --eeprom
subprocess.run([asmc, "main_peri.asm", "-I", "../common", "--eeprom", "-o", os.path.join(here, "good")], cwd=src_dir, check=True, stdout=subprocess.DEVNULL)
for ext in (".main.bin", ".peri.bin", ".main.layout.json", ".peri.layout.json"):
    p = os.path.join(here, "good" + ext)
    if os.path.exists(p):
        os.remove(p)
good = bytearray(open(os.path.join(here, "good.eeprom.bin"), "rb").read())
gmap = json.load(open(os.path.join(here, "good.eeprom.json")))
region = {r["name"]: r for r in gmap["regions"]}
layout = {d["domain"]: d["image_layout"] for d in gmap["domains"]}

def eeprom_of(domain, logical):                     # EEPROM address of a logical offset of a domain
    for name in (domain + ".devmap", domain + ".const", domain + ".desc", domain + ".code"):
        r = region.get(name)
        if r and r["bytes"] and logical >= r["logical_start"]:
            return r["address"] + logical - r["logical_start"]
    raise SystemExit("no region for %s %d" % (domain, logical))

main_br = eeprom_of("main", layout["main"][1])      # MainBranchDesc_16b: entry 0 is BR.COND PERI_RESULT_VALID, entry 1 is JMP start
main_code = region["main.code"]["address"]
main_const = region["main.const"]["address"]

def write(name, image, m=None, sym=True):
    open(os.path.join(here, name + ".eeprom.bin"), "wb").write(bytes(image))
    json.dump(m or gmap, open(os.path.join(here, name + ".eeprom.json"), "w"), indent=1)
    if sym:
        shutil.copyfile(os.path.join(here, "good.eeprom.sym"), os.path.join(here, name + ".eeprom.sym"))
    elif os.path.exists(os.path.join(here, name + ".eeprom.sym")):
        os.remove(os.path.join(here, name + ".eeprom.sym"))

def changed(at, value=None, xor=None):
    b = bytearray(good)
    b[at] = value if value is not None else b[at] ^ xor
    return b

write("good_no_names", good, sym=False)
write("bad_reserved_bits", changed(main_br + 1, xor=0x01))           # BR descriptor 0: the reserved bit [8] set
write("bad_table_index", changed(main_code + 0x03, value=0xBF))      # MOV at pc 3 names MOV descriptor 63 (the table has 11)
write("bad_branch_target", changed(main_br + 2, value=0x11))         # JMP start -> pc 0x11, the inline byte of an EXEC_CSR
write("bad_runs_off_end", changed(main_code + 0x1E, value=0xC0))     # the last JMP of Main becomes an ALU: the code runs off its end
write("bad_unreachable", changed(main_code + 0x07, value=0x41))      # JMP drop -> JMP start: 'drop' cannot be reached
write("bad_constant", changed(main_const + 0, value=0x05))           # GYRO_BIAS 0 -> 5: still a legal program, only not this one
m = json.loads(json.dumps(gmap))
for r in m["regions"]:
    if r["name"] == "peri.code":
        r["address"] += 2                                           # the map puts Peri's code 2 bytes off
write("bad_map", good, m)
write("bad_truncated", good[:-20])                                   # the last 20 bytes were not burned
print("images written to", here)
