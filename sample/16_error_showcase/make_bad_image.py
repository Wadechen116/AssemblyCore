#!/usr/bin/env python3
"""Makes burned/showcase_bad.eeprom.{bin,json,sym}: the EEPROM image of showcase.asm with three fields damaged on purpose, and checks that
the decoder and the simulator see exactly the problems the example is about.

    Main pc 2  ALU ADD                       the ALU descriptor -> a sub-operation code the profile does not assign   A (D201)
    Main pc 4  MOV SCRATCHPAD[4..5] -> OPB   its source offset -> 31 (31..32 leaves the 32-byte scratchpad)         B (D205)
    Peri pc 0  EXEC_XFER READ len=2          its ptr -> 31 (31 + 2 leaves the 32-byte PERI_RX_BUF)                  B (D205), and Peri faults B there

usage: make_bad_image.py [--bin-dir DIR]     (asmc / asmdis / asmsim; default ../../build; the .exe of a Windows build work from WSL too)
"""
import json, os, re, shutil, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
bin_dir = os.path.join(here, "..", "..", "build")
if "--bin-dir" in sys.argv:
    bin_dir = sys.argv[sys.argv.index("--bin-dir") + 1]

def exe(n):
    p = os.path.join(bin_dir, n)
    return p + ".exe" if os.name == "nt" or (not os.path.exists(p) and os.path.exists(p + ".exe")) else p
winexe = os.name != "nt" and exe("asmc").endswith(".exe")
def arg(path):                                      # a Windows exe started from WSL: paths relative to this folder, with backslashes
    return os.path.relpath(path, here).replace("/", "\\") if winexe else path
def run(cmd):
    r = subprocess.run(cmd, cwd=here, capture_output=True, timeout=120)
    return r.returncode, (r.stdout + r.stderr).decode("utf-8", errors="replace")

out_dir = os.path.join(here, "burned")
os.makedirs(out_dir, exist_ok=True)
tmp = tempfile.mkdtemp(prefix="ac16_", dir=here)
try:
    rc, out = run([exe("asmc"), "--eeprom", "showcase.asm", "-o", arg(os.path.join(tmp, "showcase_bad"))])
    if rc != 0:
        sys.exit("build failed:\n" + out)
    for ext in (".eeprom.bin", ".eeprom.json", ".eeprom.sym"):
        shutil.copy(os.path.join(tmp, "showcase_bad" + ext), os.path.join(out_dir, "showcase_bad" + ext))
finally:
    shutil.rmtree(tmp, ignore_errors=True)

base = os.path.join(out_dir, "showcase_bad")
img = bytearray(open(base + ".eeprom.bin", "rb").read())
emap = json.load(open(base + ".eeprom.json", encoding="utf-8"))
region = {r["name"]: r for r in emap["regions"]}
layout = {d["domain"]: d["image_layout"] for d in emap["domains"]}

def table(dom, idx, width):
    """EEPROM addresses of the entries of one descriptor table (ImageLayout order of spec 3.1; little-endian, the default profile)."""
    r = region[dom + ".desc"]
    lay = layout[dom]
    start, end = lay[idx], lay[idx + 1]
    return [r["address"] + (p - r["logical_start"]) for p in range(start, end - width + 1, width)]

def value(a, width):
    return sum(img[a + k] << (8 * k) for k in range(width))

def put(a, width, v):
    for k in range(width):
        img[a + k] = (v >> (8 * k)) & 0xFF

def find(dom, idx, width, want):
    for a in table(dom, idx, width):
        if value(a, width) == want:
            return a
    sys.exit("no %s table %d entry 0x%X" % (dom, idx, want))

# Main ALU table (4): the one entry of "ALU ADD unsigned" -> class arith, code 31 (no sub-operation has it)
alu = table("main", 4, 1)[0]
put(alu, 1, 0x1F)
# Main MOV table (0): MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB = src 0, dst 4, src_off 4, dst_off 0, len-1 1 -> src_off 31
mov = find("main", 0, 4, (0 << 28) | (4 << 24) | (4 << 16) | (0 << 8) | 1)
put(mov, 4, (value(mov, 4) & ~(0xFF << 16)) | (31 << 16))
# Peri EXEC_XFER MultiByte table (1): READ dev 0, addr_len 1, len 2, ptr 0 -> ptr 31
xfer = find("peri", 1, 2, (1 << 15) | (0 << 12) | (1 << 10) | ((2 - 1) << 5) | 0)
put(xfer, 2, (value(xfer, 2) & ~31) | 31)
open(base + ".eeprom.bin", "wb").write(img)

# the checks: each damaged instruction is seen in its class, nothing else is
rc, out = run([exe("asmsim"), "showcase.asm", "--eeprom", arg(base + ".eeprom.bin"), "--decoded"])
want = [("Main", 2, "A"), ("Main", 4, "B"), ("Peri", 0, "B")]
seen, dom, pc = [], None, None
for line in out.splitlines():
    m = re.match(r"^(Main|Peri) \(decoded", line)
    if m:
        dom = m.group(1)
    m = re.match(r"^\s+(?:!!|  ) ([0-9A-F]{3}) ", line)
    if m:
        pc = int(m.group(1), 16)
    m = re.match(r"^\s+([AB]) \(", line)
    if m:
        seen.append((dom, pc, m.group(1)))
ok = sorted(seen) == sorted(want)
print("decoder sees: %s -> %s" % (", ".join("%s pc %d %s" % s for s in seen), "as intended" if ok else "NOT as intended (want %s)" % want))
rc2, out2 = run([exe("asmsim"), "showcase.asm", "--eeprom", arg(base + ".eeprom.bin"), "--scenario", arg(base + ".scn")])
peri_b = rc2 == 0 and "scenario: PASS" in out2
print("running (burned/showcase_bad.scn): %s" % ("Main faults C, Peri faults B at EXEC_XFER, as intended" if peri_b else "NOT as intended:\n" + out2[-800:]))
sys.exit(0 if ok and peri_b else 1)
