#!/usr/bin/env python3
"""Layer-1 conformance kit: does the hardware decode the bytes the way the tools encode them?

Each test is a short program that does one kind of thing once per variant and writes what came out to "result CSRs" (RES + n). Run the same
image in the RTL simulation (or on the FPGA), look at the CSR writes, and compare with the table: a wrong value says WHICH encoding assumption is
wrong (docs/SIM_VS_SPEC.md section 4, layer 1; docs/PROJECT_STATUS.md 7.4, D1-D6).

The expected values are computed HERE, from the text of spec v31 (exact arithmetic, the byte order of the regions), not taken from the simulator.
The simulator is then run on the same programs and must agree -- so this is also a regression test of the tools (ctest conformance_kit).

usage: make_conformance.py [--bin-dir DIR] [--out DIR] [--res-base 0x600] [--check]
    --bin-dir DIR    holds asmc and asmsim (default ../build; the .exe of a Windows build work from WSL too)
    --out DIR        where the images and tables go (default ./out); with a Windows .exe from WSL it must lie on a Windows drive
    --res-base ADDR  first of the 128 result CSR addresses (default 0x600): they must not collide with real registers of the board
    --check          build, simulate and compare only; exit 1 on a difference (the sources and tables in this folder are not rewritten)
"""
import os, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
argv = sys.argv[1:]
def opt(name, default):
    return argv[argv.index(name) + 1] if name in argv else default
bin_dir = opt("--bin-dir", os.path.join(here, "..", "build"))
out_dir = os.path.abspath(opt("--out", os.path.join(here, "out")))
RES = int(opt("--res-base", "0x600"), 0)
check_only = "--check" in argv
DONE = RES + 0x7F
DONE_VALUE = 0xA5

def exe(n):
    p = os.path.join(bin_dir, n)
    return p + ".exe" if os.name == "nt" or (not os.path.exists(p) and os.path.exists(p + ".exe")) else p
win_from_wsl = os.name != "nt" and exe("asmc").endswith(".exe")
def arg(path):
    return subprocess.run(["wslpath", "-w", path], capture_output=True, text=True).stdout.strip() if win_from_wsl else path
def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    d = lambda b: b.decode("utf-8", errors="replace") if b else ""
    return r.returncode, d(r.stdout) + d(r.stderr)

# ---------------------------------------------------------------------------------------------------------------------------------
# The reference: spec v31 chapter 6, written again here on purpose (not shared with the simulator's code).
def s16(v): return v - 0x10000 if v & 0x8000 else v
def u32(v): return v & 0xFFFFFFFF
def le(v, n): return [(v >> (8 * i)) & 0xFF for i in range(n)]

def alu_out(op, a, b, signed):
    """ALU_OUT as 32 bits. ARITH is exact (no 16-bit wrap): ADD/SUB to 17 bits, MUL to 32; the result is extended to 32 bits."""
    x, y = (s16(a), s16(b)) if signed else (a, b)
    if op == "ADD": return u32(x + y)
    if op == "SUB": return u32(x - y)
    if op == "MUL": return u32(x * y)
    if op == "INC": return u32(x + 1)
    if op == "DEC": return u32(x - 1)
    if op == "AND": return a & b
    if op == "OR": return a | b
    if op == "XOR": return a ^ b
    if op == "NOT": return (~a) & 0xFFFF
    raise ValueError(op)

def shift_out(direction, a, amount, signed):
    """SHL: the exact 24-bit result (the operand sign-extended first when signed). SHR: a 16-bit shift, arithmetic when signed."""
    if direction == "SHL": return u32((s16(a) if signed else a) << amount)
    return u32(s16(a) >> amount) if signed else a >> amount

def cmp_true(op, a, b, signed):
    x, y = (s16(a), s16(b)) if signed else (a, b)
    return {"EQ": a == b, "NE": a != b, "LT": x < y, "GE": x >= y}[op]

# ---------------------------------------------------------------------------------------------------------------------------------
class Test:
    def __init__(self, name, title, checks):
        self.name, self.title, self.checks = name, title, checks
        self.lines, self.expect, self.notes, self.env, self.params = [], [], [], "", []
        self.untouched, self.bus = [], []
        self.layer = 1                # 1 = encoding (c..), 2 = behaviour (b..)
        self.board = False            # uses the board's own CSRs (conf_board.inc): timers, poll, done / error
        self.before = []              # (offset a, offset b, why): RES + a is written before RES + b -- an order, not a time
        self.bus_check = []           # ("read" | "write", 7-bit address, acknowledged, data bytes): the I2C transactions, in order
        self.done_by = "main"         # which sequencer writes RES_DONE
    def asm(self, text=""):
        self.lines.append(text)
    def want(self, off, value, what, uncertain=""):
        """RES + off must end up holding `value` (one byte)."""
        self.expect.append((off, value & 0xFF, what, uncertain))
    def want_bytes(self, off, values, what, uncertain=""):
        for i, v in enumerate(values):
            self.want(off + i, v, what + ("（位元組 %d）" % i if len(values) > 1 else ""), uncertain)
    def never(self, off, what):
        """RES + off must never be written."""
        self.untouched.append((off, what))
    def finish(self):
        self.asm("    EXEC_CSR WRITE8 RES_DONE, 0x%02X          ; the last write: everything above is there" % DONE_VALUE)
        self.asm("done:")
        self.asm("    JMP done")

HEADER = """; {name} -- {title}
; Layer-{layer} conformance test (generated by conformance/make_conformance.py: edit that, not this file).
; Checks: {checks}
; Results go to the result CSRs RES + n (conf_defs.inc); the last write is RES_DONE = 0x{done:02X}. Expected values: {name}.expected.csv
.include "conf_defs.inc"
{board}"""

def load_ab(t, a, b):
    """ALU_OPA = a, ALU_OPB = b (constants cannot go to the ALU directly: through the scratchpad, spec v31 5.1)."""
    t.asm("    MOV CONST(0x%04X, 2) -> MAIN_SCRATCHPAD[0..1]" % a)
    t.asm("    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA")
    t.asm("    MOV CONST(0x%04X, 2) -> MAIN_SCRATCHPAD[2..3]" % b)
    t.asm("    MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB")

def store_out(t, off):
    t.asm("    MOV ALU_OUT[0..3] -> MAIN_SCRATCHPAD[8..11]")
    t.asm("    EXEC_CSR WRITE_SP RES + %d, len=4, ptr=8" % off)

tests = []
RW = "結果 CSR 必須是「寫什麼讀回什麼」的一般暫存器"

# ---- c01: the instruction byte, the CSR descriptors, the branch descriptor ---------------------------------------------------------
t = Test("c01_csr_branch", "EXEC_CSR one-byte forms and an unconditional branch",
         "opcode / arg split of the instruction byte; CsrOneByteDesc (operation, address) and its byte order; the immediate byte; BranchDesc target")
t.zh_title = "EXEC_CSR 單位元組的三種寫入，以及無條件分支"
t.zh_checks = "指令位元組的 opcode／arg 切分；CsrOneByteDesc（運算、位址）與它的位元組序；立即值位元組；BranchDesc 的目標"
t.asm(".main")
t.asm("    EXEC_CSR WRITE8 RES + 0, 0x5A              ; a plain write")
t.asm("    EXEC_CSR WRITE8 RES + 1, 0xF0")
t.asm("    EXEC_CSR SET8   RES + 1, 0x0F              ; read-modify-write OR:  0xF0 | 0x0F")
t.asm("    EXEC_CSR WRITE8 RES + 2, 0xFF")
t.asm("    EXEC_CSR CLR8   RES + 2, 0x3C              ; read-modify-write AND NOT:  0xFF & ~0x3C")
t.asm("    JMP over                                   ; the next write must be skipped")
t.asm("    EXEC_CSR WRITE8 RES + 3, 0xBD")
t.asm("over:")
t.asm("    EXEC_CSR WRITE8 RES + 4, 0x0D")
t.want(0, 0x5A, "WRITE8 直接寫入")
t.want(1, 0xFF, "先 WRITE8 0xF0，再 SET8 0x0F（讀出後 OR）", RW)
t.want(2, 0xC3, "先 WRITE8 0xFF，再 CLR8 0x3C（讀出後 AND NOT）", RW)
t.never(3, "被 JMP 跳過：這裡如果被寫入，表示分支沒有跳到它的目標")
t.want(4, 0x0D, "分支目標之後的那道指令")
t.notes.append("寫到了別的位址：16 位元的 CsrOneByteDesc 被以相反的位元組序讀取（D2），或位址欄位不在工具放的位置。")
t.notes.append("RES+1 = 0x0F 或 RES+2 = 0x00：SET8／CLR8 沒有先讀暫存器。RES+1 = 0xF0 且 RES+2 = 0xFF：SET8 與 CLR8 的運算碼對調了。")
tests.append(t)

# ---- c02: MOV, the const pool, WRITE_SP ---------------------------------------------------------------------------------------------
t = Test("c02_mov_const", "MOV between regions, the constant pool, EXEC_CSR WRITE_SP",
         "MainMoveDesc (regions, offsets, length) and its byte order; where the constant pool lies; byte order of a multi-byte write")
t.zh_title = "區域之間的 MOV、常數池、EXEC_CSR WRITE_SP"
t.zh_checks = "MainMoveDesc（區域、位移、長度）與它的位元組序；常數池的位置；多位元組寫入的位元組順序"
t.asm(".main")
t.asm(".const K_WORD dw 0x1234                        ; stored low byte first: 34 12")
t.asm(".const K_BYTES db 0x11, 0x22, 0x33, 0x44")
t.asm("    MOV MAIN_CONST_POOL[@K_WORD]  -> MAIN_SCRATCHPAD[0..1]")
t.asm("    EXEC_CSR WRITE_SP RES + 0, len=2, ptr=0")
t.asm("    MOV MAIN_CONST_POOL[@K_BYTES] -> MAIN_SCRATCHPAD[4..7]")
t.asm("    EXEC_CSR WRITE_SP RES + 4, len=4, ptr=4")
t.asm("    MOV MAIN_SCRATCHPAD[5..6] -> MAIN_SCRATCHPAD[10..11]     ; inside one region, not overlapping: source and destination offsets differ")
t.asm("    EXEC_CSR WRITE_SP RES + 8, len=2, ptr=10")
t.asm("    MOV CONST(0xCAFE, 2) -> MAIN_SCRATCHPAD[16..17]")
t.asm("    MOV MAIN_SCRATCHPAD[16..17] -> ALU_OPA                    ; through the ALU operand and back: OR with 0 changes nothing")
t.asm("    MOV CONST(0, 2) -> MAIN_SCRATCHPAD[18..19]")
t.asm("    MOV MAIN_SCRATCHPAD[18..19] -> ALU_OPB")
t.asm("    ALU OR")
t.asm("    MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[20..21]")
t.asm("    EXEC_CSR WRITE_SP RES + 12, len=2, ptr=20")
t.want_bytes(0, [0x34, 0x12], "dw 常數 0x1234，低位元組在前")
t.want_bytes(4, [0x11, 0x22, 0x33, 0x44], "四個 db 常數，依寫的順序")
t.want_bytes(8, [0x22, 0x33], "MAIN_SCRATCHPAD[5..6] 複製到 [10..11]", "硬體必須允許同一區域內不重疊的 MOV（profile same_region_mov）")
t.want_bytes(12, [0xFE, 0xCA], "0xCAFE 經過 ALU_OPA 與 ALU_OUT：位元組 0 是低位元組")
t.notes.append("RES+0..1 = 12 34：常數池或 ALU 區域是大端。")
t.notes.append("RES+8..9 = 11 22 或 33 44：MainMoveDesc 的來源位移與目的位移不在工具放的位置。")
t.notes.append("每一格都不對：32 位元的 MainMoveDesc 被以相反的位元組序讀取（D2），或 main_const_pool_offset 與 layout 不符。")
tests.append(t)

# ---- c03: ALU arithmetic --------------------------------------------------------------------------------------------------------------
t = Test("c03_alu_arith", "ALU ADD / SUB / MUL / INC / DEC",
         "the numeric codes of arith_op (D1); signed_mode; exact results (no 16-bit wrap)")
t.zh_title = "ALU 的 ADD／SUB／MUL／INC／DEC"
t.zh_checks = "arith_op 的編碼數值（D1）；signed_mode；精確結果（不做 16 位元回繞）"
t.asm(".main")
A, B = 7, 3
load_ab(t, A, B)
arith_rows = []
for i, op in enumerate(["ADD", "SUB", "MUL", "INC", "DEC"]):
    t.asm("    ALU %s unsigned" % op)
    store_out(t, 4 * i)
    v = alu_out(op, A, B, False)
    t.want_bytes(4 * i, le(v, 4), "%s 無號，OPA = %d、OPB = %d → %d" % (op, A, B, v))
    arith_rows.append((op, v))
A2, B2 = 0xFFFE, 3
load_ab(t, A2, B2)
for j, (op, sg) in enumerate([("ADD", True), ("SUB", True), ("MUL", True), ("ADD", False)]):
    off = 20 + 4 * j
    t.asm("    ALU %s %s" % (op, "signed" if sg else "unsigned"))
    store_out(t, off)
    v = alu_out(op, A2, B2, sg)
    t.want_bytes(off, le(v, 4), "%s %s，OPA = 0x%04X、OPB = %d → 0x%08X" % (op, "有號" if sg else "無號", A2, B2, v))
load_ab(t, 3, 7)
t.asm("    ALU SUB unsigned                           ; 3 - 7: the exact difference is -4, not a 16-bit wrap")
store_out(t, 36)
t.want_bytes(36, le(alu_out("SUB", 3, 7, False), 4), "SUB 無號 3 − 7 → 精確的 −4，以 32 位元表示",
             "規格說「精確的有號 17 位元差」；如何延伸到 ALU_OUT[31:0] 是我們的解讀（符號延伸）")
t.notes.append("OPA = 7、OPB = 3 時五種運算的結果都不同（" + "、".join("%s = %d" % r for r in arith_rows)
               + "）。某一格出現的是另一種運算的值，就知道硬體把那個編碼當成哪一種運算。")
t.notes.append("「ADD 有號」那一格（RES+20 起）是 01 00 01 00：signed_mode 沒有作用（那是無號的 0xFFFE + 3）。")
tests.append(t)

# ---- c04: ALU compare ------------------------------------------------------------------------------------------------------------------
t = Test("c04_alu_cmp", "ALU CMP.EQ / NE / LT / GE and BR.COND ALU_CMP_TRUE",
         "the numeric codes of cmp_op (D1); signed_mode of a compare; the ALU_CMP_TRUE branch condition")
t.zh_title = "ALU 的 CMP.EQ／NE／LT／GE，以及 BR.COND ALU_CMP_TRUE"
t.zh_checks = "cmp_op 的編碼數值（D1）；比較的 signed_mode；分支條件 ALU_CMP_TRUE"
t.asm(".main")
n = 0
sig = {}
def cmp_case(t, op, a, b, sg, off):
    t.asm("    ALU CMP.%s %s" % (op, "signed" if sg else "unsigned"))
    t.asm("    BR.COND ALU_CMP_TRUE, yes%d" % off)
    t.asm("    EXEC_CSR WRITE8 RES + %d, 0x00" % off)
    t.asm("    JMP next%d" % off)
    t.asm("yes%d:" % off)
    t.asm("    EXEC_CSR WRITE8 RES + %d, 0x01" % off)
    t.asm("next%d:" % off)
    r = 1 if cmp_true(op, a, b, sg) else 0
    t.want(off, r, "CMP.%s %s，OPA = %s、OPB = %s（成立寫 1，不成立寫 0）" % (op, "有號" if sg else "無號", a if a < 0x8000 else hex(a), b))
    return r
for a, b in [(5, 9), (9, 9), (9, 5)]:
    load_ab(t, a, b)
    for op in ["EQ", "NE", "LT", "GE"]:
        sig.setdefault(op, []).append(cmp_case(t, op, a, b, False, n))
        n += 1
load_ab(t, 0xFFFF, 1)
for op in ["LT", "GE"]:
    cmp_case(t, op, 0xFFFF, 1, True, n); n += 1
    cmp_case(t, op, 0xFFFF, 1, False, n); n += 1
t.notes.append("無號比較時，以 (OPA, OPB) = (5, 9)、(9, 9)、(9, 5) 三組運算元，每種比較各有自己的結果樣式（"
               + "、".join("%s = %s" % (op, " ".join(str(x) for x in sig[op])) for op in ["EQ", "NE", "LT", "GE"])
               + "）：把同一種比較的三格連起來看，就知道硬體實際做的是哪一種。")
t.notes.append("OPA = 0xFFFF、OPB = 1：有號是 −1 < 1（LT 成立），無號是 65535 > 1（LT 不成立）。有號與無號的答案相同：signed_mode 不在工具放的位置。")
t.notes.append("全部是 0 或全部是 1：分支條件 ALU_CMP_TRUE 的編碼（0x5）與硬體不同。")
tests.append(t)

# ---- c05: ALU logic --------------------------------------------------------------------------------------------------------------------
t = Test("c05_alu_logic", "ALU AND / OR / XOR / NOT", "the numeric codes of logic_op (D1)")
t.zh_title = "ALU 的 AND／OR／XOR／NOT"
t.zh_checks = "logic_op 的編碼數值（D1）"
t.asm(".main")
A, B = 0x0F0F, 0x00FF
load_ab(t, A, B)
rows = []
for i, op in enumerate(["AND", "OR", "XOR", "NOT"]):
    t.asm("    ALU %s" % op)
    store_out(t, 4 * i)
    v = alu_out(op, A, B, False)
    t.want_bytes(4 * i, le(v, 4), "%s，OPA = 0x%04X、OPB = 0x%04X → 0x%04X（高 16 位元為 0）" % (op, A, B, v))
    rows.append("%s = 0x%04X" % (op, v))
t.notes.append("四種運算的結果都不同（" + "、".join(rows) + "）：某一格出現的是另一種運算的值，就知道硬體用的是哪個編碼。")
tests.append(t)

# ---- c06: ALU shift --------------------------------------------------------------------------------------------------------------------
t = Test("c06_alu_shift", "ALU SHL / SHR", "the direction bit shift_dir (D6: is 0 SHL or SHR?); the amount field; signed_mode; SHL exact to 24 bits")
t.zh_title = "ALU 的 SHL／SHR"
t.zh_checks = "方向位元 shift_dir（D6：0 是 SHL 還是 SHR？）；位數欄位；signed_mode；SHL 保留精確的 24 位元結果"
t.asm(".main")
A = 0x8001
load_ab(t, A, 0)
cases = [("SHL", 1, False), ("SHR", 1, False), ("SHL", 1, True), ("SHR", 1, True), ("SHL", 8, False), ("SHR", 8, False), ("SHL", 3, False)]
for i, (d, amt, sg) in enumerate(cases):
    t.asm("    ALU %s %d %s" % (d, amt, "signed" if sg else "unsigned"))
    store_out(t, 4 * i)
    v = shift_out(d, A, amt, sg)
    t.want_bytes(4 * i, le(v, 4), "0x%04X %s %d 位，%s → 0x%08X" % (A, d, amt, "有號" if sg else "無號", v))
t.notes.append("RES+0 起是 00 40 00 00、RES+4 起是 02 00 01 00（兩格對調）：shift_dir 的方向相反——硬體的 0 是 SHR（D6）。")
t.notes.append("RES+0 起是 02 00 00 00：SHL 被截成 16 位元；規格要保留精確的 24 位元結果（這裡 bit 16 應為 1）。")
t.notes.append("RES+16 起與 RES+24 起（位數 8 與 3）不對、而 RES+0 正確：位數欄位的讀法不同。")
tests.append(t)

# ---- c07: ALU_STATUS -------------------------------------------------------------------------------------------------------------------
t = Test("c07_alu_status", "the flags of ALU_STATUS", "which bit of ALU_STATUS[0] is which flag (D4); which bit 'negative' takes for a logic result (D5)")
t.zh_title = "ALU_STATUS 的各個旗標"
t.zh_checks = "ALU_STATUS[0] 的哪一個位元是哪一個旗標（D4）；邏輯運算結果的 negative 取哪一位（D5）"
t.asm(".main")
FLAG = {"cmp_true": 0x01, "zero": 0x02, "negative": 0x04, "carry_u": 0x08, "borrow_u": 0x10, "overflow_s": 0x20, "ov16": 0x40}
LAYOUT = "各旗標的「位置」是假設（規格只列旗標、沒有給位元）：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16"
def status_case(t, off, a, b, alu, flags, why):
    load_ab(t, a, b)
    t.asm("    ALU %s" % alu)
    t.asm("    MOV ALU_STATUS[0..0] -> MAIN_SCRATCHPAD[8..8]")
    t.asm("    EXEC_CSR WRITE_SP RES + %d, len=1, ptr=8" % off)
    v = 0
    for f in flags: v |= FLAG[f]
    t.want(off, v, "%s，OPA = 0x%04X、OPB = 0x%04X：%s（%s）" % (alu, a, b, " + ".join(flags) if flags else "沒有旗標", why), LAYOUT)
status_case(t, 0, 0x0000, 0x0000, "AND", ["zero"], "結果為 0")
status_case(t, 1, 0x8000, 0x0000, "OR", ["negative"], "邏輯結果的 bit 15")
status_case(t, 2, 0x8000, 0x0000, "SHL 1 unsigned", ["ov16"], "0x10000 放不進 16 位元")
status_case(t, 3, 0xFFFF, 0x0001, "ADD unsigned", ["carry_u", "ov16"], "0x10000：16 位元的進位")
status_case(t, 4, 0x0003, 0x0007, "SUB unsigned", ["negative", "borrow_u", "ov16"], "3 − 7 發生借位")
status_case(t, 5, 0x7FFF, 0x0001, "ADD signed", ["overflow_s", "ov16"], "32767 + 1 超出有號 16 位元的範圍")
status_case(t, 6, 0x0001, 0x0001, "ADD unsigned", [], "結果是 2，沒有特別的狀況")
status_case(t, 7, 0x0005, 0x0005, "CMP.EQ unsigned", ["cmp_true"], "相等；前一道 ADD 的旗標保留——比較只改 cmp_true")
status_case(t, 8, 0x0001, 0x0001, "ADD unsigned", ["cmp_true"], "cmp_true 保持到下一次比較")
t.notes.append("RES+0、RES+1、RES+2 各只有一個旗標（zero、negative、ov16）：那一格被設起來的位元，就是硬體放那個旗標的位置。")
t.notes.append("carry_u＝RES+3 扣掉 ov16 的位元；overflow_s＝RES+5 扣掉 ov16；borrow_u＝RES+4 扣掉 negative 與 ov16。")
t.notes.append("RES+8 沒有 cmp_true 的位元：硬體在其他運算時會清掉 cmp_true（工具的假設是保持到下一次比較）。")
tests.append(t)

# ---- c08: cascade ----------------------------------------------------------------------------------------------------------------------
t = Test("c08_cascade", "CASCADE_BEGIN / CASCADE_END", "MainBranchDesc types 1 and 2; cumulative blocks: selector k runs block 0 .. block k")
t.zh_title = "CASCADE_BEGIN／CASCADE_END"
t.zh_checks = "MainBranchDesc 的型別 1 與 2；累進的區塊：選擇值 k 會執行區塊 0 到區塊 k"
t.asm(".main")
t.asm("    CASCADE_BEGIN 1, blk0                      ; selector 1: block 0 and block 1 run, block 2 does not")
t.asm("blk0:")
t.asm("    EXEC_CSR WRITE8 RES + 0, 0xB0")
t.asm("    CASCADE_END 0, out")
t.asm("    EXEC_CSR WRITE8 RES + 1, 0xB1")
t.asm("    CASCADE_END 1, out")
t.asm("    EXEC_CSR WRITE8 RES + 2, 0xB2")
t.asm("    CASCADE_END 2, out")
t.asm("out:")
t.asm("    EXEC_CSR WRITE8 RES + 3, 0xEE")
t.want(0, 0xB0, "區塊 0")
t.want(1, 0xB1, "區塊 1（選到的最後一個區塊）")
t.never(2, "區塊 2 在選到的區塊之後：這裡被寫入表示 cascade 沒有在選擇值的地方結束")
t.want(3, 0xEE, "離開的目標")
t.notes.append("只有 RES+1 被寫入（RES+0 沒有）：硬體只跳到區塊 k——那是 switch，不是規格的累進式 cascade。")
tests.append(t)

# ---- c09: CSR read back ----------------------------------------------------------------------------------------------------------------
t = Test("c09_csr_read", "EXEC_CSR READ8 / READ_BUF and MAIN_CSR_DATA_BUF",
         "CsrOneByteDesc READ8 and its destination pointer; CsrMultiByteDesc (page, length, pointer); MOV from MAIN_CSR_DATA_BUF")
t.zh_title = "EXEC_CSR READ8／READ_BUF 與 MAIN_CSR_DATA_BUF"
t.zh_checks = "CsrOneByteDesc 的 READ8 與目的指標；CsrMultiByteDesc（頁、長度、指標）；從 MAIN_CSR_DATA_BUF 的 MOV"
t.asm(".main")
t.asm("    EXEC_CSR WRITE8 RES + 16, 0x3C")
t.asm("    EXEC_CSR READ8  RES + 16 -> MAIN_CSR_DATA_BUF[2]")
t.asm("    MOV MAIN_CSR_DATA_BUF[2..2] -> MAIN_SCRATCHPAD[0..0]")
t.asm("    EXEC_CSR WRITE_SP RES + 0, len=1, ptr=0")
t.asm("    EXEC_CSR WRITE8 RES + 20, 0xA1")
t.asm("    EXEC_CSR WRITE8 RES + 21, 0xA2")
t.asm("    EXEC_CSR WRITE8 RES + 22, 0xA3")
t.asm("    EXEC_CSR READ_BUF RES + 20, len=3, ptr=5")
t.asm("    MOV MAIN_CSR_DATA_BUF[5..7] -> MAIN_SCRATCHPAD[4..6]")
t.asm("    EXEC_CSR WRITE_SP RES + 4, len=3, ptr=4")
t.want(16, 0x3C, "準備被讀回的值")
t.want(0, 0x3C, "READ8 把 RES+16 讀進 MAIN_CSR_DATA_BUF[2]，再寫出來", RW)
t.want_bytes(20, [0xA1, 0xA2, 0xA3], "準備被讀回的三個值")
t.want_bytes(4, [0xA1, 0xA2, 0xA3], "READ_BUF 把 RES+20..22 讀進 MAIN_CSR_DATA_BUF[5..7]，再寫出來", RW)
t.notes.append("RES+4..6 的順序不同：多位元組讀取不是依位址遞增、照緩衝區的順序放。")
tests.append(t)

# ---- c10: Peri -------------------------------------------------------------------------------------------------------------------------
DEV_ADDR, WRONG_ADDR = 0x2A, 0x55
t = Test("c10_peri_xfer_pub", "Peri: EXEC_XFER, EXEC_PUB, and Main taking the result",
         "DeviceMap packing and index (D3); XferOneByte / XferMultiByte descriptors; PeriPublishDesc; PERI_RESULT_META; the result slot")
t.zh_title = "Peri：EXEC_XFER、EXEC_PUB，以及 Main 取用結果"
t.zh_checks = "DeviceMap 的排列與索引（D3）；XferOneByte／XferMultiByte 描述子；PeriPublishDesc；PERI_RESULT_META；結果槽"
t.asm(".service SID_TEST = 3")
t.asm(".devmap 0, I2C, 0x%02X, DEV_OTHER            ; entry 0 is NOT the one used: an access to 0x%02X means the index is read wrongly" % (WRONG_ADDR, WRONG_ADDR))
t.asm(".devmap 1, I2C, 0x%02X, DEV_TEST" % DEV_ADDR)
t.asm(".main")
t.asm("wait:")
t.asm("    BR.COND PERI_RESULT_VALID, take")
t.asm("    JMP wait")
t.asm("take:")
t.asm("    MOV PERI_RESULT_META[0..0] -> MAIN_SCRATCHPAD[0..0]       ; the service id (before the slot is consumed)")
t.asm("    EXEC_CSR WRITE_SP RES + 0, len=1, ptr=0")
t.asm("    MOV PERI_RESULT_SLOT[0..4] -> MAIN_SCRATCHPAD[4..8]       ; consumes the result")
t.asm("    EXEC_CSR WRITE_SP RES + 4, len=5, ptr=4")
t.finish()
t.asm(".peri")
t.asm("    EXEC_CSR WRITE8 RES + 32, 0x9E             ; Peri reaches the CSRs too")
t.asm("    EXEC_XFER WRITE dev=DEV_TEST, addr=0x30, addrlen=1, data=0x77")
t.asm("    EXEC_XFER READ  dev=DEV_TEST, addr=0x10, addrlen=1, ptr=0")
t.asm("    EXEC_XFER READ  dev=DEV_TEST, len=4, ptr=1, ext=[0x20]")
t.asm("    EXEC_PUB service=SID_TEST, src=0, len=5")
t.asm("pdone:")
t.asm("    JMP pdone")
t.want(32, 0x9E, "Peri 的 EXEC_CSR WRITE8")
t.want(0, 3, "PERI_RESULT_META[0]＝這次發佈的 service id", "PERI_RESULT_META 的位元組排列是假設（位元組 0 為 service id）")
t.want_bytes(4, [0xC3, 0xD0, 0xD1, 0xD2, 0xD3], "發佈出來的 PERI_RX_BUF[0..4]：先是單位元組讀取，再是四位元組讀取")
t.env = "[device conf]\naddr = 0x%02X\nreg 0x10 = 0xC3\nblock 0x20 = 0xD0 0xD1 0xD2 0xD3\n" % DEV_ADDR
t.bus = [("寫入", DEV_ADDR, "暫存器 0x30", "資料 0x77"),
         ("讀取", DEV_ADDR, "暫存器 0x10", "1 個位元組，裝置回 0xC3"),
         ("讀取", DEV_ADDR, "暫存器 0x20", "4 個位元組，裝置回 0xD0 0xD1 0xD2 0xD3")]
t.notes.append("測試平台需要一個位址為 0x%02X 的 I2C 裝置：暫存器 0x10 讀到 0xC3，暫存器 0x20..0x23 讀到 0xD0 0xD1 0xD2 0xD3；寫入暫存器 0x30 要回 ACK。" % DEV_ADDR)
t.notes.append("交易是對位址 0x%02X 發出的：用的是 DeviceMap 第 0 筆而不是第 1 筆。完全是另一個位址：項目的位元排列不同（D3）。" % WRONG_ADDR)
t.notes.append("RES+4 起是 D0 D1 D2 D3 C3 或整個位移：讀取的目的指標不在工具放的位置。")
t.notes.append("這支測試 Main 與 Peri 同時執行，是十支裡唯一用到 Peri、I2C 與結果槽的。")
tests.append(t)
for x in tests:
    if x.name != "c10_peri_xfer_pub": x.finish()
tests[-1].bus_check = [("write", DEV_ADDR, True, 1), ("read", DEV_ADDR, True, 1), ("read", DEV_ADDR, True, 4)]

# =================================================================================================================================
# Layer 2: behaviour. The same program on both sides must DO the same things in the same order: which CSR is written before which, which I2C
# transactions go out, what the result slot keeps. Orders are checked, never times (that is layer 3). Tests marked `board` use the board's own
# CSRs -- timers, poll, done / error -- whose addresses and bits the spec does not give: conf_board.inc holds them, with EXAMPLE values.
# =================================================================================================================================
BOARD_NOTE = "用到板子自己的 CSR（計時器、poll、done／error）：先把 conf_board.inc 改成實際的位址與位元"
T_SHORT, T_LONG = 2000, 65000

def wait_main_timer(t, tag):
    t.asm("    EXEC_CSR SET8 MAIN_CTRL, MAIN_TIMER_START")
    t.asm("%s:" % tag)
    t.asm("    BR.COND MAIN_TIMER_DONE, %s_done" % tag)
    t.asm("    JMP %s" % tag)
    t.asm("%s_done:" % tag)
def wait_peri_timer(t, tag):
    t.asm("    EXEC_CSR SET8 PERI_CTRL, PERI_TIMER_START")
    t.asm("%s:" % tag)
    t.asm("    BR.IF_TIMER_DONE %s_done" % tag)
    t.asm("    JMP %s" % tag)
    t.asm("%s_done:" % tag)

# ---- b01: one timer or two? (docs/SIM_VS_SPEC.md, Q1) -----------------------------------------------------------------------------------
t = Test("b01_timers", "the Main timer and the Peri timer run side by side",
         "are MAIN_TIMER_DONE and IF_TIMER_DONE two timers or one (Q1); a start clears the done flag; a timer can be started again")
t.layer, t.board = 2, True
t.zh_title = "Main 的計時器與 Peri 的計時器同時計時"
t.zh_checks = "MAIN_TIMER_DONE 與 IF_TIMER_DONE 是兩顆計時器還是一顆（Q1）；啟動會清除完成旗標；計時器可以再次啟動"
t.asm(".equ T_SHORT = %d                            ; ticks: Main waits this long, twice" % T_SHORT)
t.asm(".equ T_LONG  = %d                           ; ticks: Peri waits this long, twice -- far longer than everything Main does first" % T_LONG)
t.asm(".main")
t.asm("    EXEC_CSR WRITE8 MAIN_TPER,   T_SHORT & 0xFF")
t.asm("    EXEC_CSR WRITE8 MAIN_TPER_H, T_SHORT >> 8")
wait_main_timer(t, "m1")
t.asm("    EXEC_CSR WRITE8 RES + 0, 0x11              ; Main's first short wait is over")
wait_main_timer(t, "m2")
t.asm("    EXEC_CSR WRITE8 RES + 1, 0x12              ; ... and the second one")
t.asm("    EXEC_CSR WRITE8 MAIN_TPER,   T_LONG & 0xFF ; now three long waits, so that Main ends after Peri in every case")
t.asm("    EXEC_CSR WRITE8 MAIN_TPER_H, T_LONG >> 8")
wait_main_timer(t, "m3")
wait_main_timer(t, "m4")
wait_main_timer(t, "m5")
t.finish()
t.asm(".peri")
t.asm("    EXEC_CSR SET8 PERI_CTRL, PERI_RESET")
t.asm("    EXEC_CSR WRITE8 PERI_TPER,   T_LONG & 0xFF")
t.asm("    EXEC_CSR WRITE8 PERI_TPER_H, T_LONG >> 8")
wait_peri_timer(t, "p1")
wait_peri_timer(t, "p2")
t.asm("    EXEC_CSR WRITE8 RES + 8, 0x21              ; Peri's two long waits are over")
t.asm("phalt:")
t.asm("    JMP phalt")
t.want(0, 0x11, "Main 第一次短計時結束", BOARD_NOTE)
t.want(1, 0x12, "Main 第二次短計時結束", BOARD_NOTE)
t.want(8, 0x21, "Peri 兩次長計時結束", BOARD_NOTE)
t.before = [(0, 1, "Main 的兩次短計時依序完成"),
            (1, 8, "兩顆獨立的計時器：Peri 的長計時不受 Main 啟動自己的計時器影響，所以 Peri 的標記在 Main 兩次短計時之後才出現"),
            (8, 0x7F, "Main 之後又等了三次長計時，一定比 Peri 晚結束")]
t.notes.append("**RES+8 比 RES+1（甚至 RES+0）早出現：硬體只有一顆計時器**——Main 啟動計時器時，把 Peri 正在等的那一次蓋掉了，Peri 跟著 Main 的短計時一起「完成」。這是 Q1 的答案。")
t.notes.append("RES+8 一直沒有出現：一顆共用的計時器，而且完成旗標只有啟動它的一方看得到（或被另一方的啟動清掉）。")
t.notes.append("RES+0 與 RES+1 幾乎同時出現：再次啟動時完成旗標沒有被清除（工具的假設是啟動會清除）。")
t.notes.append("時間上的參考（不列入判定）：兩顆獨立時，RES+8 約在開始後 2 × %d 個 tick 出現，RES+0 約在 %d 個 tick 之後。" % (T_LONG, T_SHORT))
tests.append(t)

# ---- b02: poll --------------------------------------------------------------------------------------------------------------------------
DEV_B = 0x2B
t = Test("b02_poll_ready", "Peri polls a status register until its ready bit, then reads the data",
         "the Poll Unit: mask, clear, arm, the evaluation on a successful read into PERI_RX_BUF[0]; BR.IF_READY")
t.layer, t.board = 2, True
t.zh_title = "Peri 輪詢狀態暫存器直到 ready 位元，再讀資料"
t.zh_checks = "Poll 單元：遮罩、清除、arm、在成功讀進 PERI_RX_BUF[0] 時評估；BR.IF_READY"
t.asm(".service SID_POLL = 4")
t.asm(".devmap 0, I2C, 0x%02X, DEV_POLL" % DEV_B)
t.asm(".main")
t.asm("wait:")
t.asm("    BR.COND PERI_RESULT_VALID, take")
t.asm("    JMP wait")
t.asm("take:")
t.asm("    MOV PERI_RESULT_SLOT[0..1] -> MAIN_SCRATCHPAD[0..1]")
t.asm("    EXEC_CSR WRITE_SP RES + 0, len=2, ptr=0")
t.finish()
t.asm(".peri")
t.asm("    EXEC_CSR SET8 PERI_CTRL, PERI_RESET")
t.asm("    EXEC_CSR WRITE8 POLL_MASK, 0x01            ; ready = bit 0 of the status byte")
t.asm("poll:")
t.asm("    EXEC_CSR SET8 POLL_CTRL, POLL_CLEAR_W1P")
t.asm("    EXEC_CSR SET8 POLL_CTRL, POLL_ARM_W1P      ; the next successful READ into PERI_RX_BUF[0] is evaluated")
t.asm("    EXEC_XFER READ dev=DEV_POLL, addr=0x05, addrlen=1, ptr=0")
t.asm("    BR.IF_READY ready")
t.asm("    JMP poll")
t.asm("ready:")
t.asm("    EXEC_XFER READ dev=DEV_POLL, len=2, ptr=2, ext=[0x40]")
t.asm("    EXEC_PUB service=SID_POLL, src=2, len=2")
t.asm("phalt:")
t.asm("    JMP phalt")
t.want_bytes(0, [0xE0, 0xE1], "ready 之後讀到並發佈的兩個位元組", BOARD_NOTE)
t.env = "[device poll]\naddr = 0x%02X\nreg 0x05 = 0x00, 0x00, 0x01\nblock 0x40 = 0xE0 0xE1\n" % DEV_B
t.bus = [("讀取", DEV_B, "暫存器 0x05（狀態）", "1 個位元組，裝置回 0x00（尚未就緒）"),
         ("讀取", DEV_B, "暫存器 0x05（狀態）", "1 個位元組，裝置回 0x00"),
         ("讀取", DEV_B, "暫存器 0x05（狀態）", "1 個位元組，裝置回 0x01（就緒）"),
         ("讀取", DEV_B, "暫存器 0x40", "2 個位元組，裝置回 0xE0 0xE1")]
t.bus_check = [("read", DEV_B, True, 1)] * 3 + [("read", DEV_B, True, 2)]
t.notes.append("測試平台需要位址 0x%02X 的 I2C 裝置：暫存器 0x05 前兩次讀到 0x00、第三次起讀到 0x01；暫存器 0x40、0x41 讀到 0xE0、0xE1。" % DEV_B)
t.notes.append("狀態只被讀了一次或兩次就去讀資料：Poll 的評估不是「讀到的位元組 AND 遮罩」，或 ready 在 arm 時就被設起來。")
t.notes.append("狀態一直被讀、永遠不讀資料：遮罩或 arm 沒有生效（POLL_MASK／POLL_CTRL 的位址或位元與 conf_board.inc 不符），或評估的不是 PERI_RX_BUF[0]。")
tests.append(t)

# ---- b03: the result slot keeps the old result -------------------------------------------------------------------------------------------
DEV_C = 0x2C
t = Test("b03_slot_overrun", "two publishes before Main takes the result: the first one is kept",
         "the result slot: Keep Old Drop New, the overrun flag, PERI_RESULT_OVERRUN")
t.layer = 2
t.zh_title = "Main 取用之前 Peri 發佈了兩次：保留第一筆"
t.zh_checks = "結果槽：保留舊的、丟掉新的（Keep Old Drop New）；overrun 旗標；PERI_RESULT_OVERRUN"
t.asm(".service SID_FIRST = 1")
t.asm(".service SID_SECOND = 2")
t.asm(".devmap 0, I2C, 0x%02X, DEV_SLOT" % DEV_C)
t.asm(".main")
t.asm("wait:")
t.asm("    BR.COND PERI_RESULT_OVERRUN, take          ; only once the SECOND publish has found the slot full")
t.asm("    JMP wait")
t.asm("take:")
t.asm("    MOV PERI_RESULT_META[0..0] -> MAIN_SCRATCHPAD[0..0]")
t.asm("    EXEC_CSR WRITE_SP RES + 0, len=1, ptr=0")
t.asm("    MOV PERI_RESULT_SLOT[0..0] -> MAIN_SCRATCHPAD[4..4]")
t.asm("    EXEC_CSR WRITE_SP RES + 1, len=1, ptr=4")
t.finish()
t.asm(".peri")
t.asm("    EXEC_XFER READ dev=DEV_SLOT, addr=0x01, addrlen=1, ptr=0")
t.asm("    EXEC_PUB service=SID_FIRST, src=0, len=1")
t.asm("    EXEC_XFER READ dev=DEV_SLOT, addr=0x02, addrlen=1, ptr=0")
t.asm("    EXEC_PUB service=SID_SECOND, src=0, len=1  ; the slot still holds the first result: this one is dropped, overrun is set")
t.asm("phalt:")
t.asm("    JMP phalt")
t.want(0, 1, "結果槽裡的 service id：第一次發佈的（1）", "PERI_RESULT_META 的位元組排列是假設（位元組 0 為 service id）")
t.want(1, 0xA1, "結果槽裡的資料：第一次發佈的")
t.env = "[device slot]\naddr = 0x%02X\nreg 0x01 = 0xA1\nreg 0x02 = 0xA2\n" % DEV_C
t.bus = [("讀取", DEV_C, "暫存器 0x01", "1 個位元組，裝置回 0xA1"), ("讀取", DEV_C, "暫存器 0x02", "1 個位元組，裝置回 0xA2")]
t.bus_check = [("read", DEV_C, True, 1), ("read", DEV_C, True, 1)]
t.notes.append("測試平台需要位址 0x%02X 的 I2C 裝置：暫存器 0x01 讀到 0xA1，暫存器 0x02 讀到 0xA2。" % DEV_C)
t.notes.append("RES+0 = 2、RES+1 = 0xA2：硬體用新的結果蓋掉舊的（規格是保留舊的、丟掉新的）。")
t.notes.append("RES_DONE 一直沒有出現：第二次發佈沒有設 overrun，Main 一直在等 PERI_RESULT_OVERRUN。")
tests.append(t)

# ---- b04: no acknowledge ------------------------------------------------------------------------------------------------------------------
DEV_NONE = 0x2D
t = Test("b04_nack_error", "a transfer nobody acknowledges sets the Peri error latch",
         "NACK -> the sticky error latch; BR.IF_ERROR / IF_NO_ERROR; the sequencer goes on after a NACK")
t.layer = 2
t.done_by = "peri"
t.zh_title = "沒有裝置回應的傳輸會設起 Peri 的 error 旗標"
t.zh_checks = "NACK → error 旗標（保持住）；BR.IF_ERROR／IF_NO_ERROR；NACK 之後 sequencer 繼續執行"
t.asm(".devmap 0, I2C, 0x%02X, DEV_ABSENT          ; NO device answers at this address on the bench" % DEV_NONE)
t.asm(".peri")
t.asm("    BR.IF_NO_ERROR clean                       ; before any transfer the latch is clear")
t.asm("    EXEC_CSR WRITE8 RES + 0, 0xE0")
t.asm("    JMP go")
t.asm("clean:")
t.asm("    EXEC_CSR WRITE8 RES + 0, 0x0C")
t.asm("go:")
t.asm("    EXEC_XFER READ dev=DEV_ABSENT, addr=0x00, addrlen=1, ptr=0")
t.asm("    BR.IF_ERROR failed")
t.asm("    EXEC_CSR WRITE8 RES + 1, 0x0E")
t.asm("    JMP after")
t.asm("failed:")
t.asm("    EXEC_CSR WRITE8 RES + 1, 0xE1")
t.asm("after:")
t.asm("    BR.IF_ERROR still                          ; the latch stays set (nothing cleared it)")
t.asm("    EXEC_CSR WRITE8 RES + 2, 0x02")
t.asm("    JMP fin")
t.asm("still:")
t.asm("    EXEC_CSR WRITE8 RES + 2, 0xE2")
t.asm("fin:")
t.want(0, 0x0C, "傳輸之前：error 旗標是清的")
t.want(1, 0xE1, "沒有回應的傳輸之後：error 旗標被設起")
t.want(2, 0xE2, "再查一次：error 旗標保持住")
t.before = [(0, 1, "依程式順序"), (1, 2, "依程式順序")]
t.bus = [("讀取", DEV_NONE, "暫存器 0x00", "沒有裝置回應（NACK）")]
t.bus_check = [("read", DEV_NONE, False, 1)]
t.notes.append("測試平台在位址 0x%02X **不可以**有裝置。這支只有 Peri 程式，完成標記由 Peri 寫入。" % DEV_NONE)
t.notes.append("RES+1 = 0x0E：NACK 沒有設 error 旗標。RES+1 沒有被寫入、程式停住：NACK 被當成 fault，sequencer 停了（工具的假設是照常往下執行）。")
t.notes.append("RES+2 = 0x02：error 旗標沒有保持住（被分支讀取清掉）。")
tests.append(t)

# ---- b05: done / error as Main sees them ----------------------------------------------------------------------------------------------------
t = Test("b05_done_error", "Peri reports a good service, then a failed one; Main branches on PERI_NO_ERROR / PERI_ERROR",
         "PERI_NO_ERROR = done and no error, PERI_ERROR = done and error; the service reset clears both latches")
t.layer, t.board = 2, True
t.zh_title = "Peri 先回報一次成功的 service、再回報一次失敗的；Main 以 PERI_NO_ERROR／PERI_ERROR 分支"
t.zh_checks = "PERI_NO_ERROR＝done 且沒有 error，PERI_ERROR＝done 且有 error；service reset 會清掉兩個旗標"
t.asm(".devmap 0, I2C, 0x%02X, DEV_GOOD" % DEV_B)
t.asm(".devmap 1, I2C, 0x%02X, DEV_ABSENT          ; NO device answers at this address on the bench" % DEV_NONE)
t.asm(".main")
t.asm("w1:")
t.asm("    BR.COND PERI_NO_ERROR, good                ; the first service ends well")
t.asm("    BR.COND PERI_ERROR, early")
t.asm("    JMP w1")
t.asm("early:")
t.asm("    EXEC_CSR WRITE8 RES + 0, 0xEE              ; (not expected: an error before the good service)")
t.asm("    JMP w2")
t.asm("good:")
t.asm("    EXEC_CSR WRITE8 RES + 0, 0x0A")
t.asm("w2:")
t.asm("    BR.COND PERI_ERROR, bad                    ; the second service fails")
t.asm("    JMP w2")
t.asm("bad:")
t.asm("    EXEC_CSR WRITE8 RES + 1, 0xEB")
t.finish()
t.asm(".peri")
t.asm("    EXEC_CSR SET8 PERI_CTRL, PERI_RESET        ; service 1: a transfer that works")
t.asm("    EXEC_XFER READ dev=DEV_GOOD, addr=0x40, addrlen=1, ptr=0")
t.asm("    EXEC_CSR SET8 PERI_CTRL, PERI_DONE")
t.asm("    EXEC_CSR WRITE8 PERI_TPER,   T_GAP & 0xFF  ; leave Main the time to see it")
t.asm("    EXEC_CSR WRITE8 PERI_TPER_H, T_GAP >> 8")
wait_peri_timer(t, "gap")
t.asm("    EXEC_CSR SET8 PERI_CTRL, PERI_RESET        ; service 2: nobody answers")
t.asm("    EXEC_XFER READ dev=DEV_ABSENT, addr=0x00, addrlen=1, ptr=0")
t.asm("    EXEC_CSR SET8 PERI_CTRL, PERI_DONE")
t.asm("phalt:")
t.asm("    JMP phalt")
t.lines.insert(0, ".equ T_GAP = 60000                           ; ticks between the two services")
t.want(0, 0x0A, "第一個 service：done 且沒有 error → PERI_NO_ERROR", BOARD_NOTE)
t.want(1, 0xEB, "第二個 service：done 且有 error → PERI_ERROR", BOARD_NOTE)
t.before = [(0, 1, "先成功的那一次，再失敗的那一次")]
t.env = "[device good]\naddr = 0x%02X\nreg 0x40 = 0xE0\n" % DEV_B
t.bus = [("讀取", DEV_B, "暫存器 0x40", "1 個位元組，裝置回 0xE0"), ("讀取", DEV_NONE, "暫存器 0x00", "沒有裝置回應（NACK）")]
t.bus_check = [("read", DEV_B, True, 1), ("read", DEV_NONE, False, 1)]
t.notes.append("測試平台需要位址 0x%02X 的 I2C 裝置（暫存器 0x40 讀到 0xE0），而位址 0x%02X **不可以**有裝置。" % (DEV_B, DEV_NONE))
t.notes.append("RES+0 = 0xEE：第一個 service 就被當成有 error（error 旗標在 reset 後不是清的，或 PERI_ERROR 不看 done）。")
t.notes.append("RES+1 一直沒有出現：NACK 沒有設 error，或第二次的 done 沒有生效。RES+0 一直沒有出現：PERI_CTRL 的 done 位元與 conf_board.inc 不符。")
tests.append(t)
for x in tests:
    if x.layer == 2 and x.done_by == "main" and not any("RES_DONE" in l for l in x.lines): x.finish()
    if x.layer == 2 and x.done_by == "peri":
        x.asm("    EXEC_CSR WRITE8 RES_DONE, 0x%02X          ; the last write: everything above is there" % DONE_VALUE)
        x.asm("pdone:")
        x.asm("    JMP pdone")


# ---------------------------------------------------------------------------------------------------------------------------------
def source(t):
    board = '.include "conf_board.inc"                      ; the CSRs of the board: put YOUR addresses and bits there first\n' if t.board else ""
    return HEADER.format(name=t.name, title=t.title, checks=t.checks, done=DONE_VALUE, layer=t.layer, board=board) + "\n".join(t.lines) + "\n"

DEFS = ("; The result CSRs of the conformance tests: 128 addresses the tests write what they computed to. They must not be real registers of the\n"
        "; board that do something when written; tests that read back (c01 SET8 / CLR8, c09) need them to return what was written.\n"
        "; Change the base with:  make_conformance.py --res-base 0x...   (do not edit: generated)\n"
        ".csr RES      = 0x%03X\n.csr RES_DONE = 0x%03X          ; the last write of every test: 0x%02X\n" % (RES, DONE, DONE_VALUE))

# The board's own CSRs, as the layer-2 tests use them. EXAMPLE addresses and bits (those of sample/common/defs.inc): the spec names these registers
# and does not number them. Whoever runs the tests on the hardware puts the real ones here; conf_board_env.cfg tells OUR simulator what they do.
BOARD = """; The board's own CSRs used by the layer-2 tests (b01, b02, b05): the timers, the Poll Unit, done / error.
; EXAMPLE addresses and bits: the spec names these registers but does not number them. PUT YOUR BOARD'S VALUES HERE before running on the
; hardware, then run make_conformance.py again (this file is kept when it exists; delete it to get the examples back).
.csr POLL_MASK   = 0x300        ; which bits of the status byte mean "ready"
.csr POLL_CTRL   = 0x301        ; write 1 to: bit 0 clear the poll latches, bit 1 arm the Poll Unit
.csr PERI_CTRL   = 0x302        ; write 1 to: bit 0 set done, bit 1 set error, bit 2 clear the poll / error / done latches, bit 3 start the Peri timer
.csr PERI_TPER   = 0x310        ; Peri timer period in ticks, low byte
.csr PERI_TPER_H = 0x311        ; ... high byte
.csr MAIN_TPER   = 0x320        ; Main timer period in ticks, low byte
.csr MAIN_TPER_H = 0x321        ; ... high byte
.csr MAIN_CTRL   = 0x322        ; write 1 to: bit 0 start the Main timer

.equ POLL_CLEAR_W1P   = 0x01
.equ POLL_ARM_W1P     = 0x02
.equ PERI_DONE        = 0x01
.equ PERI_ERROR_SET   = 0x02
.equ PERI_RESET       = 0x04
.equ PERI_TIMER_START = 0x08
.equ MAIN_TIMER_START = 0x01

; what the bits do, for the assembler's flow checks (timer started before it is tested, poll armed before it is read)
.csrbit POLL_CTRL[0] = poll_clear
.csrbit POLL_CTRL[1] = poll_arm
.csrbit PERI_CTRL[3] = timer_start
"""
BOARD_ENV = """; What the board's CSRs of conf_board.inc DO, for this project's simulator (asmsim --env). The hardware needs no such file.
[csr POLL_MASK]
kind = value
bind = poll_mask
[csr POLL_CTRL]
kind = action
bit0 = poll_clear
bit1 = poll_arm
[csr PERI_CTRL]
kind = action
bit0 = done_set
bit1 = error_set
bit2 = service_reset
bit3 = timer_start
[csr PERI_TPER]
kind = value
bind = timer_period
bytes = 2
[csr MAIN_TPER]
kind = value
bind = main_timer_period
bytes = 2
[csr MAIN_CTRL]
kind = action
bit0 = main_timer_start
"""

def read_events(path):
    """The simulator's event log (asmsim --events): the writes to the result CSRs in the order they happened, and the I2C transactions."""
    import csv
    writes, bus = [], []
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        for row in csv.DictReader(f):
            if row["type"] == "csr" and row["what"] in ("write", "set", "clr") and RES <= int(row["a"]) <= DONE:
                writes.append((int(row["a"]) - RES, int(row["b"]), row["domain"]))
            elif row["type"] == "bus" and row["what"] in ("i2c.read", "i2c.write"):
                bus.append((row["what"][4:], int(row["a"]), int(row["b"]) != 0, int(row["c"])))
    return writes, bus

def check_behaviour(t, writes, bus):
    """The orders and the bus transactions the test asks for; returns the list of what is not so."""
    bad = []
    first = {}
    for i, (off, _, _) in enumerate(writes): first.setdefault(off, i)
    for a_, b_, why in t.before:
        if a_ not in first or b_ not in first: bad.append("RES+%d or RES+%d was never written" % (a_, b_))
        elif not first[a_] < first[b_]: bad.append("RES+%d is not written before RES+%d (%s)" % (a_, b_, why))
    for off, _ in t.untouched:
        if off in first: bad.append("RES+%d was written" % off)
    if t.bus_check and bus != t.bus_check: bad.append("I2C transactions %s, expected %s" % (bus, t.bus_check))
    return bad

def expected_csv(t):
    s = "位址,位移,預期值,意義,取決於\n"
    for off, v, what, unc in sorted(t.expect):
        s += "0x%03X,RES+%d,0x%02X,\"%s\",\"%s\"\n" % (RES + off, off, v, what.replace('"', "'"), unc.replace('"', "'"))
    for off, what in sorted(t.untouched):
        s += "0x%03X,RES+%d,不可被寫入,\"%s\",\"\"\n" % (RES + off, off, what.replace('"', "'"))
    s += "0x%03X,RES_DONE,0x%02X,\"測試的最後一次寫入\",\"\"\n" % (DONE, DONE_VALUE)
    return s

def scenario(t):
    s = "; %s: what the simulator must show (generated; the values come from the spec, see make_conformance.py)\n" % t.name
    s += "run until csr[0x%03X] == 0x%02X max 400000000\n" % (DONE, DONE_VALUE)
    s += "expect csr[0x%03X] == 0x%02X :: the test ran to its end\n" % (DONE, DONE_VALUE)
    for off, v, what, _ in sorted(t.expect):
        s += "expect csr[0x%03X] == 0x%02X :: RES+%d %s\n" % (RES + off, v, off, what.replace("::", ":"))
    for off, what in sorted(t.untouched):
        s += "expect csr[0x%03X] == 0 :: RES+%d must not be written\n" % (RES + off, off)
    s += "expect main.faulted == 0 && peri.faulted == 0 :: no fault\n"
    return s

def write(path, text, newline="\n"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w", encoding="utf-8", newline=newline).write(text)

failures = 0
report = []
if not check_only:
    write(os.path.join(here, "conf_defs.inc"), DEFS)
    if not os.path.exists(os.path.join(here, "conf_board.inc")): write(os.path.join(here, "conf_board.inc"), BOARD)     # (the user's own values are kept)
    write(os.path.join(here, "conf_board_env.cfg"), BOARD_ENV)
for t in tests:
    src_path = os.path.join(here, t.name + ".asm")
    if check_only:
        on_disk = open(src_path, encoding="utf-8").read() if os.path.exists(src_path) else ""
        if on_disk != source(t):
            print("  FAIL  %s.asm is not what the generator writes (run make_conformance.py)" % t.name); failures += 1
    else:
        write(src_path, source(t))
        write(os.path.join(here, t.name + ".expected.csv"), "\ufeff" + expected_csv(t))
    d = os.path.join(out_dir, t.name)
    os.makedirs(d, exist_ok=True)
    prefix = os.path.join(d, t.name)
    rc, out = run([exe("asmc"), arg(src_path), "-I", arg(here), "-o", arg(prefix), "--list", "--map", "--hex", "--artifacts", "--eeprom",
                    "--no-warn", "W402", "--no-warn", "W504",      # (the skipped write of c01 and the unread results of c07 are meant;
                    "--no-warn", "W505", "--no-warn", "W510"])     #  b03 takes the slot on OVERRUN and looks at no service id, on purpose)
    warn = "warning" in out
    if rc != 0:
        print("  FAIL  %s does not assemble\n%s" % (t.name, out)); failures += 1; continue
    scn = os.path.join(d, t.name + ".scn")
    write(scn, scenario(t))
    envtext = (BOARD_ENV if t.board else "") + t.env
    envp = os.path.join(d, t.name + "_env.cfg")
    if envtext: write(envp, envtext)
    ok = True
    # layer 1 is about values: one run, fetching takes no time. Layer 2 is about orders, which must hold however long fetching takes: it is run
    # both ways (no fetch time, and every instruction read from the I2C EEPROM), and the orders and the bus transactions are read from the event log.
    for mode in (["instant"] if t.layer == 1 else ["instant", "sequential"]):
        ev = os.path.join(d, t.name + ".events.csv")
        cmd = [exe("asmsim"), arg(src_path), "-I", arg(here), "--scenario", arg(scn), "--param", "fetch.read_mode=" + mode, "--events", arg(ev)]
        if envtext: cmd += ["--env", arg(envp)]
        rc2, out2 = run(cmd)
        if not (rc2 == 0 and "scenario: PASS" in out2):
            ok = False
            print("  FAIL  %s (fetch %s): the simulator does not give what the spec says\n%s" % (t.name, mode, "\n".join(l for l in out2.splitlines() if "FAIL" in l or "fault" in l.lower() or "scenario" in l)))
            continue
        if t.before or t.bus_check or t.untouched:
            for b_ in check_behaviour(t, *read_events(ev)):
                ok = False
                print("  FAIL  %s (fetch %s): %s" % (t.name, mode, b_))
    if os.path.exists(os.path.join(d, t.name + ".events.csv")): os.remove(os.path.join(d, t.name + ".events.csv"))
    if not ok: failures += 1
    else:
        print("  PASS  %-22s %2d values%s, the simulator agrees%s" % (t.name, len(t.expect) + len(t.untouched),
              ", %d order(s), %d bus transaction(s)" % (len(t.before), len(t.bus_check)) if t.layer == 2 else "", "  (assembler warnings)" if warn else ""))
    write(os.path.join(d, t.name + ".expected.csv"), "\ufeff" + expected_csv(t))
    report.append((t, ok))

# ---- do the tests notice a wrong encoding? Each burned image is run by a decoder whose profile differs in ONE assumption: the test made for that
# assumption must fail. (shift_dir, D6, has no profile key and cannot be varied this way.)
MUTATIONS = [
    ("c03_alu_arith", "alu_arith_sub = 2\nalu_arith_mul = 1\n", "the codes of SUB and MUL swapped"),
    ("c03_alu_arith", "alu_arith_add = 3\nalu_arith_inc = 0\n", "the codes of ADD and INC swapped"),
    ("c04_alu_cmp", "alu_cmp_lt = 3\nalu_cmp_ge = 2\n", "the codes of LT and GE swapped"),
    ("c04_alu_cmp", "alu_cmp_eq = 1\nalu_cmp_ne = 0\n", "the codes of EQ and NE swapped"),
    ("c05_alu_logic", "alu_logic_and = 2\nalu_logic_xor = 0\n", "the codes of AND and XOR swapped"),
    ("c05_alu_logic", "alu_logic_or = 3\nalu_logic_not = 1\n", "the codes of OR and NOT swapped"),
    ("c01_csr_branch", "big_endian = true\n", "descriptors read high byte first"),
    ("c02_mov_const", "big_endian = true\n", "descriptors read high byte first"),
    ("c10_peri_xfer_pub", "device_map_addr_shift = 1\ndevice_map_bus_shift = 0\n", "the DeviceMap entry packed differently"),
]
for name, profile, what in MUTATIONS:
    d = os.path.join(out_dir, name)
    if not os.path.exists(os.path.join(d, name + ".eeprom.bin")): continue
    prof = os.path.join(d, "mutated.profile")
    write(prof, profile)
    cmd = [exe("asmsim"), "--eeprom", arg(os.path.join(d, name + ".eeprom.bin")), "--profile", arg(prof), "--scenario", arg(os.path.join(d, name + ".scn")),
           "--param", "fetch.read_mode=instant"]
    if os.path.exists(os.path.join(d, name + "_env.cfg")): cmd += ["--env", arg(os.path.join(d, name + "_env.cfg"))]
    rc3, out3 = run(cmd)
    os.remove(prof)
    noticed = "scenario: PASS" not in out3
    if noticed: print("  PASS  %-22s notices: %s" % (name, what))
    else: print("  FAIL  %-22s does NOT notice: %s" % (name, what)); failures += 1

# ---- does b01 notice ONE shared timer (Q1)? The simulator has two timers; an environment in which Main's timer CSRs drive the Peri timer and
# MAIN_TIMER_DONE reads its done flag is one shared timer: the order b01 asks for must then be broken.
SHARED_TIMER_ENV = BOARD_ENV.replace("bind = main_timer_period", "bind = timer_period").replace("bit0 = main_timer_start", "bit0 = timer_start") \
    .replace("[csr MAIN_TPER]", "[csr MAIN_TPER_SHARED_DUMMY]\naddr = 0x3F0\nkind = ram\n[csr MAIN_TPER]") + "[condition main 7]\nexpr = timer.done\nname = MAIN_TIMER_DONE\n"
for t in tests:
    if t.name != "b01_timers": continue
    d = os.path.join(out_dir, t.name)
    envp, ev = os.path.join(d, "shared_timer_env.cfg"), os.path.join(d, "shared.events.csv")
    write(envp, SHARED_TIMER_ENV.replace("[csr PERI_TPER]\nkind = value\nbind = timer_period\nbytes = 2\n", "[csr PERI_TPER]\nkind = ram\n"))
    rc4, out4 = run([exe("asmsim"), arg(os.path.join(here, t.name + ".asm")), "-I", arg(here), "--env", arg(envp), "--param", "fetch.read_mode=instant",
                     "--cycles", "2000000", "--events", arg(ev)])
    bad = check_behaviour(t, *read_events(ev)) if os.path.exists(ev) else ["no event log: " + out4[-200:]]
    for f_ in (envp, ev):
        if os.path.exists(f_): os.remove(f_)
    if bad: print("  PASS  %-22s notices: one timer shared by Main and Peri (%s)" % (t.name, bad[0]))
    else: print("  FAIL  %-22s does NOT notice one shared timer" % t.name); failures += 1

print(("OK" if not failures else "FAILED") + "      %d failure(s); images and tables in %s" % (failures, out_dir))
if check_only or failures:
    sys.exit(1 if failures else 0)

# ---------------------------------------------------------------------------------------------------------------------------------
# EXPECTED.md: everything on one page, for whoever runs the RTL simulation
md = ["# 一致性測試：預期結果（第 1 層編碼、第 2 層行為）", "",
      "由 `make_conformance.py` 產生（請改產生器，不要直接改這份）。結果 CSR 的基底：`RES = 0x%03X`；每支測試最後寫入 `RES_DONE = 0x%03X`，值 `0x%02X`。" % (RES, DONE, DONE_VALUE), "",
      "第 1 層的預期值依規格 v31 的文字獨立算出（不是抄模擬器的輸出）；第 2 層的預期行為依規格的敘述逐支寫定。本專案的模擬器已跑過全部程式，結果相同。", "",
      "「取決於」欄有內容的列，代表那個預期值建立在某個前提或假設上；與硬體不同時先看那一欄。", ""]
shown_layer = 0
for t in tests:
    if t.layer != shown_layer:
        shown_layer = t.layer
        if t.layer == 1:
            md += ["---", "", "# 第 1 層：編碼", "", "硬體解出來的指令，是不是工具編進去的那一道？只看最後的值。", ""]
        else:
            md += ["---", "", "# 第 2 層：行為", "",
                   "同一支程式，兩邊做的事、做的**順序**是否相同？除了最後的值，還要看「哪個位址先被寫入」與 I2C 匯流排上的交易。只比順序，不比時間。", "",
                   "標示「用到板子自己的 CSR」的測試（`b01`、`b02`、`b05`）會寫計時器、poll、done／error 這些暫存器；規格沒有給它們的位址與位元，",
                   "**請先把 `conf_board.inc` 改成實際的數值再重新產生**（目前是範例值）。", ""]
    md += ["## %s" % t.name, "", t.zh_title, "", "**檢查的對象**：%s" % t.zh_checks, "",
           "| 位址 | 位移 | 預期值 | 意義 | 取決於 |", "|---|---|---|---|---|"]
    for off, v, what, unc in sorted(t.expect):
        md.append("| `0x%03X` | RES+%d | `0x%02X` | %s | %s |" % (RES + off, off, v, what.replace("|", "\\|"), unc.replace("|", "\\|")))
    for off, what in sorted(t.untouched):
        md.append("| `0x%03X` | RES+%d | **不可被寫入** | %s | |" % (RES + off, off, what))
    md.append("| `0x%03X` | RES_DONE | `0x%02X` | 測試的最後一次寫入 | |" % (DONE, DONE_VALUE))
    if t.before:
        md += ["", "**寫入的先後順序（必須成立）**", "", "| 先 | 後 | 原因 |", "|---|---|---|"]
        nm = lambda o: "RES_DONE" if o == 0x7F else "RES+%d" % o
        for a_, b_, why in t.before: md.append("| `%s` | `%s` | %s |" % (nm(a_), nm(b_), why))
    if t.bus:
        md += ["", "**I2C 匯流排上應該看到的交易（依序）**", "", "| 方向 | 裝置位址 | 暫存器 | 資料 |", "|---|---|---|---|"]
        for d_, a_, r_, x_ in t.bus: md.append("| %s | `0x%02X` | %s | %s |" % (d_, a_, r_, x_))
    if t.notes:
        md += ["", "**結果不同時怎麼判讀**", ""] + ["- " + n_ for n_ in t.notes]
    md.append("")
write(os.path.join(here, "EXPECTED.md"), "\n".join(md) + "\n")
