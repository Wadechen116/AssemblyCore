#!/usr/bin/env python3
"""The figures of HW_TEAM_GUIDE.md, as SVG files (text: they can be compared and reviewed like the rest).

    python3 make_figures.py            write fig*.svg next to this script
    python3 make_figures.py --check    fail when a file differs from what this script writes, or a text is wider than its box

Only what Qt's SVG reader draws is used (SVG Tiny: rectangles, lines, polygons, text; no markers, no CSS), because md2pdf turns them into the PDF.
All figures are 1000 units wide.
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = "WenQuanYi Zen Hei"                     # md2pdf runs where this font is; another viewer falls back to its own CJK font
INK, MUTED = "#1f2937", "#6b7280"
# (fill, stroke): one colour per kind of thing, the same in every figure
SW   = ("#e8f0fb", "#3b6fb5")                  # the software side: tools and what they write
HW   = ("#e7f4ea", "#3d8b56")                  # the hardware side: the design under test
OBS  = ("#fff1dc", "#c0771a")                  # what the test bench observes / records
FILE = ("#f4f5f7", "#8a93a0")                  # files and neutral things
BAD  = ("#fde9e7", "#c0392b")                  # a fault, a deliberately wrong byte
NOTE = ("#fffbe6", "#b59b2a")                  # remarks
SCALE = 1.16                                   # every text this much larger than drawn: the figures are printed as wide as the page
problems = []

def esc(s): return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
def width_of(s, size):
    return sum(1.0 if ord(c) > 0x2E7F else (0.34 if c in " .,:;'|il" else 0.60) for c in s) * size

class Fig:
    def __init__(self, name, h, title=None):
        self.name, self.h, self.out = name, h, []
        if title: self.text(500, 26, title, 19, weight="bold")
    def rect(self, x, y, w, h, style=FILE, rx=6, sw=1.6, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{style[0]}" stroke="{style[1]}" stroke-width="{sw}"{d}/>')
    def text(self, x, y, s, size=15, anchor="middle", weight="normal", fill=INK, mono=False, within=None):
        fam = "DejaVu Sans Mono" if mono else FONT
        size = round(size * SCALE, 1)
        if within is not None and width_of(s, size) > within: problems.append(f"{self.name}: '{s}' is about {width_of(s, size):.0f} wide, the box allows {within}")
        self.out.append(f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}" fill="{fill}">{esc(s)}</text>')
    def box(self, x, y, w, h, lines, style=FILE, size=15, bold_first=True, rx=6, dash=None, mono=False):
        """A rectangle with centred lines of text; the first line is the name of the thing."""
        self.rect(x, y, w, h, style, rx, dash=dash)
        if isinstance(lines, str): lines = [lines]
        lh = size * SCALE * 1.32
        y0 = y + h / 2 - lh * (len(lines) - 1) / 2 + size * SCALE * 0.36
        for i, s in enumerate(lines):
            first = i == 0 and bold_first
            self.text(x + w / 2, y0 + i * lh, s, size if first or len(lines) == 1 else size - 1.5, weight="bold" if first else "normal",
                      fill=INK if first or len(lines) == 1 else "#374151", within=w - 8, mono=mono and not first)
    def line(self, x1, y1, x2, y2, color=MUTED, sw=1.8, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.out.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{sw}"{d}/>')
    def arrow(self, pts, color=MUTED, sw=1.8, dash=None, label=None, lpos=None, lsize=13, both=False):
        """A line through the points with an arrow head at the end (and at the start with both)."""
        for a, b in zip(pts, pts[1:]): self.line(a[0], a[1], b[0], b[1], color, sw, dash)
        def head(p, q):
            dx, dy = q[0] - p[0], q[1] - p[1]
            n = (dx * dx + dy * dy) ** 0.5 or 1
            ux, uy = dx / n, dy / n
            L, W = 11, 5
            bx, by = q[0] - ux * L, q[1] - uy * L
            self.out.append(f'<polygon points="{q[0]:.1f},{q[1]:.1f} {bx - uy * W:.1f},{by + ux * W:.1f} {bx + uy * W:.1f},{by - ux * W:.1f}" fill="{color}"/>')
        head(pts[-2], pts[-1])
        if both: head(pts[1], pts[0])
        if label:
            lx, ly = lpos if lpos else ((pts[0][0] + pts[1][0]) / 2, (pts[0][1] + pts[1][1]) / 2 - 6)
            self.text(lx, ly, label, lsize, fill=color if color != MUTED else "#4b5563")
    def tag(self, x, y, n, style=OBS):
        """A numbered round mark."""
        self.out.append(f'<circle cx="{x}" cy="{y}" r="13" fill="{style[1]}"/>')
        self.text(x, y + 5.5, str(n), 15, weight="bold", fill="#ffffff")
    def dot(self, x, y, r=7, color="#c0771a"):
        self.out.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{color}"/>')
    def svg(self):
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="{self.h}" viewBox="0 0 1000 {self.h}">\n'
                f'<rect x="0" y="0" width="1000" height="{self.h}" fill="#ffffff"/>\n' + "\n".join(self.out) + "\n</svg>\n")

def legend(f, y, items):
    x = 40
    for style, s in items:
        f.rect(x, y, 22, 15, style, 3)
        f.text(x + 30, y + 13, s, 13, anchor="start", fill="#4b5563")
        x += 44 + width_of(s, 13)

# ------------------------------------------------------------------------------------------------------------------------------------------------
def fig01():
    f = Fig("fig01_overview", 500, "圖 1　整件事的全貌：誰做什麼、資料怎麼流動")
    f.rect(20, 48, 440, 330, ("#f7faff", "#b7cbe8"), 10, 1.2)
    f.text(240, 72, "軟體這一側（我們）", 16, weight="bold", fill=SW[1])
    f.rect(540, 48, 440, 330, ("#f5fbf6", "#b5d8bf"), 10, 1.2)
    f.text(760, 72, "硬體這一側（你們）", 16, weight="bold", fill=HW[1])
    f.box(40, 90, 180, 62, ["測試原始碼", "*.asm"], FILE)
    f.box(260, 90, 180, 62, ["組譯器", "asmc.exe"], SW)
    f.arrow([(220, 121), (260, 121)])
    f.box(260, 190, 180, 70, ["映像＋區段位置", "out\\<測試名>\\"], FILE)
    f.arrow([(350, 152), (350, 190)])
    f.box(40, 190, 180, 70, ["模擬器（模型）", "asmsim.exe"], SW)
    f.arrow([(260, 225), (220, 225)])
    f.box(40, 296, 400, 64, ["預期結果", "EXPECTED.md　TIMING.md　FAULTS.md"], FILE)
    f.arrow([(130, 260), (130, 296)])
    f.box(560, 190, 180, 70, ["載入", "RTL 模擬或 FPGA"], HW)
    f.arrow([(440, 225), (560, 225)], color=SW[1], sw=2.6, label="① 交付", lpos=(500, 214))
    f.box(780, 190, 180, 70, ["執行", "到完成標記為止"], HW)
    f.arrow([(740, 225), (780, 225)])
    f.box(560, 296, 400, 64, ["② 記錄", "CSR 寫入　I2C 交易　週期數　錯誤狀態"], OBS)
    f.arrow([(870, 260), (870, 296)])
    f.box(300, 410, 400, 62, ["③ 比對：實際看到的 與 預期結果", "不同之處就是我們要的資訊"], NOTE)
    f.arrow([(240, 360), (240, 441), (300, 441)])
    f.arrow([(760, 360), (760, 441), (700, 441)])
    f.arrow([(500, 410), (500, 388), (22, 388), (22, 121), (40, 121)], color=BAD[1], dash="7,4")
    f.text(128, 412, "④ 依回報修正工具", 13.5, fill=BAD[1])
    f.text(128, 431, "（編碼、參數、模型）", 13, fill=BAD[1])
    return f

def fig02():
    f = Fig("fig02_architecture", 560, "圖 2　我們依規格書理解的硬體架構（若與 RTL 不同，請直接指正）")
    f.box(30, 60, 150, 64, ["外部 ROM", "（EEPROM？）"], FILE, dash="6,4")
    f.box(230, 60, 150, 64, ["Boot 模組", "開機時載入一次"], HW)
    f.arrow([(180, 92), (230, 92)])
    f.rect(440, 50, 330, 84, HW)
    f.text(605, 72, "共用程式 SRAM（32 位元的字）", 15, weight="bold")
    f.box(452, 82, 148, 42, "Main 映像", ("#ffffff", HW[1]), 14)
    f.box(610, 82, 148, 42, "Peri 映像", ("#ffffff", HW[1]), 14)
    f.arrow([(380, 92), (440, 92)])
    f.box(810, 50, 170, 84, ["區段位置", "layout 暫存器", "Main 6 個・Peri 6 個"], HW, 14)
    f.box(170, 190, 260, 92, ["Main sequencer", "自己的 PC、ALU、暫存區", "自己的計時器"], HW)
    f.box(570, 190, 260, 92, ["Peri sequencer", "自己的 PC", "自己的計時器、Poll Unit"], HW)
    f.arrow([(526, 134), (300, 190)], label="取指", lpos=(392, 152))
    f.arrow([(684, 134), (700, 190)], label="取指", lpos=(716, 156))
    f.arrow([(895, 134), (895, 176), (760, 176), (760, 190)], dash="5,4")
    f.arrow([(895, 176), (360, 176), (360, 190)], dash="5,4")
    f.box(440, 212, 120, 48, ["結果槽", "Peri → Main"], FILE, 13.5)
    f.arrow([(570, 236), (560, 236)])
    f.arrow([(440, 236), (430, 236)])
    f.rect(60, 340, 720, 44, OBS)
    f.text(420, 368, "CSR 匯流排（Main 與 Peri 經仲裁後共用）", 15.5, weight="bold")
    f.arrow([(300, 282), (300, 340)])
    f.arrow([(640, 282), (640, 340)])
    f.box(60, 440, 220, 84, ["結果 CSR", "0x600 – 0x67F", "測試把答案寫在這裡"], OBS, 14.5)
    f.box(310, 440, 220, 84, ["板子自己的 CSR", "計時器、poll、done／error", "conf_board.inc"], HW, 14.5)
    f.box(560, 440, 220, 84, ["其他 CSR", "sensor、影像時序…", "（測試不使用）"], FILE, 14.5, dash="6,4")
    for x in (170, 420, 670): f.arrow([(x, 384), (x, 440)])
    f.box(850, 196, 130, 80, ["I2C 主控", "由 Peri 操作"], HW, 14.5)
    f.arrow([(830, 236), (850, 236)])
    f.box(850, 440, 130, 84, ["I2C 裝置", "0x2A – 0x2F", "測試平台提供"], OBS, 14.5)
    f.arrow([(915, 276), (915, 440)], both=True)
    f.tag(46, 362, 1); f.tag(915, 358, 2); f.tag(152, 190, 3); f.tag(552, 190, 3)
    f.text(30, 548, "觀察點：① 記錄每一次 CSR 寫入（位址、值、週期）　② 記錄 I2C 交易　③ 兩個 sequencer 的錯誤與停止狀態", 13.5, anchor="start", fill="#4b5563")
    return f

def fig03():
    f = Fig("fig03_image", 540, "圖 3　一份映像的內容，以第一支測試 c01 的 Main 映像為例（共 40 個位元組）")
    f.text(40, 58, "邏輯映像（*.main.bin）：一串位元組，位移從 0 起算", 14.5, anchor="start", weight="bold")
    x0, sc, y = 60, 22, 96                     # 40 bytes x 22 = 880
    segs = [(0, 18, SW, "程式碼", "18 個位元組・10 道指令"), (18, 2, FILE, "", ""), (20, 4, OBS, "分支描述子", "2 筆 × 2"), (24, 16, HW, "CSR 單位元組描述子", "8 筆 × 2 個位元組")]
    for a, n, st, t1, t2 in segs:
        f.rect(x0 + a * sc, y, n * sc, 66, st, 3)
        if t1:
            f.text(x0 + (a + n / 2) * sc, y + 28, t1, 14.5 if n > 4 else 12, weight="bold", within=n * sc + (30 if n <= 4 else 0))
            f.text(x0 + (a + n / 2) * sc, y + 50, t2, 12.5 if n > 4 else 11.5, fill="#374151")
    for off in (0, 20, 24, 40):
        f.line(x0 + off * sc, y - 10, x0 + off * sc, y)
        f.text(x0 + off * sc, y - 14, f"0x{off:02X}", 12.5, mono=True)
    f.text(x0 + 19 * sc, y + 38, "補齊", 10.5, fill=MUTED)
    f.text(40, 222, "區段位置", 14.5, anchor="start", weight="bold")
    lay = [("move_desc", 20), ("branch_desc", 20), ("csr_onebyte_desc", 24), ("csr_multibyte_desc", 40), ("alu_desc", 40), ("const_pool", 40)]
    for i, (n, v) in enumerate(lay):
        bx = 40 + i * 156
        f.box(bx, 236, 148, 48, [n, f"= {v}（0x{v:02X}）"], NOTE, 11.5)
    f.arrow([(40 + 156 + 74, 236), (x0 + 20 * sc, 154)], color=NOTE[1], dash="4,3")
    f.arrow([(40 + 312 + 74, 236), (x0 + 24 * sc, 154)], color=NOTE[1], dash="4,3")
    f.text(500, 304, "區段位置在 *.main.layout.json，不在映像裡：要另外送進硬體的 layout 暫存器，硬體靠它們找到各張表", 13, fill="#374151")
    f.text(500, 323, "沒有內容的表（這支程式沒用到 MOV、多位元組 CSR、ALU、常數）位移仍然要給：指到下一張表的開頭或映像的結尾", 12.5, fill="#4b5563")
    f.text(40, 370, "放進 SRAM（*.sram.hex）：每 4 個位元組合成一個 32 位元的字，位移較小的位元組放在低位", 14.5, anchor="start", weight="bold")
    by = 392
    bytes_ = ["00", "5A", "01", "F0"]
    for i, b in enumerate(bytes_):
        f.box(60 + i * 84, by, 76, 44, [b], SW, 17, mono=True)
        f.text(60 + i * 84 + 38, by + 62, f"位移 {i}", 12.5, fill="#4b5563")
    f.text(60 + 168, by + 84, "映像的前 4 個位元組", 13, fill="#4b5563")
    f.arrow([(404, by + 22), (500, by + 22)], label="組成", lpos=(452, by + 14))
    lanes = [("F0", "bit 31:24", "位移 3"), ("01", "bit 23:16", "位移 2"), ("5A", "bit 15:8", "位移 1"), ("00", "bit 7:0", "位移 0")]
    for i, (b, bits, off) in enumerate(lanes):
        f.box(520 + i * 108, by, 100, 44, [b], HW, 17, mono=True)
        f.text(520 + i * 108 + 50, by + 62, bits, 12.5, fill="#4b5563")
        f.text(520 + i * 108 + 50, by + 80, off, 12.5, fill="#4b5563")
    f.text(736, by + 106, "SRAM 字 0 ＝ 0xF0015A00", 15, weight="bold", mono=True)
    return f

def fig04():
    f = Fig("fig04_decode", 430, "圖 4　一道指令怎麼解：c01 的第一道指令（位元組 00 5A）")
    f.text(40, 70, "① 指令位元組", 15, anchor="start", weight="bold")
    f.box(60, 86, 120, 54, ["0x00"], SW, 20, mono=True)
    f.arrow([(180, 113), (250, 113)])
    bits = "00000000"
    for i, c in enumerate(bits):
        st = OBS if i < 2 else HW
        f.box(250 + i * 44, 86, 40, 54, [c], st, 19, mono=True)
    f.text(250 + 42, 162, "bit 7:6", 13, fill=OBS[1]); f.text(250 + 42, 180, "opcode = 00", 13.5, weight="bold", fill=OBS[1])
    f.text(250 + 88 + 130, 162, "bit 5:0", 13, fill=HW[1]); f.text(250 + 88 + 130, 180, "arg = 0（描述子的索引）", 13.5, weight="bold", fill=HW[1])
    f.box(660, 78, 300, 70, ["Main 的 opcode", "00 EXEC_CSR　01 分支", "10 MOV　　　11 ALU"], FILE, 14)
    f.text(40, 232, "② 用 arg 查描述子表", 15, anchor="start", weight="bold")
    f.box(60, 248, 250, 58, ["CSR 單位元組描述子[0]", "位於 layout 的位移 24"], HW, 14)
    f.arrow([(310, 277), (370, 277)])
    f.box(370, 248, 130, 58, ["0x0600"], HW, 20, mono=True)
    f.arrow([(500, 277), (560, 277)])
    f.box(560, 240, 400, 74, ["bit 15:14 = 00 → WRITE8（直接寫入）", "bit 11:0 = 0x600 → CSR 位址"], NOTE, 14, bold_first=False)
    f.text(40, 362, "③ 指令的下一個位元組是立即值", 15, anchor="start", weight="bold")
    f.box(60, 376, 120, 44, ["0x5A"], SW, 19, mono=True)
    f.arrow([(180, 398), (370, 398)], label="要寫入的值", lpos=(275, 390))
    f.box(370, 372, 590, 52, ["結果：CSR 匯流排上出現一次寫入　位址 0x600　值 0x5A"], OBS, 15.5)
    return f

def fig05():
    f = Fig("fig05_c01_timeline", 330, "圖 5　c01 執行時 CSR 匯流排上應該看到的寫入（由左到右是時間）")
    f.line(40, 150, 960, 150, INK, 2.2)
    f.arrow([(940, 150), (968, 150)], color=INK, sw=2.2)
    f.text(968, 176, "時間", 13, fill=MUTED)
    ev = [(90, "0x600", "0x5A", "RES+0", OBS), (200, "0x601", "0xF0", "RES+1", OBS), (310, "0x601", "0xFF", "RES+1", OBS), (420, "0x602", "0xFF", "RES+2", OBS),
          (530, "0x602", "0xC3", "RES+2", OBS), (750, "0x604", "0x0D", "RES+4", OBS), (880, "0x67F", "0xA5", "完成標記", HW)]
    for x, a, v, n, st in ev:
        f.line(x, 104, x, 150, st[1], 2)
        f.dot(x, 150, 7, st[1])
        f.box(x - 48, 56, 96, 48, [a, v], st, 14, mono=True)
        f.text(x, 174, n, 13, fill="#374151")
    f.box(592, 56, 96, 48, ["0x603", "不可出現"], BAD, 13.5, dash="5,4")
    f.line(640, 104, 640, 150, BAD[1], 1.6, "5,4")
    f.text(640, 174, "RES+3", 13, fill=BAD[1])
    f.text(255, 200, "先寫 0xF0，再 SET8 0x0F", 12.5, fill="#4b5563")
    f.text(255, 217, "（讀出、OR、寫回）", 12.5, fill="#4b5563")
    f.text(475, 200, "先寫 0xFF，再 CLR8 0x3C", 12.5, fill="#4b5563")
    f.text(475, 217, "（讀出、AND NOT、寫回）", 12.5, fill="#4b5563")
    f.text(640, 200, "被 JMP 跳過", 12.5, fill=BAD[1])
    f.text(880, 200, "看到它就可以", 12.5, fill="#4b5563"); f.text(880, 217, "停止記錄", 12.5, fill="#4b5563")
    f.box(60, 250, 880, 58, ["判定方式", "每個位址取「最後一次」被寫入的值，和預期值比；標示不可出現的位址，整段紀錄裡不能有它"], NOTE, 14)
    return f

def fig06():
    f = Fig("fig06_testbench", 500, "圖 6　測試平台需要的東西")
    f.box(40, 70, 190, 70, ["映像載入", "SRAM 初始值 或 Boot"], OBS, 14.5)
    f.box(40, 170, 190, 70, ["區段位置載入", "12 個 layout 暫存器"], OBS, 14.5)
    f.box(40, 270, 190, 70, ["重置與啟動", "兩個 sequencer 從 PC 0"], OBS, 14.5)
    f.rect(310, 60, 380, 290, HW, 10, 2.2)
    f.text(500, 90, "受測設計（DUT）", 17, weight="bold")
    f.box(330, 110, 160, 56, ["程式 SRAM"], ("#ffffff", HW[1]), 14.5)
    f.box(510, 110, 160, 56, ["layout 暫存器"], ("#ffffff", HW[1]), 14.5)
    f.box(330, 190, 160, 60, ["Main", "sequencer"], ("#ffffff", HW[1]), 14.5)
    f.box(510, 190, 160, 60, ["Peri", "sequencer"], ("#ffffff", HW[1]), 14.5)
    f.box(330, 276, 160, 56, ["CSR 匯流排"], ("#ffffff", HW[1]), 14.5)
    f.box(510, 276, 160, 56, ["I2C 主控"], ("#ffffff", HW[1]), 14.5)
    f.arrow([(230, 105), (330, 132)]); f.arrow([(230, 205), (270, 205), (270, 178), (590, 178), (590, 166)]); f.arrow([(230, 305), (296, 305), (296, 220), (330, 220)])
    f.box(770, 70, 200, 84, ["I2C 裝置模型", "位址 0x2A – 0x2F", "各有指定的回應"], OBS, 14.5)
    f.box(770, 184, 200, 84, ["錯誤與停止狀態", "Main 與 Peri 各自的", "只有第 4 層需要"], OBS, 14.5)
    f.arrow([(670, 304), (730, 304), (730, 112), (770, 112)], both=True)
    f.arrow([(670, 220), (770, 225)])
    f.box(250, 400, 300, 76, ["CSR 寫入紀錄", "位址、值、第幾個 ISA 週期", "所有測試都靠它"], OBS, 14.5)
    f.box(590, 400, 300, 76, ["I2C 交易紀錄", "裝置位址、讀或寫、資料、ACK", "第 2 層需要"], OBS, 14.5)
    f.arrow([(410, 332), (410, 400)])
    f.arrow([(730, 304), (730, 400)])
    f.text(40, 430, "綠色：你們的設計", 13, anchor="start", fill=HW[1])
    f.text(40, 452, "橘色：測試平台", 13, anchor="start", fill=OBS[1])
    return f

def fig07():
    f = Fig("fig07_order", 600, "圖 7　建議的進行順序")
    f.box(60, 56, 300, 60, ["準備", "載入方式、CSR 寫入紀錄"], OBS)
    f.arrow([(210, 116), (210, 146)])
    f.box(60, 146, 300, 60, ["第 1 層　c01", "只用到 CSR 寫入與跳躍"], SW)
    f.arrow([(360, 176), (470, 176)], color=BAD[1], label="不符", lpos=(415, 168))
    f.box(470, 140, 480, 72, ["先停下來，把紀錄給我們", "多半是載入、位元組序或區段位置的問題", "後面的測試都以它為前提"], BAD, 14.5)
    f.arrow([(210, 206), (210, 240)], label="相符", lpos=(240, 228))
    f.box(60, 240, 300, 60, ["第 1 層　c02 – c09", "MOV、ALU、cascade、CSR 讀取"], SW)
    f.arrow([(210, 300), (210, 334)])
    f.box(60, 334, 300, 60, ["第 3 層　t01_alu", "量一道 ALU 的週期數"], OBS)
    f.arrow([(360, 364), (470, 364)], label="同時回答", lpos=(415, 356))
    f.box(470, 328, 480, 72, ["取指方式：約 5 個週期 → 從 SRAM", "約 6,081 個週期 → 每道指令經 I2C 讀 EEPROM", "這決定之後所有時間怎麼看"], NOTE, 14.5)
    f.arrow([(210, 394), (210, 432)])
    f.line(110, 432, 890, 432, MUTED, 1.8)
    cols = [(40, ["不需要額外準備", "t02 – t04、t05、t08", "第 4 層 X 以外的案例"], HW), (280, ["需要 I2C 裝置模型", "c10、b03、b04、t06", "第 4 層 X 開頭的案例"], HW),
            (520, ["先填 conf_board.inc", "b01、t07"], HW), (760, ["兩者都需要", "b02、b05"], HW)]
    for x, lines, st in cols:
        f.arrow([(x + 100, 432), (x + 100, 462)])
        f.box(x, 462, 200, 84, lines, st, 14)
    f.text(500, 576, "這四組彼此獨立，哪一組的條件先具備就先做；每完成一支都可以回報，不必等全部做完", 14, fill="#4b5563")
    return f

def fig08():
    f = Fig("fig08_timers", 560, "圖 8　b01：用三次寫入的先後，判斷計時器是兩顆還是一顆")
    def lane(y, name, segs, marks):
        f.text(34, y + 26, name, 14.5, anchor="start", weight="bold")
        f.line(110, y + 40, 800, y + 40, "#c9ced6", 1.2)
        for x, w, s, st in segs:
            f.box(x, y, w, 40, [s], st, 12.5, bold_first=False)
        for x, s, st in marks:
            f.line(x, y - 8, x, y + 54, st[1], 2.2)
            f.dot(x, y + 40, 6, st[1])
            f.text(x, y + 72, s, 13.5, weight="bold", fill=st[1])
    main = [(110, 70, "短", SW), (184, 70, "短", SW), (258, 150, "長", SW), (412, 150, "長", SW), (566, 150, "長", SW)]
    f.rect(20, 46, 960, 240, ("#f6fbf7", "#b5d8bf"), 8, 1.2)
    f.text(40, 72, "兩顆獨立的計時器（規格的定義）", 15, anchor="start", weight="bold", fill=HW[1])
    lane(96, "Main", main, [(182, "RES+0", SW), (256, "RES+1", SW), (718, "完成標記", HW)])
    lane(196, "Peri", [(110, 232, "長（65000 tick）", OBS), (346, 232, "長（65000 tick）", OBS)], [(580, "RES+8", OBS)])
    f.box(820, 110, 146, 130, ["先後順序", "RES+0", "→ RES+1", "→ RES+8", "→ 完成標記"], ("#ffffff", HW[1]), 14)
    f.rect(20, 304, 960, 240, ("#fef6f5", "#ecc0bb"), 8, 1.2)
    f.text(40, 330, "只有一顆共用的計時器（和規格不同）", 15, anchor="start", weight="bold", fill=BAD[1])
    lane(354, "Main", main[:3], [(182, "RES+0", SW), (256, "RES+1", SW)])
    lane(454, "Peri", [(110, 70, "被蓋掉", BAD)], [(184, "RES+8 提前出現", BAD)])
    f.text(420, 372, "（之後和上面相同）", 12.5, anchor="start", fill=MUTED)
    f.text(330, 470, "Main 啟動計時器時，把 Peri 正在等的那一次蓋掉了，", 13.5, anchor="start", fill=BAD[1])
    f.text(330, 490, "Peri 跟著 Main 的短計時一起「完成」。", 13.5, anchor="start", fill=BAD[1])
    f.box(820, 368, 146, 130, ["判定", "RES+8 不晚於", "RES+1", "→ 只有一顆"], ("#ffffff", BAD[1]), 14)
    return f

def fig09():
    f = Fig("fig09_timing", 440, "圖 9　第 3 層的量測方法：兩段相減，消掉標記本身的時間")
    y = 110
    f.line(40, y + 60, 960, y + 60, INK, 2.2); f.arrow([(940, y + 60), (968, y + 60)], color=INK, sw=2.2)
    xs = [80, 310, 880]
    for i, x in enumerate(xs):
        f.line(x, y - 30, x, y + 60, OBS[1], 2.4); f.dot(x, y + 60, 7.5, OBS[1])
        f.box(x - 58, y - 68, 116, 38, [f"標記 {('k', 'k+1', 'k+2')[i]}"], OBS, 14.5)
        f.text(x, y + 84, f"寫入 RES+{('k', 'k+1', 'k+2')[i]}", 12.5, fill="#4b5563")
    for i in range(4): f.box(104 + i * 46, y + 6, 42, 40, ["指令"], SW, 12.5, bold_first=False)
    for i in range(12): f.box(334 + i * 44, y + 6, 40, 40, ["指令"], SW, 12.5, bold_first=False)
    f.text(195, y - 8, "N1 = 4 道同樣的指令", 13.5, weight="bold", fill=SW[1])
    f.text(595, y - 8, "N2 = 12 道同樣的指令", 13.5, weight="bold", fill=SW[1])
    f.arrow([(80, y + 112), (310, y + 112)], color=HW[1], both=True); f.text(195, y + 134, "第一段：量到 A 個週期", 14, weight="bold", fill=HW[1])
    f.arrow([(310, y + 112), (880, y + 112)], color=HW[1], both=True); f.text(595, y + 134, "第二段：量到 B 個週期", 14, weight="bold", fill=HW[1])
    f.box(60, 290, 420, 124, ["一道指令的時間", "＝（B − A）÷（N2 − N1）", "", "每一段都含一次標記寫入的時間，", "相減之後正好消掉"], NOTE, 15)
    f.box(520, 290, 420, 124, ["例：t01_alu 從 SRAM 取指時的預測", "A = 27　B = 67", "（67 − 27）÷（12 − 4）＝ 5 個週期", "其中取指 4、執行 1"], FILE, 15)
    return f

def fig10():
    f = Fig("fig10_faults", 540, "圖 10　第 4 層：一份故意寫壞的映像怎麼做出來、執行時看什麼")
    f.box(30, 60, 190, 66, ["合法的小程式", "f_<編號>.asm（載體）"], FILE, 14)
    f.arrow([(220, 93), (280, 93)], label="組譯", lpos=(250, 84))
    f.box(280, 60, 180, 66, ["合法的映像", "f_<編號>_carrier.*"], SW, 14)
    f.arrow([(460, 93), (610, 93)], color=BAD[1], sw=2.4, label="改 1–2 個位元組", lpos=(535, 82))
    f.box(610, 60, 170, 66, ["寫壞的映像", "f_<編號>.*"], BAD, 14)
    f.arrow([(780, 93), (830, 93)])
    f.box(830, 60, 150, 66, ["在硬體上執行"], HW, 14)
    f.text(40, 172, "程式的內容（以 F4 為例：把 MOV 的描述子索引改成表格裡沒有的 63）", 14.5, anchor="start", weight="bold")
    cells = [(60, 190, OBS, ["寫入 RES+0", "= 0xF0"]), (270, 190, BAD, ["受測指令", "0x80 → 0xBF"]), (480, 190, OBS, ["寫入 RES+1", "= 0x0F"]), (690, 190, HW, ["寫入完成標記", "0x67F = 0xA5"])]
    for x, w, st, lines in cells:
        f.box(x, 190, w, 62, lines, st, 14.5)
    for x in (250, 460, 670): f.arrow([(x, 221), (x + 20, 221)])
    f.text(155, 272, "「下一道就是它」", 12.5, fill="#4b5563"); f.text(365, 272, "pc = 2", 12.5, fill=BAD[1]); f.text(575, 272, "「它之後還在執行」", 12.5, fill="#4b5563"); f.text(785, 272, "「跑到結尾了」", 12.5, fill="#4b5563")
    f.text(40, 320, "可能看到的兩種結果", 14.5, anchor="start", weight="bold")
    def row(y, name, marks, st, note1, note2):
        f.rect(40, y, 920, 86, (st[0], st[1]), 8, 1.3)
        f.text(60, y + 30, name, 14.5, anchor="start", weight="bold", fill=st[1])
        f.text(60, y + 56, note1, 13, anchor="start", fill="#374151")
        f.text(60, y + 74, note2, 13, anchor="start", fill="#374151")
        for i, (lab, on) in enumerate(marks):
            x = 560 + i * 136
            f.box(x, y + 20, 124, 46, [lab, "有" if on else "沒有"], OBS if on else FILE, 13.5, dash=None if on else "5,4")
    row(338, "甲　硬體偵測到，在受測指令停止", [("RES+0", True), ("RES+1", False), ("完成標記", False)], HW, "模擬器目前的做法。請再記錄：在哪一級發現、", "怎麼回報、之後是停住還是重置")
    row(438, "乙　硬體沒有偵測，照樣往下執行", [("RES+0", True), ("RES+1", True), ("完成標記", True)], NOTE, "這不算失敗，是我們需要知道的事實。", "請照實記錄，模擬器會改成和硬體一樣")
    return f

def fig11():
    f = Fig("fig11_layers", 356, "圖 11　四層各回答一個問題，由下往上越來越深")
    rows = [("第 4 層　錯誤偵測", "遇到不該執行的指令，硬體怎麼反應？", "48 份映像", "以硬體為準", BAD),
            ("第 3 層　時間", "每道指令花幾個週期？", "8 支程式", "沒有對錯，量了回填", OBS),
            ("第 2 層　行為", "做事的順序和模擬器一樣嗎？", "5 支程式", "有預期的順序", HW),
            ("第 1 層　編碼", "工具產生的位元組，硬體解成同一道指令嗎？", "10 支程式", "有預期的值", SW)]
    for i, (a, b, c, d, st) in enumerate(rows):
        y = 50 + i * 66
        inset = (3 - i) * 26
        f.rect(40 + inset, y, 920 - 2 * inset, 56, st, 6)
        f.text(60 + inset, y + 34, a, 15.5, anchor="start", weight="bold")
        f.text(250 + inset, y + 34, b, 14, anchor="start", fill="#374151")
        f.text(940 - inset, y + 24, c, 13.5, anchor="end", weight="bold", fill=st[1])
        f.text(940 - inset, y + 44, d, 12.5, anchor="end", fill="#4b5563")
    f.text(500, 338, "下面一層不過，上面一層的結果就無法解讀——所以從第 1 層的 c01 開始", 14, fill="#4b5563")
    return f

FIGS = [fig01, fig02, fig03, fig04, fig05, fig06, fig07, fig08, fig09, fig10, fig11]

def main():
    check = "--check" in sys.argv
    bad = 0
    for fn in FIGS:
        f = fn()
        path = os.path.join(HERE, f.name + ".svg")
        text = f.svg()
        if check:
            old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
            if old != text: print("DIFFERS", f.name + ".svg"); bad += 1
        else:
            open(path, "w", encoding="utf-8", newline="\n").write(text)
    for p in problems: print("TOO WIDE", p)
    if check and (bad or problems): sys.exit(1)
    print(("OK      " if check else "wrote   ") + f"{len(FIGS)} figure(s)")

if __name__ == "__main__":
    main()
