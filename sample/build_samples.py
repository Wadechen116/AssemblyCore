#!/usr/bin/env python3
"""Builds every sample project the way the IDE does (F7) into a folder of its own, out_sampleNN (out_sample07a ... for the four projects of 07),
and checks the EEPROM image it burns. Each folder gets a README.txt: where the files come from, the command, the EEPROM regions, the checks.

usage: build_samples.py [--bin-dir DIR] [--set-output-dir]
    --bin-dir DIR       holds asmc, asmdis, asmsim (default ../build; the .exe of a Windows build work from WSL too)
    --set-output-dir    also write output_dir = out_sampleNN into each .acproj, so the IDE builds into the same folder

A project that is meant to fail (sample_expect = fail, the 07 error samples) has no image: its folder holds only the README with the errors.
"""
import datetime, glob, json, os, re, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
bin_dir = os.path.join(here, "..", "build")
if "--bin-dir" in sys.argv:
    bin_dir = sys.argv[sys.argv.index("--bin-dir") + 1]
set_output_dir = "--set-output-dir" in sys.argv

def exe(n):
    p = os.path.join(bin_dir, n)
    return p + ".exe" if os.name == "nt" or (not os.path.exists(p) and os.path.exists(p + ".exe")) else p

winexe_from_wsl = os.name != "nt" and exe("asmc").endswith(".exe")
def arg(path, cwd):                                 # a Windows exe started from WSL: paths relative to its working directory, with backslashes
    return os.path.relpath(path, cwd).replace("/", "\\") if winexe_from_wsl else path
def shown(cmd):                                     # the command as it is written in the README (the tool by its name)
    return " ".join([os.path.basename(cmd[0]).replace(".exe", "")] + cmd[1:])

def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, timeout=300)
    dec = lambda b: b.decode("utf-8", errors="replace") if b else ""
    return r.returncode, dec(r.stdout) + dec(r.stderr)

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

def write_readme(path, text):
    open(path, "w", encoding="utf-8-sig", newline="\r\n").write(text)

now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
projects = sorted(glob.glob(os.path.join(here, "[0-9]*", "*.acproj")))
summary = []
for proj in projects:
    d = os.path.dirname(proj)
    p = parse(proj)
    name = p.get("name", os.path.splitext(os.path.basename(proj))[0])
    tag = re.match(r"\d+[a-z]?", name).group(0)
    out_name = "out_sample" + tag
    out_dir = os.path.join(d, out_name)
    os.makedirs(out_dir, exist_ok=True)
    if set_output_dir:
        text = open(proj, encoding="utf-8").read()
        text = re.sub(r"(?m)^output_dir\s*=.*$", "output_dir = " + out_name, text)
        open(proj, "w", encoding="utf-8").write(text)
    prefix = os.path.join(out_dir, name)
    model = p.get("call_model", "inline")
    common = []
    for inc in p["include_dir"]:
        common += ["-I", inc]
    if p.get("profile"):
        common += ["--profile", p["profile"]]
    build = [exe("asmc"), "--call-model", model, "--dataflow", p.get("dataflow", "warn"), "--list", "--map", "--dbg", "--hex", "--artifacts", "--eeprom"] + common
    for q in p["wcet"]:
        build += ["--wcet", q]
    build += p["source"] + ["-o", arg(prefix, d)]
    rc, out = run(build, d)
    rel = os.path.relpath(proj, here).replace("/", "\\")
    failing = p.get("sample_expect") == "fail"
    head = ("範例 %s（%s）%s\n" % (tag, name, "的建置結果" if failing else "建置產生的檔案")) + "=" * 40 + "\n\n" \
        + ("這個資料夾屬於範例 %s：它故意建置失敗，所以這裡只有這份說明。\n\n" % tag if failing else "這個資料夾裡的所有檔案，都是由範例 %s 建置產生的。\n\n" % tag) \
        + "來源\n----\n" \
        + "  專案      sample\\%s（output_dir = %s）\n" % (rel, out_name) \
        + "  原始碼    %s\n" % ", ".join(p["source"]) \
        + ("  include   %s\n" % ", ".join(p["include_dir"]) if p["include_dir"] else "") \
        + ("  profile   %s\n" % p["profile"] if p.get("profile") else "  profile   預設（planning-default；EEPROM 位址都是暫定值）\n") \
        + ("  產生自    %s（原始碼由它產生）\n" % (p.get("hardware") or p.get("blocks")) if p.get("hardware") or p.get("blocks") else "") \
        + "  呼叫模型  %s\n" % model \
        + "  建置時間  %s\n" % now \
        + "  工具      %s（與 AssemblyCoreIDE 同一版引擎）\n" % ("dist\\msvc2026\\asmc.exe" if winexe_from_wsl else "asmc") \
        + "  命令      %s\n\n" % shown(build).replace(arg(prefix, d), out_name + "\\" + name)
    if p.get("sample_expect") == "fail":
        codes = sorted(set(re.findall(r"error (E\d{3})\b", out)))
        want = p.get("sample_errors", "").split()
        ok = rc == 1 and set(want) <= set(codes)
        write_readme(os.path.join(out_dir, "README.txt"), head
                     + "這個範例「故意」建置失敗\n------------------------\n"
                     + "  範例 %s 用來示範組譯器的錯誤訊息，程式裡刻意放了錯誤，所以建置不會成功，\n" % tag
                     + "  也就沒有燒錄檔（.eeprom.bin）。這個資料夾只放這份說明。\n\n"
                     + "  預期的錯誤碼：%s\n" % " ".join(want)
                     + "  實際報出的錯誤碼：%s\n" % " ".join(codes)
                     + "  結果：%s\n\n" % ("符合預期（建置失敗，預期的錯誤碼都有報出）" if ok else "不符合預期！")
                     + "組譯器的完整訊息\n----------------\n" + "\n".join("  " + l for l in out.strip().splitlines()) + "\n")
        summary.append((name, out_name, "故意失敗" if ok else "不符合預期", ok))
        print("%-26s %-18s intended failure: %s" % (name, out_name, "as expected" if ok else "NOT as expected"))
        continue
    if rc != 0:
        write_readme(os.path.join(out_dir, "README.txt"), head + "建置失敗\n--------\n" + out)
        summary.append((name, out_name, "建置失敗", False))
        print("%-26s %-18s BUILD FAILED" % (name, out_name))
        continue
    eep = prefix + ".eeprom.bin"
    emap = json.load(open(prefix + ".eeprom.json", encoding="utf-8"))
    size = os.path.getsize(eep)
    # the checks: the image is a program of the specification, it is this program
    dis = [exe("asmdis"), "--eeprom", arg(eep, d), "--check"] + (["--profile", p["profile"]] if p.get("profile") else [])
    rc1, out1 = run(dis, d)
    ver = [exe("asmsim"), "--call-model", model] + common + p["source"] + ["--verify-eeprom", arg(eep, d)]
    rc2, out2 = run(ver, d)
    line1 = next((l for l in out1.splitlines() if l.startswith("check:")), out1.strip()[-200:])
    line2 = next((l for l in out2.splitlines() if l.startswith(("OK", "MISMATCH"))), out2.strip()[-200:])
    ok = rc1 == 0 and rc2 == 0
    regions = "".join("  %-12s 0x%04X        %d\n" % (r["name"], r["address"], r["bytes"]) for r in emap["regions"])
    files = sorted(os.listdir(out_dir))
    write_readme(os.path.join(out_dir, "README.txt"), head
                 + "EEPROM 燒錄檔（要燒進 EEPROM 的就是這組）\n----------------------------------------\n"
                 + "  %s.eeprom.bin    燒錄的 byte，%d byte\n" % (name, size)
                 + "  %s.eeprom.hex    同上，Intel HEX 格式（燒錄器用）\n" % name
                 + "  %s.eeprom.json   對照表：六個區域的位址與描述表位移（載入模擬器、反組譯時一定要有）\n" % name
                 + "  %s.eeprom.sym    名稱：標籤、CSR、服務、裝置（反組譯時用來還原名稱）\n\n" % name
                 + "  區域          EEPROM 位址   大小（byte）\n" + regions
                 + "  EEPROM：I2C 位址 0x%02X、記憶體位址 %d byte、容量 %d byte\n\n" % (emap["i2c_address"], emap["address_bytes"], emap["capacity"])
                 + "其他檔案：Main／Peri 的邏輯映像（.main.bin／.peri.bin 與 .layout.json、.hex）、列表 .lst、.map、除錯資訊 .dbg.json、\n"
                 + "v31 產物組（.sram.hex .boot.json .memory.json .sections.json .descriptors.json .services.json .events.json\n"
                 + ".validation.json .manifest.json）。共 %d 個檔案（含這份說明）。\n\n" % (len(files) + (0 if "README.txt" in files else 1))
                 + "已做過的驗證（%s）\n------------------------------\n" % now
                 + "  反組譯檢查   %s\n      %s\n" % (shown(dis).replace(arg(eep, d), out_name + "\\" + name + ".eeprom.bin"), line1)
                 + "  和原始碼比對 %s\n      %s\n" % (shown(ver).replace(arg(eep, d), out_name + "\\" + name + ".eeprom.bin"), line2)
                 + "  結果：%s\n\n" % ("通過" if ok else "有問題！")
                 + "怎麼用\n------\n"
                 + "  IDE：Debug →「從 EEPROM 映像除錯…」選 %s.eeprom.bin；\n" % name
                 + "       Build →「驗證 EEPROM 燒錄檔…」或「反組譯 EEPROM 燒錄檔…」。\n"
                 + "  在 IDE 開這個範例的專案再建置（F7），也會寫回這個資料夾（會覆蓋燒錄檔，這份說明不會被覆蓋）。\n"
                 + "  重新產生全部範例：python3 sample/build_samples.py --bin-dir <工具所在資料夾>\n")
    summary.append((name, out_name, "%d byte，檢查通過" % size if ok else "檢查有問題", ok))
    print("%-26s %-18s %4d bytes  disasm %s  verify %s" % (name, out_name, size, "OK" if rc1 == 0 else "FAILED", "OK" if rc2 == 0 else "FAILED"))

bad = [s for s in summary if not s[3]]
print("\n%d project(s), %d with a problem" % (len(summary), len(bad)))
sys.exit(1 if bad else 0)
