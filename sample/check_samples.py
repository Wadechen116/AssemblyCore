#!/usr/bin/env python3
"""Builds and runs every sample project the way the IDE would, with the command-line tools of the same engine.

For each */*.acproj:  assemble (every call model in `sample_models`, default inline), check the expected outcome (`sample_expect = ok|fail`,
`sample_errors = E101 ...` must all be reported), run every `scenario`, and for good projects disassemble the images again
(asmdis --check: the text must assemble to the identical image).

usage: check_samples.py [--bin-dir DIR]      (DIR holds asmc, asmsim, asmdis; default ../build)
"""
import glob, os, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
bin_dir = os.path.join(here, "..", "build")
if "--bin-dir" in sys.argv:
    bin_dir = sys.argv[sys.argv.index("--bin-dir") + 1]
def exe(n):
    p = os.path.join(bin_dir, n)
    return p + ".exe" if os.name == "nt" or (not os.path.exists(p) and os.path.exists(p + ".exe")) else p

fails = 0
def report(ok, what, detail=""):
    global fails
    print(("  PASS  " if ok else "  FAIL  ") + what)
    if not ok:
        fails += 1
        if detail:
            print("        " + detail.strip().replace("\n", "\n        ")[:1500])

def parse(path):
    p = {"source": [], "include_dir": [], "scenario": [], "wcet": []}
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if "=" not in line:
            continue
        k, v = [x.strip() for x in line.split("=", 1)]
        if k in p:
            p[k].append(v)
        else:
            p[k] = v
    return p

# A Windows exe started from WSL understands the working directory but not /mnt/... arguments: give it paths relative to that directory.
winexe_from_wsl = os.name != "nt" and exe("asmc").endswith(".exe")
def arg(path, cwd):
    return os.path.relpath(path, cwd) if winexe_from_wsl else path

def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=120)
    return r.returncode, r.stdout + r.stderr

projects = sorted(glob.glob(os.path.join(here, "[0-9]*", "*.acproj")))
print("%d sample projects" % len(projects))
tmp = tempfile.mkdtemp(prefix="acsample_", dir=os.environ.get("ACSAMPLE_TMP"))
for proj in projects:
    d = os.path.dirname(proj)
    p = parse(proj)
    name = p.get("name", os.path.basename(proj))
    print("\n" + name)
    models = p.get("sample_models", "inline").split()
    common = []
    for i in p["include_dir"]:
        common += ["-I", i]
    if p.get("profile"):
        common += ["--profile", p["profile"]]
    for w in p["wcet"]:
        common += ["--wcet", w]
    if p.get("hardware"):
        # the description must be valid, and the generated files in the project must be exactly what it generates now
        rc0, out0 = run([exe("asmhw"), "check", p["hardware"]], d)
        report(rc0 == 0, "hardware description %s is valid" % p["hardware"], out0)
        gen_prefix = os.path.join(tmp, name + "_hw")
        rc1, out1 = run([exe("asmhw"), "gen", p["hardware"], "-o", arg(gen_prefix, d)], d)
        if rc1 == 0:
            for have, made in ((p["source"][0], gen_prefix + ".inc"), (p.get("environment", ""), gen_prefix + ".env.cfg")):
                same = os.path.exists(os.path.join(d, have)) and open(os.path.join(d, have), encoding="utf-8").read() == open(made, encoding="utf-8").read()
                report(same, "%s is what %s generates (run asmhw gen board.achw)" % (have, p["hardware"]))
        else:
            report(False, "asmhw gen", out1)
    expect = p.get("sample_expect", "ok")
    for model in models:
        prefix = os.path.join(tmp, name + "_" + model)
        cmd = [exe("asmc"), "--call-model", model, "--artifacts"] + common + p["source"] + ["-o", arg(prefix, d)]
        rc, out = run(cmd, d)
        if expect == "fail":
            codes = {w.rstrip(":") for w in out.replace("error ", " error ").split() if len(w) >= 4 and w[0] == "E" and w[1:4].isdigit()}
            want = set(p.get("sample_errors", "").split())
            report(rc == 1 and want <= codes, "%s: build fails with %s" % (model, " ".join(sorted(want))), "exit %d, got %s\n%s" % (rc, sorted(codes), out))
            continue
        report(rc == 0, "%s: builds without errors" % model, out)
        if rc != 0:
            continue
        # the artifacts exist and the logical images disassemble back to identical bytes
        for dom in ("main", "peri"):
            bin_ = prefix + "." + dom + ".bin"
            if not os.path.exists(bin_):
                continue
            cmd = [exe("asmdis"), "--" + dom, "--check", arg(bin_, d), "--events", arg(prefix + ".events.json", d)]
            if p.get("profile"):
                cmd += ["--profile", p["profile"]]
            rc2, out2 = run(cmd, d)
            report(rc2 == 0 and "check: OK" in out2, "%s: %s image -> asmdis -> same image" % (model, dom), out2)
        for scn in p["scenario"]:
            cmd = [exe("asmsim"), "--call-model", model] + common + p["source"]
            if p.get("environment"):
                cmd += ["--env", p["environment"]]
            cmd += ["--scenario", scn]
            rc3, out3 = run([c for c in cmd if c != "--wcet" and not (":" in c and c in p["wcet"])], d)
            report(rc3 == 0 and "scenario: PASS" in out3, "%s: scenario %s" % (model, scn), out3)
print("\n%s  %d failure(s)" % ("FAILED" if fails else "OK    ", fails))
sys.exit(1 if fails else 0)
