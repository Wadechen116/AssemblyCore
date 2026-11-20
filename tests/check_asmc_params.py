#!/usr/bin/env python3
"""asmc --params / --param / --fetch-time: the command line counts cycles and the WCET the way AssemblyCoreIDE and the simulator do.

usage: check_asmc_params.py --bin-dir DIR      (DIR holds asmc; the .exe of a Windows build works from WSL too)
The expected numbers are the ones AssemblyCoreIDE's build gives for the same program (AssemblyCoreIDE/tests/services_tests.cpp,
t_build_follows_params): one program, one model, two front ends.
"""
import os, subprocess, sys, tempfile

bin_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")
if "--bin-dir" in sys.argv: bin_dir = sys.argv[sys.argv.index("--bin-dir") + 1]
asmc = os.path.join(bin_dir, "asmc")
if not os.path.exists(asmc) and os.path.exists(asmc + ".exe"): asmc += ".exe"
win_from_wsl = os.name != "nt" and asmc.endswith(".exe")

def arg(path):                       # a path the tool understands
    return subprocess.run(["wslpath", "-w", path], capture_output=True, text=True).stdout.strip() if win_from_wsl else path

SRC = (".main\n.wcet T, a, b\na: MOV CONST(1, 2) -> MAIN_SCRATCHPAD[0..1]\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n"
       " MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPB\n ALU ADD unsigned\nb: JMP b\n")
failures = 0
def check(ok, name, detail=""):
    global failures
    print(("  PASS  " if ok else "  FAIL  ") + name + ("" if ok else "\n        " + detail))
    if not ok: failures += 1

base = os.environ.get("ACIDE_TEST_OUT") if win_from_wsl else None      # a Windows tool needs a folder on a Windows drive
with tempfile.TemporaryDirectory(dir=base) as d:
    src = os.path.join(d, "w.asm")
    open(src, "w", newline="\n").write(SRC)
    par = os.path.join(d, "w.simparams")
    open(par, "w", newline="\n").write("[params]\nlatency.alu = 7\n")
    def run(tag, *opts):
        r = subprocess.run([asmc, arg(src), "-o", arg(os.path.join(d, tag)), "--map", "--dbg"] + [arg(o) if o == par else o for o in opts], capture_output=True, text=True)
        img = open(os.path.join(d, tag + ".main.bin"), "rb").read() if r.returncode == 0 else b""
        mp = open(os.path.join(d, tag + ".map")).read() if r.returncode == 0 else ""
        dbg = open(os.path.join(d, tag + ".dbg.json")).read() if r.returncode == 0 else ""
        return r, img, mp, dbg
    plain, img0, map0, dbg0 = run("p0")
    check(plain.returncode == 0 and "WCET T: 10 cycles\n" in plain.stdout and "fetch" not in plain.stdout and "fetching" not in map0 and "fetch" not in dbg0,
          "without the options nothing changes: WCET 10, no fetch numbers anywhere", plain.stdout + plain.stderr)
    ft, img1, map1, dbg1 = run("p1", "--fetch-time")
    check("WCET T: 33443 cycles (10 executing + 33433 fetching the instructions)" in ft.stdout and "I2C EEPROM, one read per region, 400000 Hz" in ft.stdout,
          "--fetch-time: the default fetch (I2C EEPROM) is counted, 33433 cycles, as the IDE's build gives", ft.stdout + ft.stderr)
    check("33443 cycles (10 executing + 33433 fetching)" in map1 and '"fetchCycles":33433' in dbg1 and '"fetch":6080' in dbg1, "the map and the debug JSON carry the two parts")
    pf, img2, _, _ = run("p2", "--params", par)
    check("WCET T: 33449 cycles (16 executing + 33433 fetching the instructions)" in pf.stdout, "--params FILE: latency.alu = 7 from the file, and the fetching", pf.stdout + pf.stderr)
    both, _, _, _ = run("p3", "--params", par, "--param", "latency.alu=2", "--param", "fetch.read_mode=instant")
    check("WCET T: 11 cycles\n" in both.stdout and "fetch" not in both.stdout, "--param wins over the file; fetch.read_mode = instant adds nothing", both.stdout + both.stderr)
    fe, img4, _, _ = run("p4", "--param", "fetch.source=frontend")
    check("WCET T: 27 cycles (10 executing + 17 fetching the instructions)" in fe.stdout and "program SRAM through the Frontend" in fe.stdout,
          "fetch.source = frontend: a few cycles per instruction", fe.stdout + fe.stderr)
    check(img0 and img0 == img1 == img2 == img4, "the image is the same in every case")
    bad, _, _, _ = run("p5", "--param", "nokey=1")
    check(bad.returncode == 2 and "unknown parameter" in bad.stderr, "an unknown parameter is refused (exit 2)", bad.stderr)
    bad2, _, _, _ = run("p6", "--params", os.path.join(d, "none.simparams"))
    check(bad2.returncode == 2 and "cannot read" in bad2.stderr, "a parameter file that cannot be read is refused (exit 2)", bad2.stderr)

print(("OK" if not failures else "FAILED") + "      %d failure(s)" % failures)
sys.exit(1 if failures else 0)
