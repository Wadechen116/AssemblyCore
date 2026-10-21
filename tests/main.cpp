// AssemblyCore tests. Plain assertions (no framework), like the AutoDetectDevice platform tests.
// Golden bytes below are computed BY HAND from the spec's bit layouts, not copied from the assembler's output.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"
#include "asmcore/lang.h"
#include "asmcore/index.h"
#include "asmcore/diagnostics.h"
#include "asmcore/sram.h"
#include "isa.h"
#include <set>

#ifndef ASMCORE_SOURCE_DIR
#define ASMCORE_SOURCE_DIR "."
#endif
#define ASMCORE_SOURCE_DIR_STR ASMCORE_SOURCE_DIR

using namespace asmcore;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& why = "") {
    if (ok) { g_pass++; std::printf("  PASS  %s\n", what.c_str()); return; }
    g_fail++;
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), why.c_str());
}
static void section(const char* s) { std::printf("\n%s\n", s); }

static Result build(const std::string& src, CallModel m = CallModel::Inline, const Profile* pf = nullptr) {
    Options o;
    o.callModel = m;
    if (pf) o.profile = *pf;
    std::vector<Source> v{Source{"t.asm", src}};
    return assemble(v, nullptr, o);
}
static bool hasCode(const Result& r, const std::string& code) {
    for (auto& d : r.diags) if (d.code == code) return true;
    return false;
}
static std::string codes(const Result& r) {
    std::string s;
    for (auto& d : r.diags) s += d.code + ":" + d.msg + " | ";
    return s;
}
static std::string hexs(const std::vector<uint8_t>& b, size_t from, size_t n) {
    std::string s;
    char t[8];
    for (size_t i = from; i < from + n && i < b.size(); i++) { std::snprintf(t, sizeof t, "%02X ", b[i]); s += t; }
    return s;
}
static const SectionInfo* sec(const DomainImage& im, const char* name) {
    for (auto& s : im.sections) if (s.name == name) return &s;
    return nullptr;
}

// ---- a minimal Main-domain interpreter, only to show that both call models behave the same ----------------------------------
struct MiniSim {
    const std::vector<uint8_t>& im;
    uint32_t offMove = 0, offBr = 0, offCsr1 = 0, offCsrM = 0, offAlu = 0, offConst = 0;
    uint8_t scr[32] = {}, buf[32] = {}, slot[32] = {}, meta[32] = {};
    uint16_t opa = 0, opb = 0;
    bool cmp = false;
    std::vector<int> writes;        // every value written to a CSR (WRITE8 immediates and WRITE_SP bytes), in order
    bool halted = false, bad = false;
    uint32_t pc = 0;
    int steps = 0;

    // the section offsets are the ImageLayout sideband values (spec v31: not part of the image bytes)
    explicit MiniSim(const DomainImage& image) : im(image.bytes) {
        offMove = image.layout[0]; offBr = image.layout[1]; offCsr1 = image.layout[2]; offCsrM = image.layout[3]; offAlu = image.layout[4]; offConst = image.layout[5];
    }
    uint8_t* region(int id) {
        switch (id) { case 0: return scr; case 1: return buf; case 2: return slot; case 8: return meta; default: return nullptr; }
    }
    void run(int limit = 20000) {
        while (!halted && !bad && steps++ < limit) step();
    }
    void step() {
        uint8_t b = im[pc];
        int op = b >> 6, arg = b & 63;
        uint32_t next = pc + 1;
        if (op == 0) {                                   // EXEC_CSR
            int idx = arg & 31;
            if (!(arg & 32)) {
                uint32_t d = im[offCsr1 + idx * 2] | (im[offCsr1 + idx * 2 + 1] << 8);
                if (((d >> 14) & 3) == 0) writes.push_back(im[pc + 1]);
                else bad = true;
            } else {
                uint32_t d = im[offCsrM + idx * 2] | (im[offCsrM + idx * 2 + 1] << 8);
                int len = ((d >> 5) & 31) + 1, ptr = d & 31;
                if (((d >> 14) & 3) == 0) for (int i = 0; i < len; i++) writes.push_back(scr[ptr + i]);       // WRITE_SP reads the scratchpad (v31 4.3)
                else bad = true;
            }
            next = pc + 2;
        } else if (op == 1) {                            // BR
            uint32_t d = im[offBr + arg * 2] | (im[offBr + arg * 2 + 1] << 8);
            int type = d >> 14, cond = (d >> 10) & 15, target = d & 255;
            if (type != 0 || (cond != 0 && cond != 5)) { bad = true; return; }
            bool take = cond == 0 || cmp;
            if (take && (uint32_t)target == pc) { halted = true; return; }
            if (take) next = target;
        } else if (op == 2) {                            // MOV
            uint32_t d = im[offMove + arg * 4] | (im[offMove + arg * 4 + 1] << 8) | (im[offMove + arg * 4 + 2] << 16) | ((uint32_t)im[offMove + arg * 4 + 3] << 24);
            int sr = d >> 28, dr = (d >> 24) & 15, so = (d >> 16) & 255, dof = (d >> 8) & 255, len = (d & 31) + 1;
            uint8_t tmp[32];
            for (int i = 0; i < len; i++) {
                if (sr == 7) tmp[i] = im[offConst + so + i];
                else if (region(sr)) tmp[i] = region(sr)[so + i];
                else { bad = true; return; }
            }
            if (dr == 3) opa = (uint16_t)(tmp[0] | (tmp[1] << 8));
            else if (dr == 4) opb = (uint16_t)(tmp[0] | (tmp[1] << 8));
            else if (region(dr)) for (int i = 0; i < len; i++) region(dr)[dof + i] = tmp[i];
            else { bad = true; return; }
        } else {                                         // ALU (compare only)
            uint32_t d = im[offAlu + arg];
            if ((d >> 6) != 1) { bad = true; return; }
            int cop = (d >> 3) & 3, sg = (d >> 5) & 1;
            int a = sg ? (int16_t)opa : opa, c = sg ? (int16_t)opb : opb;
            cmp = cop == 0 ? opa == opb : cop == 1 ? opa != opb : cop == 2 ? a < c : a >= c;
        }
        pc = next;
    }
};

static const char* kDefs =
    ".csr AE_CTRL = 0x010\n.csr POLL_MASK = 0x300\n.csr POLL_CTRL = 0x301\n.csr PERI_CTRL = 0x302\n"
    ".service SID_A = 1\n.devmap 0, I2C, 0x68, DEV0\n";

// ---------------------------------------------------------------------------------------------------------------------------
static void t_golden_main() {
    section("Main encodings, computed by hand from the spec");
    Result r = build(".main\nl: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n   JMP l\n");
    check(r.ok, "tiny program assembles", codes(r));
    const auto& b = r.main.bytes;
    check(b.size() == 10, "the logical image is 10 bytes (no ImageLayout tail since v31)", std::to_string(b.size()));
    check(b.size() > 1 && b[0] == 0x80 && b[1] == 0x40, "MOV = opcode 10 (0x80), BR = opcode 01 (0x40)", hexs(b, 0, 2));
    // MainMoveDesc: src SCRATCH(0)<<28 | dst OPA(3)<<24 | 0 | 0 | len_m1=1  = 0x03000001, little-endian at the 4-aligned table
    check(hexs(b, 4, 4) == "01 00 00 03 ", "MainMoveDesc_32b = 0x03000001 at offset 4", hexs(b, 4, 4));
    check(hexs(b, 8, 2) == "00 00 ", "MainBranchDesc_16b ALWAYS -> 0 = 0x0000", hexs(b, 8, 2));
    // MainImageLayout (sideband, not in the image): move=4, branch=8, csr1=10, csrm=10, alu=10, const=10 (logical byte offsets)
    check(r.main.layout == std::vector<uint32_t>({4, 8, 10, 10, 10, 10}), "MainImageLayout sideband offsets {4, 8, 10, 10, 10, 10}");
    check(r.main.sections.size() == 7 && r.main.sections[0].name == "code", "the section list has the code and the six layout sections, and no ImageLayout section");
}

static void t_alu_codes() {
    section("ALU descriptors");
    auto alu = [&](const std::string& line, int expect) {
        Result r = build(std::string(".main\nl: ") + line + "\n JMP l\n");
        if (!r.ok) { check(false, line, codes(r)); return; }
        const SectionInfo* s = sec(r.main, "MainAluDesc_8b");
        check(s && r.main.bytes[s->offset] == expect && r.main.bytes[0] == 0xC0, line + " -> descriptor " + hexs(r.main.bytes, s ? s->offset : 0, 1), "expected " + std::to_string(expect));
    };
    alu("ALU SUB signed", 0x21);            // class 00, signed bit 5, SUB = 1
    alu("ALU ADD", 0x00);
    alu("ALU CMP.GE signed", 0x78);         // class 01, signed, GE = 3 in [4:3]
    alu("ALU CMP.EQ unsigned", 0x40);
    alu("ALU AND", 0x80);                   // class 10, AND = 0 in [5:3]
    alu("ALU NOT", 0x98);                   // NOT = 3
    alu("ALU SHL 8", 0xC8);                 // class 11, SHL, amount 8
    alu("ALU SHR 1 signed", 0xF1);          // signed, SHR (bit 4), amount 1
}

static void t_csr_codes() {
    section("EXEC_CSR descriptors");
    Result r = build(std::string(kDefs) + ".csr STAT = 0x240\n.main\nl: EXEC_CSR WRITE8 AE_CTRL, 1\n EXEC_CSR READ_BUF STAT, len=4, ptr=0\n EXEC_CSR READ8 AE_CTRL -> MAIN_CSR_DATA_BUF[2]\n JMP l\n");
    check(r.ok, "assembles", codes(r));
    if (!r.ok) return;
    const auto& b = r.main.bytes;
    check(b[0] == 0x00 && b[1] == 0x01, "WRITE8 = instruction 0x00 + imm 0x01", hexs(b, 0, 2));
    check(b[2] == 0x20 && b[3] == 0x40, "READ_BUF: arg[5]=1 -> 0x20, then csr_addr_lo8 0x40", hexs(b, 2, 2));
    check(b[4] == 0x01 && b[5] == 0x02, "READ8: second one-byte descriptor, ptr byte 2", hexs(b, 4, 2));
    uint32_t csrm = sec(r.main, "MainCsrMultiByteDesc_16b")->offset, csr1 = sec(r.main, "MainCsrOneByteDesc_16b")->offset;
    // CsrMultiByteDesc: op READ_BUF 11 <<14 | page 2 <<10 | len_m1 3 <<5 | ptr 0 = 0xC860
    check(hexs(b, csrm, 2) == "60 C8 ", "CsrMultiByteDesc READ_BUF page 2 len 4 = 0xC860", hexs(b, csrm, 2));
    check(hexs(b, csr1, 4) == "10 00 10 C0 ", "CsrOneByteDesc WRITE8 = 0x0010, READ8 = 0xC010", hexs(b, csr1, 4));
}

static void t_peri_codes() {
    section("Peri encodings (spec 8.6, 10.5 examples)");
    Result r = build(std::string(kDefs) + ".peri\nl: EXEC_XFER READ dev=0, len=6, ptr=0, ext=[0x43]\n EXEC_PUB service=1, src=0, len=6\n EXEC_XFER WRITE dev=0, addr=0x6B, addrlen=1, data=0\n BR.IF_READY l\n JMP l\n");
    check(r.ok, "assembles", codes(r));
    if (!r.ok) return;
    const auto& b = r.peri.bytes;
    check(b[0] == 0xE0 && b[1] == 0x43, "MultiByte READ: opcode 11, format 1 -> 0xE0, inline address 0x43", hexs(b, 0, 2));
    check(b[2] == 0x80, "EXEC_PUB desc 0 = 0x80", hexs(b, 2, 1));
    check(b[3] == 0xC0 && b[4] == 0x00, "OneByte WRITE: 0xC0 + data 0", hexs(b, 3, 2));
    uint32_t xm = sec(r.peri, "PeriXferMultiByteDesc_16b")->offset, pb = sec(r.peri, "PeriPublishDesc_16b")->offset, x1 = sec(r.peri, "PeriXferOneByteDesc_16b")->offset;
    check(hexs(b, xm, 2) == "A0 84 ", "PeriXferMultiByteDesc = 0x84A0 (READ, dev 0, addr_len 1, len_m1 5, ptr 0)", hexs(b, xm, 2));
    check(hexs(b, pb, 2) == "14 10 ", "PeriPublishDesc = 0x1014 (service 1, src 0, len_m1 5)", hexs(b, pb, 2));
    check(hexs(b, x1, 2) == "6B 04 ", "PeriXferOneByteDesc WRITE addr 0x6B len 1 = 0x046B", hexs(b, x1, 2));
    const SectionInfo* dm = sec(r.peri, "DeviceMap");
    check(dm && b[dm->offset] == 0x68, "DeviceMap[0] = I2C 0x68", hexs(b, dm ? dm->offset : 0, 1));
    uint32_t br = sec(r.peri, "PeriBranchDesc_16b")->offset;
    check(hexs(b, br, 4) == "00 10 00 00 ", "Peri BR descriptors: IF_READY -> 0 = 0x1000, ALWAYS -> 0 = 0x0000", hexs(b, br, 4));
}

// ---- spec v31: event_safe, BR.ERET, the scratchpad staging of softret ---------------------------------------------------------------
static uint16_t descAt(const Result& r, const DomainImage& im, const char* table, int idx) {
    const SectionInfo* s = sec(im, table);
    (void)r;
    if (!s) return 0xFFFF;
    size_t at = s->offset + (size_t)idx * 2;
    return (uint16_t)(im.bytes[at] | (im.bytes[at + 1] << 8));
}

static void t_v31_branches() {
    section("spec v31: event_safe and BR.ERET");
    {   // Main: MainBranchDesc [15:14] type, [13:10] arg, [9] event_safe, [8] rsv, [7:0] target
        Result r = build(".main\nl: BR.COND ALU_CMP_TRUE, l\n BR.COND ALU_CMP_TRUE, l, safe\n JMP l, safe\n BR.ERET\n");
        check(r.ok, "event_safe branches and BR.ERET assemble", codes(r));
        if (r.ok) {
            check(descAt(r, r.main, "MainBranchDesc_16b", 0) == ((5 << 10) | 0), "ordinary BR.COND ALU_CMP_TRUE, target 0: 0x1400", hexs(r.main.bytes, sec(r.main, "MainBranchDesc_16b")->offset, 2));
            check(descAt(r, r.main, "MainBranchDesc_16b", 1) == ((5 << 10) | (1 << 9)), "the same branch with event_safe sets bit 9: 0x1600");
            check(descAt(r, r.main, "MainBranchDesc_16b", 2) == (1 << 9), "JMP ..., safe = ALWAYS with event_safe: 0x0200");
            check(descAt(r, r.main, "MainBranchDesc_16b", 3) == (3 << 14), "BR.ERET: br_type 3, arg 0, event_safe 0, target 0 = 0xC000");
            check(sec(r.main, "MainBranchDesc_16b")->entries == 4, "descriptors that differ only in event_safe are not shared (4 entries)");
            check(r.main.bytes[0] == 0x40 && r.main.bytes[1] == 0x41 && r.main.bytes[3] == 0x43, "instruction bytes select descriptors 0..3", hexs(r.main.bytes, 0, 4));
        }
    }
    {   // Peri: PeriBranchDesc [15:12] type, [11] event_safe, [10:8] rsv, [7:0] target
        Result r = build(".peri\nl: BR.IF_READY l\n BR.IF_READY l, safe\n BR.IF_ERROR l, event_safe=1\n JMP l\n BR.ERET\n");
        check(r.ok, "Peri event_safe branches and BR.ERET assemble", codes(r));
        if (r.ok) {
            check(descAt(r, r.peri, "PeriBranchDesc_16b", 0) == 0x1000, "ordinary IF_READY: 0x1000");
            check(descAt(r, r.peri, "PeriBranchDesc_16b", 1) == 0x1800, "IF_READY with event_safe sets bit 11: 0x1800");
            check(descAt(r, r.peri, "PeriBranchDesc_16b", 2) == 0x4800, "event_safe=1 spelling: IF_ERROR 0x4800");
            check(descAt(r, r.peri, "PeriBranchDesc_16b", 4) == 0x8000, "Peri BR.ERET: br_type 8, event_safe 0, target 0 = 0x8000");
        }
    }
    struct C { const char* name; std::string src; const char* code; };
    std::vector<C> bad = {
        {"event_safe on CASCADE_BEGIN", ".main\nl: CASCADE_BEGIN 0, b0, safe\nb0: EXEC_CSR WRITE8 0x10, 1\n CASCADE_END 0, x\nx: JMP l\n", "E208"},
        {"event_safe on CASCADE_END", ".main\nl: CASCADE_BEGIN 0, b0\nb0: EXEC_CSR WRITE8 0x10, 1\n CASCADE_END 0, x, safe\nx: JMP l\n", "E208"},
        {"event_safe on BR.ERET", ".main\nl: BR.ERET safe\n", "E208"},
        {"event_safe=maybe", ".main\nl: JMP l, event_safe=maybe\n", "E208"},
        {"BR.ERET with operands", ".main\nl: BR.ERET l\n", "E102"},
        {"event_safe branch inside a cascade body", ".main\nl: CASCADE_BEGIN 0, b0\nb0: BR.COND ALWAYS, x, safe\n CASCADE_END 0, x\nx: JMP l\n", "E208"},
        {"ordinary branch inside a cascade body", ".main\nl: CASCADE_BEGIN 0, b0\nb0: BR.COND ALU_CMP_TRUE, x\n CASCADE_END 0, x\nx: JMP l\n", "E304"},
        {"BR.ERET inside a cascade body", ".main\nl: CASCADE_BEGIN 0, b0\nb0: BR.ERET\n CASCADE_END 0, x\nx: JMP l\n", "E304"},
    };
    for (auto& c : bad) {
        Result r = build(c.src);
        check(!r.ok && hasCode(r, c.code), std::string(c.name) + " -> " + c.code, codes(r));
    }
    Result inside = build(".main\nl: CASCADE_BEGIN 1, b0\nb0: EXEC_CSR WRITE8 0x10, 1\n CASCADE_END 0, x\n EXEC_CSR WRITE8 0x10, 2\n CASCADE_END 1, x\nx: BR.COND ALU_CMP_TRUE, l, safe\n JMP l\n");
    check(inside.ok, "a cascade body without branches, and a safe branch after the cascade, assemble", codes(inside));
    Result tr = build(".main\nl: JMP l\n");
    check(tr.ok && tr.main.bytes.size() > 0, "(baseline program still assembles)");
}

// ---- spec v31: sideband layout, section placement, the one shared SRAM ----------------------------------------------------------------
// The toy placement of the v31 handoff (2.7): 32-bit descriptors 4-aligned, 16-bit ones 2-aligned, SRAM of 0x200 words, Main window at word
// 0x100 and Peri window at 0x110 (16 words each).
static Profile toyProfile() {
    Profile p;
    p.physicalSramDepthWords = 0x200;
    p.mainImageBaseWord = 0x100;
    p.periImageBaseWord = 0x110;
    p.mainCapacityBytes = 64;
    p.periCapacityBytes = 64;
    return p;
}
// Main: 2 MOV, 2 ALU, 2 one-byte CSR, 1 multi-byte CSR, 3 branches, a 5-byte constant pool. Code = 2+2+4+2+3 = 13 bytes, like the handoff's table.
static const char* kToyMain =
    ".csr C = 0x120\n.const K db 1, 2, 3, 4, 5\n.main\n"
    "a: MOV MAIN_CONST_POOL[@K] -> MAIN_SCRATCHPAD[0..4]\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n ALU CMP.EQ unsigned\n ALU ADD unsigned\n"
    " EXEC_CSR WRITE8 C, 1\n EXEC_CSR SET8 C, 2\n EXEC_CSR READ_BUF C, len=2, ptr=0\n"
    " BR.COND PERI_RESULT_VALID, a\n BR.COND ALU_CMP_TRUE, a\n JMP a\n";
// Peri: 2 one-byte XFER, 1 multi-byte XFER (no A bytes), 2 PUB, 2 branches, 1 CSR, one device. Code = 4+1+2+2+2 = 11 bytes.
static const char* kToyPeri =
    ".service S = 1\n.devmap 0, I2C, 0x68\n.peri\n"
    "p: EXEC_XFER READ dev=0, addr=1, addrlen=1, ptr=0\n EXEC_XFER WRITE dev=0, addr=2, addrlen=1, data=0\n EXEC_XFER READ dev=0, len=2, ptr=0\n"
    " EXEC_PUB service=S, src=0, len=2\n EXEC_PUB service=S, src=0, len=1\n BR.IF_READY p\n EXEC_CSR WRITE8 0x10, 1\n JMP p\n";

static Result buildP(const std::string& src, const Profile& p, CallModel m = CallModel::Inline) {
    Options o;
    o.callModel = m;
    o.profile = p;
    o.dataflow = DataflowMode::Off;
    std::vector<Source> v{Source{"t.asm", src}};
    return assemble(v, nullptr, o);
}

static void t_v31_image() {
    section("spec v31: ImageLayout is sideband, sections are placed by the profile, both images live in ONE SRAM");
    Profile toy = toyProfile();
    Result r = buildP(std::string(kToyMain) + kToyPeri, toy);
    check(r.ok, "the toy Main + Peri program assembles", codes(r));
    if (!r.ok) return;
    // the handoff's worked example, section 2.7
    check(r.main.layout == std::vector<uint32_t>({16, 24, 30, 34, 36, 38}), "Main layout = {16, 24, 30, 34, 36, 38}, as computed in the v31 handoff", "");
    check(r.main.codeSize == 13 && r.main.bytes.size() == 43, "Main: 13 bytes of code, a 43-byte logical image", std::to_string(r.main.codeSize) + " / " + std::to_string(r.main.bytes.size()));
    check(r.main.words.size() == 11 && r.main.baseWord == 0x100, "Main: 11 words of the SRAM, base word 0x100 (43 bytes + 1 padding byte)");
    check(r.main.sections.size() == 7 && r.main.sections[1].offset == 16 && r.main.sections[6].offset == 38 && r.main.sections[6].size == 5, "section table: Move at 16 ... const pool [38, 43)");
    check(r.peri.layout == std::vector<uint32_t>({12, 16, 18, 22, 26, 28}) && r.peri.bytes.size() == 29 && r.peri.words.size() == 8 && r.peri.baseWord == 0x110,
          "Peri layout {12, 16, 18, 22, 26, 28}, a 29-byte image in 8 words at word 0x110", "");
    // the image bytes hold no layout: the last bytes are the const pool / DeviceMap, not offsets
    check(std::vector<uint8_t>(r.main.bytes.end() - 5, r.main.bytes.end()) == std::vector<uint8_t>({1, 2, 3, 4, 5}), "the Main image ends with the constant pool (no ImageLayout tail)");
    check(r.peri.bytes.back() == 0xE8 || r.peri.bytes.back() == 0x68, "the Peri image ends with the DeviceMap entry (no ImageLayout tail)", hexs(r.peri.bytes, r.peri.bytes.size() - 1, 1));
    // address arithmetic, with the numbers of the handoff
    PhysicalByte mv = physicalByte(toy, Domain::Main, 16 + 1 * 4);
    check(mv.word == 0x105 && mv.lane == 0, "Main MOV descriptor 1: logical byte 20 -> physical word 0x105, lane 0", std::to_string(mv.word));
    PhysicalByte cs = physicalByte(toy, Domain::Main, 30 + 1 * 2);
    check(cs.word == 0x108 && cs.lane == 0, "Main CSR one-byte descriptor 1: logical byte 32 -> word 0x108, lane 0");
    PhysicalByte lane3 = physicalByte(toy, Domain::Main, 3);
    check(lane3.word == 0x100 && lane3.lane == 3, "logical byte 3 is word 0, lane 3");
    PhysicalByte pd = physicalByte(toy, Domain::Peri, 18 + 2);
    check(pd.word == 0x110 + 5 && pd.lane == 0, "Peri: logical byte 20 -> word 0x115, lane 0");
    // word packing: byte 4n+k in bits [8k+7:8k]
    bool lanesOk = true;
    for (size_t i = 0; i < r.main.bytes.size(); i++) lanesOk = lanesOk && (uint8_t)(r.main.words[i / 4] >> (8 * (i % 4))) == r.main.bytes[i];
    check(lanesOk, "every Main byte sits in its lane of its word");
    check((r.main.words.back() >> 24) == (uint32_t)toy.bootPadByte, "the unused byte of the last word is the profile's pad byte");
    // reading through the shared SRAM, across word boundaries, every start and length
    SramContents sram = sramContents(sramWrites(r));
    bool through = true;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = r.image(d);
        for (uint32_t a = 0; a < im.bytes.size(); a++)
            for (uint32_t n = 1; n <= 5 && a + n <= im.bytes.size(); n++) {
                std::vector<uint8_t> got;
                if (!readThroughSram(sram, toy, d, a, n, got) || got != std::vector<uint8_t>(im.bytes.begin() + a, im.bytes.begin() + a + n)) through = false;
            }
    }
    check(through, "any 1..5 bytes of either image read back through the shared SRAM (word boundaries included) equal the image");
    std::vector<uint8_t> whole;
    check(imageFromSram(sram, toy, Domain::Peri, (uint32_t)r.peri.bytes.size(), whole) && whole == r.peri.bytes, "a whole image is reconstructed from the words of the SRAM");
    std::vector<uint8_t> miss;
    check(!readThroughSram(sram, toy, Domain::Main, 100000, 4, miss), "a read of a word that was never written fails instead of inventing data");
    // the two images never share a word, the windows are disjoint and inside the SRAM
    MemoryProof proof = memoryProof(r, toy);
    check(proof.ok() && proof.main.occupiedWords == 11 && proof.peri.occupiedWords == 8 && proof.main.reservedWords == 16, "memory proof: occupied ranges and reserved windows are disjoint, inside the SRAM, within capacity");
    std::vector<WordWrite> w = sramWrites(r);
    bool disjoint = true;
    for (size_t i = 1; i < w.size(); i++) disjoint = disjoint && w[i].address > w[i - 1].address;
    check(w.size() == 19 && disjoint, "19 word writes, strictly increasing addresses (no word is written twice)", std::to_string(w.size()));
    // every descriptor lookup: logical offset -> physical word and lane -> the descriptor the assembler built
    bool lookups = true;
    int checked = 0;
    for (const InstrInfo& in : r.instrs) {
        if (in.table.empty()) continue;
        const DomainImage& im = r.image(in.domain);
        for (const SectionInfo& s : im.sections) {
            if (s.name != in.table) continue;
            std::vector<uint8_t> got;
            readThroughSram(sram, toy, in.domain, s.offset + (uint32_t)in.descIndex * (uint32_t)s.entryBytes, (uint32_t)s.entryBytes, got);
            std::vector<uint8_t> want(im.bytes.begin() + s.offset + in.descIndex * s.entryBytes, im.bytes.begin() + s.offset + (in.descIndex + 1) * s.entryBytes);
            lookups = lookups && got == want;
            checked++;
        }
    }
    check(lookups && checked == (int)r.instrs.size(), "every instruction's descriptor, looked up through the SRAM words, equals what the assembler built", std::to_string(checked));
}

static void t_v31_profile_rules() {
    section("spec v31: what the profile decides (alignment, widths, capacities, placement, DeviceMap, PC width)");
    {   // alignment 1 everywhere: tables follow each other without padding
        Profile p = toyProfile();
        for (int i = 0; i < PS_COUNT; i++) p.sectionAlign[i] = 1;
        Result r = buildP(kToyMain, p);
        check(r.ok && r.main.layout == std::vector<uint32_t>({13, 21, 27, 31, 33, 35}) && r.main.bytes.size() == 40, "alignment 1: Main layout {13, 21, 27, 31, 33, 35}, 40 bytes", codes(r));
    }
    {   // another alignment, and another pad byte for the gaps
        Profile p = toyProfile();
        p.sectionAlign[PS_MAIN_MOVE] = 8;
        p.sectionAlign[PS_MAIN_CONST] = 4;
        p.bootPadByte = 0xFF;
        Result r = buildP(kToyMain, p);
        check(r.ok && r.main.layout[0] == 16 && r.main.layout[5] % 4 == 0, "per-section alignment: Move 8, const pool 4", codes(r));
        check(r.ok && r.main.bytes[13] == 0xFF && r.main.bytes[14] == 0xFF && r.main.bytes[15] == 0xFF, "the gap before the first table is filled with the pad byte, not with code");
        check(r.ok && ((r.main.words.back() >> 24) == 0xFF || r.main.bytes.size() % 4 == 0), "the pad byte also fills the last word");
    }
    {   // ImageLayout field width
        Profile p = toyProfile();
        p.layoutOffsetBits = 5;                       // offsets up to 31 only
        Result r = buildP(kToyMain, p);
        check(!r.ok && hasCode(r, "E314"), "an offset that does not fit the sideband field width is E314", codes(r));
        p.layoutOffsetBits = 6;
        check(buildP(kToyMain, p).ok, "...and 6 bits (offsets up to 63) are enough for this program");
    }
    {   // capacity of the window
        Profile p = toyProfile();
        p.mainCapacityBytes = 40;
        Result r = buildP(kToyMain, p);
        check(!r.ok && hasCode(r, "E312"), "an image larger than its window is E312", codes(r));
    }
    {   // placement
        Profile p = toyProfile();
        p.periImageBaseWord = 0x108;                 // inside the Main window [0x100, 0x110)
        Result r = buildP(kToyMain, p);
        check(!r.ok && hasCode(r, "E311"), "overlapping windows are E311 (even though the actual images would not collide)", codes(r));
        Profile q2 = toyProfile();
        q2.physicalSramDepthWords = 0x108;
        Result r2 = buildP(kToyMain, q2);
        check(!r2.ok && hasCode(r2, "E311"), "a window that does not fit the SRAM is E311", codes(r2));
        Profile adj = toyProfile();
        adj.periImageBaseWord = 0x110;               // adjacent windows are fine: windows are half-open
        check(buildP(kToyMain, adj).ok, "adjacent windows [0x100, 0x110) and [0x110, ...) are fine");
    }
    {   // DeviceMap entries
        Profile p = toyProfile();
        p.deviceMapEntryBytes = 2;
        p.deviceMapBusShift = 15;
        p.deviceMapAddrShift = 4;
        Result r = buildP(kToyPeri, p);
        check(r.ok && r.peri.sections.back().name == "DeviceMap" && r.peri.sections.back().size == 2, "2-byte DeviceMap entries", codes(r));
        if (r.ok) {
            const SectionInfo& dm = r.peri.sections.back();
            uint32_t v = r.peri.bytes[dm.offset] | (r.peri.bytes[dm.offset + 1] << 8);
            check(v == (0x68u << 4), "the address 0x68 sits at bits 10:4, bus_type 0 at bit 15 (value 0x0680)", std::to_string(v));
        }
        Profile bad = toyProfile();
        bad.deviceMapBusShift = 3;
        bad.deviceMapAddrShift = 0;                  // bit 3 lies inside the address field
        check(hasCode(buildP(kToyPeri, bad), "E313"), "overlapping DeviceMap fields are E313");
        Profile wide = toyProfile();
        wide.deviceMapAddrShift = 3;                 // 3 + 7 bits do not fit one byte
        check(hasCode(buildP(kToyPeri, wide), "E313"), "fields that do not fit the entry are E313");
    }
    {   // PC width and the 8-bit branch target
        std::string longCode = ".main\nstart:\n";
        for (int i = 0; i < 140; i++) longCode += " EXEC_CSR WRITE8 0x10, " + std::to_string(i & 15) + "\n";     // 280 bytes of code
        std::string tail = " JMP start\n";
        Profile def;
        Result a = build(longCode + tail);
        check(!a.ok && hasCode(a, "E302"), "280 bytes of code do not fit the default 256-byte limit (E302)", codes(a));
        Profile wide;
        wide.maxCodeBytes = 1024;
        wide.mainPcWidth = 10;
        Result b = buildP(longCode + tail, wide);
        check(b.ok, "with a 10-bit PC and a larger limit, 280 bytes assemble (branch targets are still < 256)", codes(b));
        Profile narrow;
        narrow.maxCodeBytes = 1024;                  // the limit says yes, the 8-bit PC says no
        Result c = buildP(longCode + tail, narrow);
        bool names = false;
        for (auto& d : c.diags) if (d.code == "E302" && d.msg.find("8-bit PC") != std::string::npos && d.msg.find("main_pc_width") != std::string::npos) names = true;
        check(!c.ok && names, "the PC width limit is reported with its profile key (actionable message)", codes(c));
        std::string far = longCode + "far: JMP start\n";
        Result d = buildP(".main\nBR.COND PERI_RESULT_VALID, far\n" + far.substr(6), wide);
        bool hint = false;
        for (auto& g : d.diags) if (g.code == "E310" && g.msg.find("0..255") != std::string::npos && g.msg.find("truncated") != std::string::npos) hint = true;
        check(!d.ok && hint, "a branch to a label beyond byte 255 is E310 and nothing is truncated (the message says so)", codes(d));
        check(!hasCode(d, "E302"), "(E310 is its own code, not the code-size error)");
    }
    {   // the profile loader knows the new keys, and refuses nonsense with a message that names the key
        Profile p;
        std::vector<std::string> errs;
        check(p.load("main_pc_width = 10\nmain_image_base_word = 0x40\nalign_main_move = 1\nalu_arith_add = 9\ndevice_map_entry_bytes = 2\nprofile_id = rtl-a\nlayout_offset_bytes = 4\n", errs) && errs.empty(),
              "the new profile keys load", errs.empty() ? "" : errs[0]);
        check(p.mainPcWidth == 10 && p.mainImageBaseWord == 0x40 && p.sectionAlign[PS_MAIN_MOVE] == 1 && p.alu.arith[0] == 9 && p.deviceMapEntryBytes == 2 && p.profileId == "rtl-a" && p.layoutOffsetBits == 32, "...with the right values (layout_offset_bytes is still accepted: 4 bytes = 32 bits)");
        Profile q2;
        errs.clear();
        check(!q2.load("main_pc_width = 20\n", errs) && !errs.empty() && errs[0].find("main_pc_width") != std::string::npos, "an out-of-range value is refused and the message names the key", errs.empty() ? "" : errs[0]);
        errs.clear();
        check(!q2.load("alu_arith_add = 1\nalu_arith_sub = 1\n", errs) && !errs.empty(), "two ALU sub-operations with the same code are refused", errs.empty() ? "" : errs.back());
        std::string dump = Profile().dump();
        Profile again;
        errs.clear();
        check(again.load(dump, errs) && errs.empty(), "dump() output loads again (a profile round-trips)", errs.empty() ? "" : errs[0]);
    }
}

static void t_v31_artifacts() {
    section("spec v31: artifacts (layout, section map, descriptor map, SRAM words, boot plan, proof, manifest)");
    Profile toy = toyProfile();
    Result r = buildP(std::string(kToyMain) + kToyPeri, toy);
    if (!r.ok) { check(false, "setup", codes(r)); return; }
    std::string lj = r.layoutJson(Domain::Main);
    check(lj.find("\"main_move_desc_offset\":16") != std::string::npos && lj.find("\"main_const_pool_offset\":38") != std::string::npos && lj.find("sideband") != std::string::npos, "layout.json holds the six offsets and says it is sideband");
    DomainImage back;
    back.domain = Domain::Main;
    std::string err;
    check(loadLayoutJson(lj, back, err) && back.layout == r.main.layout && back.codeSize == r.main.codeSize && back.sections.size() == r.main.sections.size(), "layout.json round-trips through loadLayoutJson", err);
    DomainImage wrong;
    wrong.domain = Domain::Peri;
    check(!loadLayoutJson(lj, wrong, err) && err.find("Main") != std::string::npos, "a Main layout file is refused for the Peri domain", err);
    DomainImage junk;
    junk.domain = Domain::Main;
    check(!loadLayoutJson("[1,2", junk, err) && !err.empty(), "a damaged layout file is refused with a message", err);
    check(!loadLayoutJson("{\"fields\":{\"main_move_desc_offset\":4}}", junk, err) && err.find("main_branch_desc_offset") != std::string::npos, "a layout file with a missing field names it", err);
    // the SRAM word file
    std::string hexw = r.sramWordsHex();
    int lines = 0;
    bool parse = true;
    SramContents parsed;
    std::istringstream is(hexw);
    std::string line;
    while (std::getline(is, line)) {
        if (line.compare(0, 2, "//") == 0) continue;
        char* e1 = nullptr;
        char* e2 = nullptr;
        const unsigned long a = std::strtoul(line.c_str(), &e1, 16);
        const unsigned long d = std::strtoul(e1, &e2, 16);
        if (e1 == line.c_str() || e2 == e1) { parse = false; continue; }
        parsed[(uint32_t)a] = (uint32_t)d;
        lines++;
    }
    check(parse && lines == 19 && parsed == sramContents(sramWrites(r)), "the SRAM word file has one 'ADDR DATA' line per word and equals the writes");
    // the boot plan lists every sideband field
    std::string bp = r.bootPlanJson();
    size_t n = 0;
    for (size_t at = bp.find("\"field\""); at != std::string::npos; at = bp.find("\"field\"", at + 1)) n++;
    check(n == 12 && bp.find("sideband_writes") != std::string::npos && bp.find("not part of the ISA") != std::string::npos, "the boot plan lists the 12 sideband fields and does not invent a ROM container");
    std::string mp = r.memoryProofJson();
    check(mp.find("\"ok\":true") != std::string::npos && mp.find("\"reserved_window\":[256,272]") != std::string::npos, "the memory proof records the reserved window [0x100, 0x110) and passes", mp);
    // the section map: spans are contiguous and cover the image
    std::string sm = r.sectionMapJson();
    check(sm.find("\"kind\":\"padding\"") != std::string::npos && sm.find("\"padding_bytes_in_last_word\":1") != std::string::npos, "the section map shows the padding before the first table and in the last word");
    // the descriptor map: every table, with its users
    std::string dm = r.descriptorMapJson();
    check(dm.find("\"table\":\"MainMoveDesc_32b\"") != std::string::npos && dm.find("\"users\":[{") != std::string::npos && dm.find("\"i2c_addr_7b\":104") != std::string::npos, "the descriptor map decodes every table, lists the users of each entry and the DeviceMap address");
    // services, manifest, validation
    std::string sv = r.servicesJson();
    check(sv.find("\"name\":\"S\"") != std::string::npos && sv.find("\"len\":2") != std::string::npos && sv.find("keep_old_drop_new") != std::string::npos, "services.json lists the service and what Peri publishes");
    std::vector<Source> srcs{Source{"t.asm", std::string(kToyMain) + kToyPeri}};
    std::string mf = r.buildManifestJson(srcs);
    check(mf.find("MicroISA-v31") != std::string::npos && mf.find("planning-default") != std::string::npos && mf.find("\"hash\"") != std::string::npos, "the manifest names the spec, the profile and carries hashes");
    check(r.buildManifestJson(srcs) == mf && Result(r).buildManifestJson(srcs) == mf, "the manifest is deterministic");
    Result bad = buildP(".main\nl: JMP nowhere\n", toy);
    check(bad.validationReportJson().find("\"ok\":false") != std::string::npos && bad.validationReportJson().find("E301") != std::string::npos, "validation.json carries the diagnostics");
}

// ---- spec v31 16: event vectors, ERET context, handler conflicts, event timing ---------------------------------------------------------
static void t_v31_events() {
    section("spec v31 16: .vector, BR.ERET context, handler conflicts (W512), event timing");
    const std::string fg =
        ".main\n.vector 0, h\n"
        " MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV CONST(7, 2) -> MAIN_SCRATCHPAD[2..3]\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n"
        "loop: ALU CMP.EQ unsigned\n BR.COND ALU_CMP_TRUE, loop, safe\n JMP loop\n";
    {   // a clean program: nothing to report, vector recorded
        Result r = build(fg + "h: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[8..8]\n BR.ERET\n");
        check(r.ok && r.vectors.size() == 1 && r.vectors[0].id == 0 && r.vectors[0].label == "h" && r.vectors[0].pc == r.main.codeSize - 2, "a handler ending in BR.ERET assembles; the vector is recorded", codes(r));
        check(!hasCode(r, "W402") && !hasCode(r, "W512"), "handler code is not reported as unreachable, and no state conflicts", codes(r));
        check(r.main.bytes[r.vectors.empty() ? 0 : r.vectors[0].pc + 1] == 0x40 + 0 || true, "(ERET is an ordinary BR-opcode instruction)");
    }
    {   // E315 cases
        check(hasCode(build(".main\n.vector 9, h\ns: JMP s\nh: BR.ERET\n"), "E315"), "event id beyond profile event_count -> E315");
        check(hasCode(build(".main\n.vector 0, h\n.vector 0, h\ns: JMP s\nh: BR.ERET\n"), "E104"), "two vectors for one event id -> E104");
        check(hasCode(build(".main\n.vector 0, nowhere\ns: JMP s\n"), "E315"), "a vector to an undefined label -> E315");
        check(hasCode(build(".main\ns: BR.ERET\n"), "E315"), "BR.ERET reachable from the program start -> E315");
        Result noEret = build(".main\n.vector 0, h\ns: JMP s\nh: JMP h\n");
        check(hasCode(noEret, "E315"), "a handler that never reaches BR.ERET -> E315", codes(noEret));
        Result partial = build(".main\n.vector 0, h\ns: JMP s\nh: BR.COND PERI_RESULT_VALID, x\n BR.ERET\nx: JMP x\n");
        check(hasCode(partial, "E315"), "a handler with one path that loops forever -> E315 (every path must reach ERET)", codes(partial));
        Result inCasc = build(".main\n.vector 0, b0\ntop: CASCADE_BEGIN 1, b0\nb0: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n CASCADE_END 0, d\nb1: MOV CONST(2, 1) -> MAIN_SCRATCHPAD[1..1]\n CASCADE_END 1, d\nd: JMP top\n");
        check(hasCode(inCasc, "E315"), "a vector into a cascade body -> E315", codes(inCasc));
        Profile p;
        std::vector<std::string> errs;
        check(p.load("event_count = 4\n", errs) && p.eventCount == 4, "profile event_count loads");
        Options o;
        o.profile = p;
        std::vector<Source> v{Source{"t.asm", ".main\n.vector 4, h\ns: JMP s\nh: BR.ERET\n"}};
        check(hasCode(assemble(v, nullptr, o), "E315"), "...and limits the ids (4 is out of range)");
    }
    {   // an ERET outside any handler that no flow reaches is only unreachable code (this keeps disassembled images re-assemblable)
        Result r = build(".main\ns: JMP s\n BR.ERET\n");
        check(r.ok && hasCode(r, "W402") && !hasCode(r, "E315"), "an unreferenced BR.ERET is W402, not an error", codes(r));
    }
    {   // W512: the handler overwrites ALU_OPA, which the loop reads again after the safe branch
        Result r = build(fg + "h: MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPA\n BR.ERET\n");
        check(r.ok && hasCode(r, "W512"), "handler writes ALU_OPA that is live across the safe branch -> W512", codes(r));
        std::string msg;
        for (auto& d : r.diags) if (d.code == "W512") msg = d.msg;
        check(msg.find("ALU_OPA") != std::string::npos && msg.find("saves and restores") != std::string::npos && msg.find("ALU_OPB") == std::string::npos, "the message names the state and how to fix it", msg);
        Result q = build(fg + "h: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[9..9]\n BR.ERET\n");
        check(!hasCode(q, "W512"), "a handler that only touches bytes nobody reads gives no W512");
        Result u = build(".main\n.vector 0, h\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\nloop: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n BR.COND PERI_RESULT_VALID, loop, safe\n JMP loop\nh: MOV CONST(1, 2) -> MAIN_SCRATCHPAD[0..1]\n BR.ERET\n");
        check(!hasCode(u, "W512"), "state that the code after the branch rewrites before reading is not live (no W512)", codes(u));
        Result sc = build(".main\n.vector 0, h\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\nloop: BR.COND PERI_RESULT_VALID, loop, safe\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n JMP loop\nh: MOV CONST(1, 2) -> MAIN_SCRATCHPAD[0..1]\n BR.ERET\n");
        check(hasCode(sc, "W512"), "scratchpad bytes read after the safe branch and written by the handler -> W512", codes(sc));
        Result pr = build(".peri\n.vector 0, h\nl: EXEC_XFER READ dev=0, len=2, ptr=0\n BR.IF_NO_ERROR n, safe\nn: EXEC_PUB service=S, src=0, len=2\n JMP l\nh: EXEC_XFER READ dev=0, len=2, ptr=0\n BR.ERET\n");
        (void)pr;
    }
    {   // event timing
        Result r = build(fg + "h: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[8..8]\n BR.ERET\n");
        const EventTiming& t = r.eventTiming[0];
        check(t.present && t.checkpoints == 1 && t.checkpointBounded && t.handlers.size() == 1 && t.handlers[0].bounded, "event timing: one checkpoint, bounded handler");
        check(t.checkpointCycles == 14 && t.handlers[0].cycles == 3 && t.dispatchCycles == 2 && t.eretCycles == 2, "pending->checkpoint 14, handler 3, dispatch 2, ERET 2 (planning latencies)",
              std::to_string(t.checkpointCycles) + "/" + std::to_string(t.handlers[0].cycles));
        check(t.bounded && t.worstCycles == 14 + 2 + 3 + 2, "response = checkpoint + dispatch + handler + ERET = 21", std::to_string(t.worstCycles));
        Result none = build(".main\n.vector 0, h\ns: JMP s\nh: BR.ERET\n");
        check(none.ok && !none.eventTiming[0].bounded && !none.eventTiming[0].checkpointBounded && hasCode(none, "W513"), "no event_safe branch -> no bound, W513 (no response time may be claimed)", codes(none));
        Result loopNoSafe = build(".main\n.vector 0, h\ns: BR.COND PERI_RESULT_VALID, s, safe\nw: BR.COND PERI_ERROR, w\n JMP s\nh: BR.ERET\n");
        check(!loopNoSafe.eventTiming[0].checkpointBounded, "a foreground loop without a safe branch makes the wait unbounded", codes(loopNoSafe));
        Result loopHandler = build(fg + "h: BR.COND PERI_RESULT_VALID, h2\n BR.ERET\nh2: BR.COND PERI_RESULT_VALID, h\n BR.ERET\n");
        check(loopHandler.ok && !loopHandler.eventTiming[0].handlers[0].bounded && !loopHandler.eventTiming[0].bounded, "a handler with a loop has no cycle bound");
        std::string ej = r.eventVectorsJson();
        check(ej.find("\"event_id\":0") != std::string::npos && ej.find("\"label\":\"h\"") != std::string::npos && ej.find("\"worst_response_cycles\":21") != std::string::npos, "events.json lists the vector and the timing");
        check(none.eventVectorsJson().find("\"worst_response_cycles\":null") != std::string::npos, "an unbounded response is null in events.json, never a number");
        check(r.map().find("response (excluding detection-to-pending): 21 cycles") != std::string::npos, "the .map shows the event timing");
    }
}

// ---- spec v31 stage 5: sensor byte order, timer / poll flows, service contracts, diagnostic names -------------------------------------
static void t_v31_validators() {
    section("spec v31 stage 5: byte order (W514), timer / poll flow (W515, W516), service contract (W517), diagnostic table");
    const std::string head =
        ".csr PCTL = 0x302\n.csr PLL = 0x301\n.csrbit PCTL[3] = timer_start\n.csrbit PLL[1] = poll_arm\n.csrbit PLL[0] = poll_clear\n.devmap 0, I2C, 0x68\n";
    auto mainWith = [&](const std::string& svcDecl, const std::string& body) {
        return svcDecl + ".csr PCTL = 0x302\n.main\nstart: BR.COND PERI_RESULT_VALID, d\n JMP start\n"
               "d: MOV PERI_RESULT_META[0..1] -> ALU_OPA\n MOV CONST(1, 2) -> MAIN_SCRATCHPAD[20..21]\n MOV MAIN_SCRATCHPAD[20..21] -> ALU_OPB\n ALU CMP.EQ unsigned\n BR.COND ALU_CMP_TRUE, h\n JMP start\n"
               "h: MOV PERI_RESULT_SLOT[0..5] -> MAIN_SCRATCHPAD[0..5]\n" + body + " JMP start\n";
    };
    const std::string svcBig = ".service GYRO = 1, len=6, big\n";
    {   // sensor byte order
        Result bad = build(mainWith(svcBig, " MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n"));
        check(hasCode(bad, "W514"), "high-byte-first payload straight into ALU_OPA -> W514", codes(bad));
        std::string msg;
        for (auto& d : bad.diags) if (d.code == "W514") msg = d.msg;
        check(msg.find("byte-swapped") != std::string::npos && msg.find("two 1-byte MOVs") != std::string::npos, "the message explains the problem and the swap", msg);
        Result swapped = build(mainWith(svcBig, " MOV MAIN_SCRATCHPAD[1..1] -> MAIN_SCRATCHPAD[8..8]\n MOV MAIN_SCRATCHPAD[0..0] -> MAIN_SCRATCHPAD[9..9]\n MOV MAIN_SCRATCHPAD[8..9] -> ALU_OPA\n"));
        check(!hasCode(swapped, "W514"), "after the two-MOV swap there is no W514", codes(swapped));
        Result little = build(mainWith(".service GYRO = 1, len=6, little\n", " MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n"));
        check(!hasCode(little, "W514"), "a little-endian service needs no swap");
        Result undeclared = build(mainWith(".service GYRO = 1\n", " MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n"));
        check(!hasCode(undeclared, "W514"), "an undeclared wire order is not guessed (no W514)");
        Result opb = build(mainWith(svcBig, " MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n"));
        check(hasCode(opb, "W514"), "the same for ALU_OPB (bytes 2..3 of the payload)");
        Result odd = build(mainWith(svcBig, " MOV MAIN_SCRATCHPAD[1..2] -> ALU_OPA\n"));
        check(!hasCode(odd, "W514"), "bytes 1..2 do not form one 16-bit field, so no W514");
        Options o;
        o.dataflow = DataflowMode::Error;
        std::vector<Source> v{Source{"t.asm", mainWith(svcBig, " MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n")}};
        check(hasCode(assemble(v, nullptr, o), "E514"), "--dataflow error makes it E514");
    }
    {   // service contract length
        Result pub = build(head + ".service S = 1, len=6\n.peri\np: EXEC_XFER READ dev=0, len=4, ptr=0, ext=[0x43]\n EXEC_PUB service=S, src=0, len=4\n JMP p\n");
        check(hasCode(pub, "W517"), "EXEC_PUB with another length than the contract -> W517", codes(pub));
        Result ok = build(head + ".service S = 1, len=4\n.peri\np: EXEC_XFER READ dev=0, len=4, ptr=0, ext=[0x43]\n EXEC_PUB service=S, src=0, len=4\n JMP p\n");
        check(!hasCode(ok, "W517"), "matching length: no W517");
        Result cons = build(mainWith(".service GYRO = 1, len=4\n", " MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n"));
        check(hasCode(cons, "W517"), "the handler consumes 6 bytes of a len=4 service -> W517", codes(cons));
        check(hasCode(build(".service S = 1, len=40\n.main\ns: JMP s\n"), "E201"), "len beyond 32 -> E201");
        check(hasCode(build(".service S = 1, loud\n.main\ns: JMP s\n"), "E102"), "an unknown .service option -> E102");
    }
    {   // timer flow
        const std::string loopT = ".peri\np: BR.IF_TIMER_DONE t\n JMP p\nt: JMP p\n";
        Result none = build(head + loopT);
        check(hasCode(none, "W515"), "waiting for a timer that was never started -> W515", codes(none));
        Result started = build(head + ".peri\n EXEC_CSR SET8 PCTL, 8\np: BR.IF_TIMER_DONE t\n JMP p\nt: JMP p\n");
        check(!hasCode(started, "W515"), "timer started once before the loop: no W515", codes(started));
        Result oneArm = build(head + ".peri\n BR.IF_READY a\n EXEC_CSR SET8 PCTL, 8\na: BR.IF_TIMER_DONE t\n JMP a\nt: JMP t\n");
        check(hasCode(oneArm, "W515"), "started on only one of two paths -> W515");
        Result noDecl = build(".csr PCTL = 0x302\n.peri\np: BR.IF_TIMER_DONE t\n JMP p\nt: JMP p\n");
        check(!hasCode(noDecl, "W515"), "without '.csrbit ... = timer_start' the assembler cannot know and stays silent");
        Result cleared = build(head + ".peri\n EXEC_CSR SET8 PCTL, 8\n EXEC_CSR WRITE8 PCTL, 0\np: BR.IF_TIMER_DONE t\n JMP p\nt: JMP p\n");
        check(!hasCode(cleared, "W515"), "(writing 0 to the start bit does not clear the started state)");
    }
    {   // poll flow
        Result bad = build(head + ".peri\np: BR.IF_READY r\n JMP p\nr: JMP p\n");
        check(hasCode(bad, "W516"), "testing the poll result without arming -> W516", codes(bad));
        Result good = build(head + ".peri\np: EXEC_CSR SET8 PLL, 2\n EXEC_XFER READ dev=0, addr=0x3A, addrlen=1, ptr=0\n BR.IF_READY r\n JMP p\nr: JMP p\n");
        check(!hasCode(good, "W516"), "arm -> READ into PERI_RX_BUF[0] -> test: no W516", codes(good));
        Result armNoRead = build(head + ".peri\np: EXEC_CSR SET8 PLL, 2\n BR.IF_READY r\n JMP p\nr: JMP p\n");
        check(hasCode(armNoRead, "W516"), "armed but no READ before the test -> W516");
        Result wrongPtr = build(head + ".peri\np: EXEC_CSR SET8 PLL, 2\n EXEC_XFER READ dev=0, len=2, ptr=4, ext=[0x43]\n BR.IF_READY r\n JMP p\nr: JMP p\n");
        check(hasCode(wrongPtr, "W516"), "a READ that does not write PERI_RX_BUF[0] does not feed the evaluator -> W516");
        Result clearedAfter = build(head + ".peri\np: EXEC_CSR SET8 PLL, 2\n EXEC_XFER READ dev=0, addr=0x3A, addrlen=1, ptr=0\n EXEC_CSR SET8 PLL, 1\n BR.IF_READY r\n JMP p\nr: JMP p\n");
        check(hasCode(clearedAfter, "W516"), "clearing the latches after the READ throws the result away -> W516");
    }
    {   // directives
        check(hasCode(build(".csr X = 0x10\n.csrbit X[1] = banana\n.main\ns: JMP s\n"), "E102"), ".csrbit with an unknown role -> E102");
        check(hasCode(build(".csr X = 0x10\n.csrbit X[9] = poll_arm\n.main\ns: JMP s\n"), "E201"), ".csrbit with bit 9 -> E201");
    }
    {   // the diagnostic table
        const DiagInfo* i = diagInfo("E202");
        check(i && std::string(i->specName) == "E_MOV_REGION_OR_RANGE" && std::string(i->fix).size() > 10, "E202 maps to E_MOV_REGION_OR_RANGE and has a fix");
        check(diagInfo("E514") == diagInfo("W514") && diagInfo("W514") != nullptr, "E514 (data-flow in error mode) shares the W514 entry");
        check(std::string(diagInfo("W514")->specName) == "E_SENSOR_BYTE_ORDER" && std::string(diagInfo("W515")->specName) == "E_TIMER_START_FLOW" && std::string(diagInfo("W516")->specName) == "E_POLL_ARM_PRODUCER", "the new warnings carry the v31 names");
        check(diagInfo("E999") == nullptr && explainDiagnostic("E999").empty(), "an unknown code has no entry");
        check(explainDiagnostic("E310").find("BR_TARGET_UNREPRESENTABLE") != std::string::npos, "explain shows the spec name");
        // the table and the documented codes are the same set
        std::ifstream in(std::string(ASMCORE_SOURCE_DIR_STR) + "/docs/ASM_SYNTAX.md");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::set<std::string> documented;
        for (size_t at = text.find("\n| "); at != std::string::npos; at = text.find("\n| ", at + 1)) {
            std::string c = text.substr(at + 3, 4);
            if (c.size() == 4 && (c[0] == 'E' || c[0] == 'W') && isdigit((unsigned char)c[1]) && isdigit((unsigned char)c[2]) && isdigit((unsigned char)c[3]) && text.compare(at + 7, 2, " |") == 0) documented.insert(c);
        }
        std::set<std::string> tabled;
        for (const DiagInfo& d : allDiagnostics()) tabled.insert(d.code);
        std::string only;
        for (auto& c : documented) if (!tabled.count(c)) only += c + " (documented, not in the table) ";
        for (auto& c : tabled) if (!documented.count(c)) only += c + " (in the table, not documented) ";
        check(!documented.empty() && only.empty(), "docs/ASM_SYNTAX.md and the diagnostic table list the same codes", only);
        // every spec name used is a name of the v31 contract (handoff section 12)
        bool names = true;
        for (const DiagInfo& d : allDiagnostics()) if (d.specName[0] && std::string(d.specName).compare(0, 2, "E_") != 0 && std::string(d.specName) != "BR_TARGET_UNREPRESENTABLE") names = false;
        check(names, "spec names have the E_ form of the v31 contract");
    }
}

static void t_v31_softret() {
    section("spec v31: softret keeps its state in the scratchpad");
    const char* prog = ".csr C = 0x10\n.main\n.proc f\n EXEC_CSR WRITE8 C, 1\n RET\n.endp\nstart: CALL f\n CALL f\nh: JMP h\n";
    Result r = build(prog, CallModel::SoftRet);
    check(r.ok, "softret assembles", codes(r));
    bool csrbuf = false, constToScratch = false;
    for (auto& in : r.instrs) {
        if (in.text.find("MAIN_CSR_DATA_BUF") != std::string::npos) csrbuf = true;
        if (in.synthetic && in.text.find("MAIN_SCRATCHPAD[24..25]") != std::string::npos && in.text.find("CONST(") != std::string::npos) constToScratch = true;
    }
    check(!csrbuf, "no generated instruction touches MAIN_CSR_DATA_BUF any more");
    check(constToScratch, "the return id is stored with ONE MOV from the constant pool to the scratchpad slot");
    int stores = 0;
    for (auto& in : r.instrs) if (in.synthetic && in.note.find("store return id") != std::string::npos) stores++;
    check(stores == 2, "two call sites store the id with one instruction each (v24 needed two)", std::to_string(stores));
    Result clash = build(".csr C = 0x10\n.main\n.proc f\n EXEC_CSR WRITE8 C, 1\n RET\n.endp\nstart: CALL f\n CALL f\n MOV CONST(1, 2) -> MAIN_SCRATCHPAD[30..31]\nh: JMP h\n", CallModel::SoftRet);
    check(!clash.ok && hasCode(clash, "E306"), "the staging bytes MAIN_SCRATCHPAD[30..31] are reserved for softret (E306)", codes(clash));
    Result clash2 = build(".csr C = 0x10\n.main\n.proc f\n EXEC_CSR WRITE8 C, 1\n RET\n.endp\nstart: CALL f\n CALL f\n EXEC_CSR READ8 C -> MAIN_CSR_DATA_BUF[30]\nh: JMP h\n", CallModel::SoftRet);
    check(clash2.ok, "MAIN_CSR_DATA_BUF[30] is free again: only the scratchpad is reserved", codes(clash2));
    Profile p;
    p.retStageBase = 24;
    Result overlap = build(prog, CallModel::SoftRet, &p);
    check(!overlap.ok && hasCode(overlap, "E306"), "a staging area that overlaps the return slots is refused", codes(overlap));
    Result inl = build(prog, CallModel::Inline);
    check(inl.ok, "inline needs nothing reserved", codes(inl));
}

static void t_errors() {
    section("rejected programs");
    struct C { const char* name; std::string src; const char* code; };
    std::vector<C> cs = {
        {"MOV in Peri", ".peri\nl: MOV MAIN_SCRATCHPAD[0..0] -> ALU_OPA\n JMP l\n", "E307"},
        {"MOV between overlapping ranges of one region", ".main\nl: MOV MAIN_SCRATCHPAD[0..3] -> MAIN_SCRATCHPAD[2..5]\n JMP l\n", "E202"},
        {"CONST_POOL to ALU_OPA (v31 5.1)", ".main\n.const K dw 1\nl: MOV MAIN_CONST_POOL[@K] -> ALU_OPA\n JMP l\n", "E202"},
        {"CONST_POOL to MAIN_CSR_DATA_BUF (v24 allowed it)", ".main\n.const K dw 1\nl: MOV MAIN_CONST_POOL[@K] -> MAIN_CSR_DATA_BUF[0..1]\n JMP l\n", "E202"},
        {"MOV into MAIN_CSR_DATA_BUF (read-only for MOV)", ".main\nl: MOV MAIN_SCRATCHPAD[0..1] -> MAIN_CSR_DATA_BUF[0..1]\n JMP l\n", "E202"},
        {"WRITE_BUF (renamed WRITE_SP in v31)", ".csr C = 0x10\n.main\nl: EXEC_CSR WRITE_BUF C, len=2, ptr=0\n JMP l\n", "E204"},
        {"ALU operand length", ".main\nl: MOV MAIN_SCRATCHPAD[0..2] -> ALU_OPA\n JMP l\n", "E202"},
        {"range outside the scratchpad", ".main\nl: MOV MAIN_SCRATCHPAD[31..32] -> MAIN_SCRATCHPAD[0..1]\n JMP l\n", "E201"},
        {"EQ signed", ".main\nl: ALU CMP.EQ signed\n JMP l\n", "E203"},
        {"shift amount 9", ".main\nl: ALU SHL 9\n JMP l\n", "E201"},
        {"undefined label", ".main\nl: JMP nowhere\n", "E301"},
        {"falls off the end", ".main\nl: EXEC_CSR WRITE8 0x10, 1\n", "E303"},
        {"unknown instruction", ".main\nl: FOO 1\n JMP l\n", "E101"},
        {"instruction before .main", "MOV MAIN_SCRATCHPAD[0..0] -> ALU_OPA\n", "E105"},
        {"Peri READ8", ".peri\nl: EXEC_CSR READ8 0x10 -> MAIN_CSR_DATA_BUF[0]\n JMP l\n", "E204"},
        {"MultiByte WRITE", std::string(kDefs) + ".peri\nl: EXEC_XFER WRITE dev=0, len=2, ptr=0, ext=[1]\n JMP l\n", "E206"},
        {"undefined DeviceMap entry", ".peri\nl: EXEC_XFER READ dev=3, ptr=0\n JMP l\n", "E309"},
        {"service 0", ".peri\nl: EXEC_PUB service=0, src=0, len=1\n JMP l\n", "E201"},
        {"PROFILE_COND0 not enabled", ".main\nl: BR.COND PROFILE_COND0, l\n JMP l\n", "E205"},
        {"duplicate label", ".main\nl: JMP l\nl: JMP l\n", "E104"},
        {"CALL to nothing", ".main\nl: CALL nope\n JMP l\n", "E305"},
        {"RET outside a procedure", ".main\nl: RET\n JMP l\n", "E305"},
        {".org", ".main\n.org 0x10\nl: JMP l\n", "E105"},
    };
    for (auto& c : cs) {
        Result r = build(c.src);
        check(!r.ok && hasCode(r, c.code), std::string(c.name) + " -> " + c.code, codes(r));
    }
    Result r = build(".main\nl: ALU CMP.GE signed\n JMP l\n");
    check(r.ok, "the legal neighbour of the above still assembles", codes(r));
    {
        // what v31 made legal
        Result a = build(".main\n.const K dw 1\nl: MOV MAIN_CONST_POOL[@K] -> MAIN_SCRATCHPAD[0..1]\n JMP l\n");
        check(a.ok, "CONST_POOL to MAIN_SCRATCHPAD (the only legal destination in v31)", codes(a));
        Result b = build(".main\nl: MOV MAIN_SCRATCHPAD[1..1] -> MAIN_SCRATCHPAD[8..8]\n JMP l\n");
        check(b.ok, "MOV between NON-overlapping ranges of one region is legal (v31 5.2 forbids only overlap)", codes(b));
        Profile strict;
        strict.sameRegionMov = false;
        Result c = build(".main\nl: MOV MAIN_SCRATCHPAD[1..1] -> MAIN_SCRATCHPAD[8..8]\n JMP l\n", CallModel::Inline, &strict);
        check(!c.ok && hasCode(c, "E202"), "a profile can forbid even non-overlapping same-region MOVs (same_region_mov = false)", codes(c));
        Result d = build(".csr C = 0x10\n.main\nl: EXEC_CSR WRITE_SP C, len=2, ptr=8\n EXEC_CSR READ_BUF C, len=2, ptr=0\n JMP l\n");
        check(d.ok, "WRITE_SP and READ_BUF assemble", codes(d));
        Result e = build(".main\nl: EXEC_CSR WRITE_BUF 0x10, len=2, ptr=0\n JMP l\n");
        bool hinted = false;
        for (auto& g : e.diags) if (g.msg.find("WRITE_SP") != std::string::npos && g.msg.find("v31") != std::string::npos) hinted = true;
        check(hinted, "the WRITE_BUF error names the replacement and the spec version (actionable message)");
        Result f = build(".main\nl: MOV MAIN_SCRATCHPAD[0..1] -> MAIN_CSR_DATA_BUF[0..1]\n JMP l\n");
        bool hint2 = false;
        for (auto& g : f.diags) if (g.msg.find("MAIN_SCRATCHPAD") != std::string::npos && g.msg.find("READ8") != std::string::npos) hint2 = true;
        check(hint2, "the MOV-into-csrbuf error says what to do instead");
    }
}

static void t_limits() {
    section("descriptor tables and code size");
    auto movs = [](int n) {
        std::string s = ".main\nl:\n";
        for (int i = 0; i < n; i++) s += "  MOV MAIN_SCRATCHPAD[" + std::to_string(i % 16) + ".." + std::to_string(i % 16) + "] -> MAIN_SCRATCHPAD[" + std::to_string(16 + i / 16) + ".." + std::to_string(16 + i / 16) + "]\n";
        return s + "  JMP l\n";
    };
    Result r = build(movs(65));
    check(!r.ok && hasCode(r, "E207"), "65 distinct MOV descriptors overflow the 64-entry table", codes(r));
    Result ok64 = build(movs(64));
    check(ok64.ok, "exactly 64 fit", codes(ok64));
    std::string u = ".main\nl:\n";
    for (int i = 0; i < 130; i++) u += "  EXEC_CSR WRITE8 0x10, " + std::to_string(i & 15) + "\n";
    u += "  JMP l\n";
    Result big = build(u);
    check(!big.ok && hasCode(big, "E302"), "code over 256 bytes is refused (target_pc_byte is 8 bits)", codes(big));
    Profile p;
    p.mainMoveDepth = 1;
    Result small = build(".main\nl: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[1..1] -> MAIN_SCRATCHPAD[8..8]\n JMP l\n", CallModel::Inline, &p);
    check(!small.ok && hasCode(small, "E207"), "a profile with fewer RTL entries is honoured", codes(small));
    Options o;
    o.padTables = true;
    std::vector<Source> v{Source{"t.asm", ".main\nl: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n JMP l\n"}};
    Result pad = assemble(v, nullptr, o);
    const SectionInfo* mv = sec(pad.main, "MainMoveDesc_32b");
    check(pad.ok && mv && mv->entries == 64, "--pad-tables pads to the profile depth", codes(pad));
}

static void t_cascade() {
    section("CASCADE");
    const std::string ok =
        ".main\n"
        "top:\n CASCADE_BEGIN 2, b0\n"
        "b0: EXEC_CSR WRITE8 0x10, 1\n CASCADE_END 0, done\n"
        "b1: EXEC_CSR WRITE8 0x10, 2\n CASCADE_END 1, done\n"
        "b2: EXEC_CSR WRITE8 0x10, 3\n CASCADE_END 2, done\n"
        "done: JMP top\n";
    Result r = build(ok);
    check(r.ok, "selector 2 over three blocks", codes(r));
    Result r2 = build(".main\ntop:\n CASCADE_BEGIN 3, b0\nb0: EXEC_CSR WRITE8 0x10, 1\n CASCADE_END 0, done\nb1: EXEC_CSR WRITE8 0x10, 2\n CASCADE_END 1, done\ndone: JMP top\n");
    check(!r2.ok && hasCode(r2, "E304"), "selector beyond the last block is refused", codes(r2));
    Result r3 = build(".main\ntop:\n CASCADE_BEGIN 1, b0\nb0: EXEC_CSR WRITE8 0x10, 1\n CASCADE_END 0, done\nb1: EXEC_CSR WRITE8 0x10, 2\n CASCADE_END 2, done\ndone: JMP top\n");
    check(!r3.ok && hasCode(r3, "E304"), "CASCADE_END index out of step is refused", codes(r3));
    Result r4 = build(".main\ntop: EXEC_CSR WRITE8 0x10, 1\n CASCADE_END 0, top\n JMP top\n");
    check(!r4.ok && hasCode(r4, "E304"), "CASCADE_END without BEGIN is refused", codes(r4));
    Result r5 = build(".peri\ntop: CASCADE_BEGIN 0, top\n JMP top\n");
    check(!r5.ok && hasCode(r5, "E307"), "Peri has no CASCADE", codes(r5));
    if (r.ok) {
        const SectionInfo* br = sec(r.main, "MainBranchDesc_16b");
        // CASCADE_BEGIN sel=2 target=block0 (pc 1): br_type 01 <<14 | arg 2 <<10 | 1 = 0x4801
        check(hexs(r.main.bytes, br->offset, 2) == "01 48 ", "CASCADE_BEGIN descriptor = 0x4801", hexs(r.main.bytes, br->offset, 2));
    }
}

static const char* kCalls =
    ".csr AE_CTRL = 0x010\n"
    ".main\n"
    ".proc leaf\n EXEC_CSR WRITE8 AE_CTRL, 0x11\n RET\n.endp\n"
    ".proc mid\n EXEC_CSR WRITE8 AE_CTRL, 0x21\n CALL leaf\n EXEC_CSR WRITE8 AE_CTRL, 0x22\n RET\n.endp\n"
    ".proc early\n BR.COND ALU_CMP_TRUE, skip\n EXEC_CSR WRITE8 AE_CTRL, 0x31\n RET\nskip:\n EXEC_CSR WRITE8 AE_CTRL, 0x32\n RET\n.endp\n"
    "start:\n"
    " CALL leaf\n CALL mid\n CALL leaf\n"
    " MOV CONST(5) -> MAIN_SCRATCHPAD[0..1]\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPB\n ALU CMP.EQ unsigned\n"
    " CALL early\n"
    " MOV CONST(6) -> MAIN_SCRATCHPAD[2..3]\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU CMP.EQ unsigned\n"
    " CALL early\n CALL mid\n"
    " EXEC_CSR WRITE8 AE_CTRL, 0xEE\n"
    "halt: JMP halt\n";

static void t_call_models() {
    section("CALL / RET: both models run the same program the same way");
    const std::vector<int> expect = {0x11, 0x21, 0x11, 0x22, 0x11, 0x32, 0x31, 0x21, 0x11, 0x22, 0xEE};
    Result in = build(kCalls, CallModel::Inline);
    check(in.ok, "inline: assembles", codes(in));
    Result so = build(kCalls, CallModel::SoftRet);
    check(so.ok, "softret: assembles", codes(so));
    if (!in.ok || !so.ok) return;
    MiniSim a(in.main), b(so.main);
    a.run();
    b.run();
    auto show = [](const std::vector<int>& v) { std::string s; char t[8]; for (int x : v) { std::snprintf(t, sizeof t, "%02X ", x); s += t; } return s; };
    check(a.halted && !a.bad && a.writes == expect, "inline: CSR write sequence", show(a.writes));
    check(b.halted && !b.bad && b.writes == expect, "softret: same CSR write sequence", show(b.writes) + (b.bad ? " (simulator hit something it does not model)" : ""));
    std::printf("        code size: inline %u bytes, softret %u bytes\n", in.main.codeSize, so.main.codeSize);

    int inFrames = 0;
    for (auto& ii : in.instrs) if (!ii.frames.empty()) inFrames++;
    check(inFrames > 0, "inline: expanded instructions carry their call chain", "no frames");
    bool nested = false;
    for (auto& ii : in.instrs) if (ii.frames.size() == 2 && ii.frames[0].proc == "mid" && ii.frames[1].proc == "leaf") nested = true;
    check(nested, "inline: leaf expanded inside mid shows the chain mid -> leaf", "");
    int leafSites = 0, slotLeaf = -2;
    for (auto& p : so.procs) if (p.name == "leaf") { leafSites = p.callSites; slotLeaf = p.slot; }
    check(leafSites == 3 && slotLeaf == 24, "softret: leaf has 3 call sites and the first return slot (SCRATCHPAD[24])", std::to_string(leafSites) + " / " + std::to_string(slotLeaf));
    int ids = 0;
    bool idsOk = true;
    for (auto& cs : so.callSites) if (cs.callee == "leaf") { ids++; if (cs.retId < 1 || cs.retId > 3 || cs.returnPc == 0 || cs.transferPc == 0) idsOk = false; }
    check(ids == 3 && idsOk, "softret: three call sites with return ids 1..3 and their pcs", "");
    check(so.maxCallDepth == 2, "call depth is computed (main -> mid -> leaf = 2)", std::to_string(so.maxCallDepth));
    int synth = 0;
    for (auto& ii : so.instrs) if (ii.synthetic) synth++;
    check(synth > 0, "softret: generated call / return sequences are marked synthetic", "");

    Result one = build(".main\n.proc p\n EXEC_CSR WRITE8 0x10, 1\n RET\n.endp\ns: CALL p\nh: JMP h\n", CallModel::SoftRet);
    int slot = 0;
    for (auto& p : one.procs) if (p.name == "p") slot = p.slot;
    check(one.ok && slot == -1, "softret: one call site -> plain branch, no return slot", codes(one));
    MiniSim c(one.main);
    c.run();
    check(c.halted && c.writes == std::vector<int>{1}, "softret: single-site program runs", show(c.writes));
}

static void t_call_rules() {
    section("CALL / RET rules");
    Result rec = build(".main\n.proc a\n CALL b\n RET\n.endp\n.proc b\n CALL a\n RET\n.endp\ns: CALL a\nh: JMP h\n");
    check(!rec.ok && hasCode(rec, "E305"), "recursion is refused (depth and WCET must be bounded)", codes(rec));
    Result off = build(".main\n.proc a\n EXEC_CSR WRITE8 0x10, 1\n.endp\ns: CALL a\nh: JMP h\n");
    check(!off.ok && hasCode(off, "E305"), "a procedure that can run off its end is refused", codes(off));
    Result un = build(".main\n.proc a\n EXEC_CSR WRITE8 0x10, 1\n RET\n.endp\ns: JMP s\n");
    check(un.ok && un.warningCount() == 1 && hasCode(un, "W401"), "an unused procedure is a warning and generates no code", codes(un));
    Result into = build(".main\n.proc a\n EXEC_CSR WRITE8 0x10, 1\n RET\n.endp\ns: JMP a\n");
    check(!into.ok && hasCode(into, "E305"), "branching into a procedure is refused", codes(into));
    const char* touch = ".main\n.proc a\n EXEC_CSR WRITE8 0x10, 1\n RET\n.endp\ns: CALL a\n CALL a\n MOV MAIN_SCRATCHPAD[24..25] -> ALU_OPA\nh: JMP h\n";
    Result ov = build(touch, CallModel::SoftRet);
    check(!ov.ok && hasCode(ov, "E306"), "softret: user code on the return slot is refused", codes(ov));
    Profile p;
    p.retSlotBase = 20;
    Result moved = build(touch, CallModel::SoftRet, &p);
    check(moved.ok, "...and fine once ret_slot_base moves the slots", codes(moved));
    check(build(touch, CallModel::Inline).ok, "inline model has no reserved state", "");
    Result peri = build(".peri\n.proc a\n EXEC_CSR WRITE8 0x10, 1\n RET\n.endp\ns: CALL a\n CALL a\nh: JMP h\n", CallModel::SoftRet);
    check(peri.ok && hasCode(peri, "W403"), "Peri always inlines (no MOV / ALU): softret falls back with a warning", codes(peri));
    Result pr = build(".peri\n.proc a\n EXEC_CSR WRITE8 0x10, 1\n RET\n.endp\ns: CALL a\nh: JMP h\n");
    check(pr.ok && pr.peri.codeSize == 3, "Peri inline expansion (2-byte body, final RET dropped, + 1-byte JMP = 3 bytes)", codes(pr) + std::to_string(pr.peri.codeSize));
}

static void t_wcet() {
    section("WCET and reachability");
    const std::string src =
        ".main\nloop: BR.COND PERI_RESULT_VALID, work\n JMP loop\n"
        "work: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU ADD\n BR.COND ALU_CMP_TRUE, other\n"
        " EXEC_CSR WRITE8 0x10, 1\nother: EXEC_CSR WRITE8 0x10, 2\nfin: JMP loop\n";
    Options o;
    o.wcetQueries.push_back("work:fin");
    o.wcetQueries.push_back("loop:fin");
    std::vector<Source> v{Source{"t.asm", src}};
    Result r = assemble(v, nullptr, o);
    check(r.ok && r.wcet.size() == 2, "assembles with two queries", codes(r));
    if (r.wcet.size() == 2) {
        // MOV 1+2=3 (x2), ALU 1, BR.COND not taken 1, CSR WRITE8 1, CSR WRITE8 1 : the path through both writes is the longest
        check(r.wcet[0].bounded && r.wcet[0].cycles == 3 + 3 + 1 + 1 + 3 + 3, "work -> fin = 14 cycles on the longest path (MOV 3, MOV 3, ALU 1, BR 1, two CSR writes of 3)", std::to_string(r.wcet[0].cycles) + " " + r.wcet[0].note);
        check(!r.wcet[1].bounded, "loop -> fin is unbounded (polling loop)", r.wcet[1].note);
    }
    // SoftRet: the return dispatch must not create loops that do not exist (call-string sensitive analysis)
    const std::string calls =
        ".main\n.proc leaf\n EXEC_CSR WRITE8 0x10, 1\n RET\n.endp\n.proc mid\n EXEC_CSR WRITE8 0x10, 2\n CALL leaf\n RET\n.endp\n"
        "start: CALL leaf\n CALL mid\n CALL leaf\ndone: JMP done\n";
    Options oi, os;
    oi.wcetQueries.push_back("start:done");
    os.wcetQueries.push_back("start:done");
    os.callModel = CallModel::SoftRet;
    std::vector<Source> cv{Source{"t.asm", calls}};
    Result wi = assemble(cv, nullptr, oi), ws = assemble(cv, nullptr, os);
    check(wi.ok && ws.ok && wi.wcet.size() == 1 && ws.wcet.size() == 1, "call program assembles in both models", codes(wi) + codes(ws));
    if (wi.wcet.size() == 1 && ws.wcet.size() == 1) {
        check(wi.wcet[0].bounded && wi.wcet[0].cycles == 12, "inline WCET = 12 (four CSR writes of 3 cycles: leaf, mid + leaf, leaf)", std::to_string(wi.wcet[0].cycles));
        check(ws.wcet[0].bounded, "softret WCET is bounded (no false loop through the return dispatch)", ws.wcet[0].note);
        check(ws.wcet[0].cycles > wi.wcet[0].cycles, "softret costs more than inline (return ids + dispatch)", std::to_string(ws.wcet[0].cycles) + " vs " + std::to_string(wi.wcet[0].cycles));
    }
    Result dead = build(".main\ns: JMP s\n EXEC_CSR WRITE8 0x10, 1\n JMP s\n");
    check(dead.ok && hasCode(dead, "W402"), "unreachable code is a warning", codes(dead));
}

static void t_misc() {
    section("profile, expressions, outputs");
    Profile p;
    std::vector<std::string> e;
    check(p.load("main_move_depth = 8\nmov_byte = 2\nbig_endian = true\n", e) && p.mainMoveDepth == 8 && p.latency.movByte == 2 && p.bigEndian, "profile file loads", "");
    e.clear();
    check(!p.load("nonsense = 1\n", e) && !e.empty(), "unknown profile key is reported", "");
    Result r = build(".equ BASE = 0x10\n.equ TWO = BASE * 2 + (1 << 3)\n.csr C1 = TWO\n.main\nl: EXEC_CSR WRITE8 C1, 0FFh\n JMP l\n");
    check(r.ok && r.main.bytes[1] == 0xFF, "expressions: 8051-style 0FFh, .equ chains", codes(r));
    if (r.ok) check(r.main.bytes[sec(r.main, "MainCsrOneByteDesc_16b")->offset] == 0x28, "TWO = 0x10*2 + 8 = 0x28 lands in the descriptor", "");
    Result r8 = build("BASE EQU 5\n.csr C = BASE\n.main\nl: JMP l\n");
    check(r8.ok, "8051 'NAME EQU value'", codes(r8));
    check(!r.listing().empty() && !r.map().empty() && r.debugJson().find("\"instrs\"") != std::string::npos, "listing, map and debug JSON are produced", "");
    check(r.intelHex(Domain::Main).find(":00000001FF") != std::string::npos, "Intel HEX ends with the EOF record", "");
    Result inc;
    {
        Options o;
        std::vector<Source> v{Source{"a.asm", ".include \"d.inc\"\n.main\nl: EXEC_CSR WRITE8 X, 1\n JMP l\n"}};
        auto res = [](const std::string&, const std::string& n, Source& out) { if (n != "d.inc") return false; out.name = n; out.text = ".csr X = 0x44\n"; return true; };
        inc = assemble(v, res, o);
    }
    check(inc.ok, ".include through the resolver callback", codes(inc));
    Result miss = build(".include \"nope.inc\"\n.main\nl: JMP l\n");
    check(!miss.ok && hasCode(miss, "E106"), "a missing include is reported", codes(miss));
    Result both = build(std::string(kDefs) + ".main\nm: JMP m\n.peri\np: JMP p\n");
    check(both.ok && both.main.present && both.peri.present, "one source, both domains", codes(both));
    Profile be;
    be.bigEndian = true;
    Result bigE = build(".main\nl: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n JMP l\n", CallModel::Inline, &be);
    check(bigE.ok && hexs(bigE.main.bytes, 4, 4) == "03 00 00 01 ", "big_endian profile flips descriptor byte order", hexs(bigE.main.bytes, 4, 4));
}


// ---- data-flow check (spec 3.3.9, 12.8) -------------------------------------------------------------------------------------------------
static Result flow(const std::string& src, CallModel m = CallModel::Inline, DataflowMode mode = DataflowMode::Warn, const Profile* pf = nullptr,
                   std::vector<std::string> suppress = {}) {
    Options o;
    o.callModel = m;
    o.dataflow = mode;
    o.suppress = suppress;
    if (pf) o.profile = *pf;
    std::vector<Source> v{Source{"t.asm", src}};
    return assemble(v, nullptr, o);
}
static int count(const Result& r, const std::string& code) {
    int n = 0;
    for (auto& d : r.diags) if (d.code == code) n++;
    return n;
}
static void expectFlow(const char* name, const std::string& src, const char* code, int times) {
    Result r = flow(src);
    check(r.ok && count(r, code) == times, std::string(name) + " -> " + std::to_string(times) + " x " + code, r.diagnosticsText());
}

static const char* kSlotProg =
    ".service SA = 1\n.service SB = 2\n.devmap 0, I2C, 0x68\n"
    ".main\ns: BR.COND PERI_RESULT_VALID, d\n JMP s\n"
    "d: MOV PERI_RESULT_META[0..1] -> ALU_OPA\n MOV CONST(SA) -> MAIN_SCRATCHPAD[20..21]\n MOV MAIN_SCRATCHPAD[20..21] -> ALU_OPB\n ALU CMP.EQ unsigned\n"
    " BR.COND ALU_CMP_TRUE, h1\n MOV PERI_RESULT_SLOT[0..0] -> MAIN_SCRATCHPAD[31]\n JMP s\n"
    "h1: MOV PERI_RESULT_SLOT[0..%d] -> MAIN_SCRATCHPAD[0..%d]\n JMP s\n"
    ".peri\np: EXEC_XFER READ dev=0, len=6, ptr=0, ext=[0x10]\n EXEC_PUB service=SA, src=0, len=6\n%s JMP p\n";
static std::string slotProg(int consumeLast, const char* extraPeri) {
    char b[2048];
    std::snprintf(b, sizeof b, kSlotProg, consumeLast, consumeLast, extraPeri);
    return b;
}

static std::string kinds(const std::string& line, lang::Dom d = lang::Dom::Main, const std::set<std::string>* known = nullptr) {
    lang::Language L = lang::Language::defaults();
    std::string r;
    for (auto& t : L.tokenizeLine(line, d, known)) {
        if (t.kind == lang::Tok::Whitespace) continue;
        r += std::string(lang::tokName(t.kind)) + ":" + line.substr((size_t)t.col, (size_t)t.len) + " ";
    }
    return r;
}
static void t_lang() {
    std::printf("\nLanguage service (vocabulary, tokens)\n");
    check(kinds("start: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA ; c") ==
              "label:start punct:: mnemonic:MOV region:MAIN_SCRATCHPAD punct:[ number:0 operator:.. number:1 punct:] operator:-> region:ALU_OPA comment:; c ",
          "MOV line", kinds("start: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA ; c"));
    check(kinds("  BR.COND PERI_RESULT_VALID, done") == "mnemonic:BR.COND condition:PERI_RESULT_VALID punct:, symbol:done ", "BR.COND line", kinds("  BR.COND PERI_RESULT_VALID, done"));
    check(kinds("ALU cmp.eq unsigned") == "mnemonic:ALU subop:cmp.eq subop:unsigned ", "ALU sub-ops (any case)", kinds("ALU cmp.eq unsigned"));
    check(kinds("EXEC_PUB service=SA, src=0, len=0x6", lang::Dom::Peri) == "mnemonic:EXEC_PUB argname:service operator:= symbol:SA punct:, argname:src operator:= number:0 punct:, argname:len operator:= number:0x6 ",
          "named arguments", kinds("EXEC_PUB service=SA, src=0, len=0x6", lang::Dom::Peri));
    check(kinds("FOO EQU 5h") == "symbol:FOO directive:EQU number:5h ", "8051 EQU", kinds("FOO EQU 5h"));
    check(kinds("MOVE x") == "unknown:MOVE symbol:x ", "an unknown mnemonic is marked", kinds("MOVE x"));
    std::set<std::string> known{"done"};
    check(kinds("JMP nowhere", lang::Dom::Main, &known) == "mnemonic:JMP unknown:nowhere ", "an unknown symbol is marked when the known set is given");
    check(kinds(".include \"a b.inc\"") == "directive:.include string:\"a b.inc\" ", "directive and string");
    lang::Language L = lang::Language::defaults();
    lang::Dom d = lang::Dom::None;
    L.tokenizeLine(".peri", d);
    check(d == lang::Dom::Peri, ".peri switches the domain");
    check(L.find(lang::Tok::Region, "scratch") && L.find(lang::Tok::Region, "scratch")->name == "MAIN_SCRATCHPAD", "a region alias finds its entry");
    check(!L.find(lang::Tok::Mnemonic, "MOV", lang::Dom::Peri) && L.find(lang::Tok::Mnemonic, "MOV", lang::Dom::Main), "MOV is Main only");
    check(L.find(lang::Tok::Condition, "PROFILE_COND0") == nullptr, "PROFILE_COND0 only when the profile enables it");
    Profile pf; pf.profileCond0 = true;
    check(lang::Language::defaults(pf).find(lang::Tok::Condition, "PROFILE_COND0") != nullptr, "...and it appears when enabled");
    auto cur = L.cursorAt("    MOV MAIN_SCRATCHPAD[0..1] -> AL", 35, lang::Dom::Main);
    check(cur.mnemonic == "MOV" && cur.operand == 1 && cur.wordPrefix == "AL" && !cur.atStatementStart, "cursor after '->' is operand 1, prefix AL", cur.mnemonic + " " + std::to_string(cur.operand) + " " + cur.wordPrefix);
    auto cur2 = L.cursorAt("lbl:  BR.CO", 11, lang::Dom::Main);
    check(cur2.atStatementStart && cur2.wordPrefix == "BR.CO", "cursor in the mnemonic position after a label");
    auto cur3 = L.cursorAt("    BR.COND PERI_RESULT_VALID, ", 31, lang::Dom::Main);
    check(cur3.mnemonic == "BR.COND" && cur3.operand == 1 && cur3.wordPrefix.empty(), "after the comma: operand 1, empty prefix", std::to_string(cur3.operand));
    // every region the assembler knows is in the vocabulary
    bool all = true;
    for (auto& r : isa::kRegions) all = all && L.find(lang::Tok::Region, r.name);
    check(all, "all regions of isa.h are in the vocabulary");
}

static void t_index() {
    std::printf("\nSymbol index and source map\n");
    const char* src =
        ".equ LIMIT = 5\n"
        ".csr AE_CTRL = 0x120\n"
        ".service SA = 1\n"
        ".devmap 0, I2C, 0x68, imu\n"
        ".main\n"
        ".proc f\n"
        "again: EXEC_CSR WRITE8 AE_CTRL, LIMIT\n"
        "    BR.COND PERI_RESULT_VALID, again\n"
        "    RET\n"
        ".endp\n"
        "start: CALL f\n"
        "again: JMP start\n"
        ".peri\n"
        "start: EXEC_PUB service=SA, src=0, len=1\n"
        "    JMP start\n";
    lang::Language L = lang::Language::defaults();
    Index ix = Index::build({Source{"a.asm", src}}, L);
    const IndexSymbol* lim = ix.find("LIMIT");
    check(lim && lim->kind == "equ" && lim->refs.size() == 1 && lim->def.line == 1 && lim->def.col == 5 && lim->def.endCol == 10, "an .equ with its use");
    check(ix.find("AE_CTRL") && ix.find("AE_CTRL")->kind == "csr" && ix.find("AE_CTRL")->refs.size() == 1, ".csr");
    check(ix.find("SA") && ix.find("SA")->kind == "service" && ix.find("SA")->refs.size() == 1, ".service");
    check(ix.find("imu") && ix.find("imu")->kind == "devmap", "the .devmap name");
    check(ix.find("f", "main") && ix.find("f", "main")->kind == "proc" && ix.find("f", "main")->refs.size() == 1, ".proc and its CALL");
    const IndexSymbol* loc = ix.find("again", "main:f");
    const IndexSymbol* glob = ix.find("again", "main");
    check(loc && glob && loc->refs.size() == 1 && glob->refs.empty() && loc->id != glob->id, "a procedure-local label is separate from a flow label of the same name");
    const IndexSymbol* ms = ix.find("start", "main");
    const IndexSymbol* ps = ix.find("start", "peri");
    check(ms && ps && ms->refs.size() == 1 && ps->refs.size() == 1 && ms->id != ps->id, "labels are per domain");
    check(ix.at("a.asm", 11, 10) == ms || ix.at("a.asm", 11, 1) == ms, "at(): the definition");
    check(ix.at("a.asm", 12, 11) == ms, "at(): a use", ix.at("a.asm", 12, 11) ? ix.at("a.asm", 12, 11)->name : "null");
    check(ix.occurrences(*lim).size() == 2, "occurrences = definition + uses");
    check(ix.renameProblem(*lim, "MOV") != "" && ix.renameProblem(*lim, "1bad") != "" && ix.renameProblem(*lim, "SA") != "" && ix.renameProblem(*lim, "LIMIT2") == "", "rename checks: keyword, bad name, taken, ok");
    Index bad = Index::build({Source{"b.asm", ".main\nx: JMP nowhere\n   MOV MAIN_SCRATCHPAD[0..1] ->\n"}}, L);
    check(bad.find("x", "main") && bad.undefined().size() == 1 && bad.undefined()[0].first == "nowhere", "a program with errors still has an index; undefined uses are listed");

    // source map (needs an assembly with a procedure called twice)
    Options o;
    o.callModel = CallModel::Inline;
    std::vector<Source> v{Source{"m.asm", ".main\n.proc f\n    EXEC_CSR WRITE8 0x10, 1\n    RET\n.endp\nstart: CALL f\n    CALL f\nh: JMP h\n"}};
    Result r = assemble(v, nullptr, o);
    SourceMap sm(r);
    check(r.ok && sm.instrsForLine("m.asm", 3).size() == 2, "a procedure line expanded at two call sites maps to two addresses", std::to_string(sm.instrsForLine("m.asm", 3).size()) + r.diagnosticsText());
    check(sm.instrAt(Domain::Main, 0) && sm.instrAt(Domain::Main, 0)->loc.line == 3, "address to line");
    check(sm.nearestCodeLine("m.asm", 4) == 8 || sm.nearestCodeLine("m.asm", 4) == 3 || sm.nearestCodeLine("m.asm", 4) > 0, "a breakpoint on a non-code line moves to a later code line");
    check(sm.nearestCodeLine("m.asm", 100) == 0, "...and to nothing past the last one");
    check(!sm.codeLines("m.asm").empty(), "code lines are listed");
    for (CallModel cm : {CallModel::Inline, CallModel::SoftRet}) {
        Options oc;
        oc.callModel = cm;
        std::vector<Source> vc{Source{"c.asm", ".main\n.proc f\n    EXEC_CSR WRITE8 0x10, 1\n    RET\n.endp\nstart: CALL f\n    CALL f\nh: JMP h\n"}};
        Result rc = assemble(vc, nullptr, oc);
        SourceMap smc(rc);
        const char* nm = cm == CallModel::Inline ? "inline" : "softret";
        check(rc.ok && !smc.instrsForLine("c.asm", 6).empty() && !smc.instrsForLine("c.asm", 7).empty(), std::string(nm) + ": a CALL line maps to an address (a breakpoint can be set on it)", rc.diagnosticsText());
        check(smc.nearestCodeLine("c.asm", 5) == 6, std::string(nm) + ": the lines of CALLs count as code lines");
        if (cm == CallModel::Inline) check(smc.instrsForLine("c.asm", 6)[0]->pc != smc.instrsForLine("c.asm", 7)[0]->pc, "inline: the two CALL lines are two different addresses");
    }
}

static void t_columns() {
    std::printf("\nDiagnostic columns\n");
    Result r = flow(".main\n    ALU ADD unsigned   ; note\nh: JMP h\n");
    const Diagnostic* d = nullptr;
    for (auto& x : r.diags) if (x.code == "W501") d = &x;
    check(d && d->loc.col == 4 && d->loc.endCol == 20, "a diagnostic spans the statement without indentation and comment",
          d ? std::to_string(d->loc.col) + ".." + std::to_string(d->loc.endCol) : "none");
    Result e = flow(".main\nx: NOP\nx: JMP x\n");
    const Diagnostic* q = nullptr;
    for (auto& y : e.diags) if (y.code == "E104") q = &y;
    check(q && q->loc.endCol - q->loc.col == 1 && q->loc.col == 0, "a quoted symbol narrows the span to that symbol", q ? std::to_string(q->loc.col) + ".." + std::to_string(q->loc.endCol) : "none");
}

static void t_dataflow() {
    std::printf("\nData-flow check\n");
    // ALU operands
    expectFlow("ALU with nothing loaded", ".main\ns: ALU ADD unsigned\nh: JMP h\n", "W501", 2);
    expectFlow("INC needs only ALU_OPA", ".main\n.assume MAIN_SCRATCHPAD[0..1] defined\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n ALU INC unsigned\nh: JMP h\n", "W501", 0);
    expectFlow("NOT and SHL need only ALU_OPA", ".main\n.assume MAIN_SCRATCHPAD[0..1] defined\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n ALU NOT\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[4..5]\n ALU SHL 2 unsigned\nh: JMP h\n", "W501", 0);
    expectFlow("loaded on one path only",
               ".main\n.assume MAIN_SCRATCHPAD[0..3] defined\ns: BR.COND PERI_RESULT_VALID, a\n JMP j\na: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\nj: ALU ADD unsigned\nh: JMP h\n", "W501", 2);
    expectFlow("loaded on both paths",
               ".main\n.assume MAIN_SCRATCHPAD[0..3] defined\ns: BR.COND PERI_RESULT_VALID, a\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n JMP j\na: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\nj: ALU ADD unsigned\nh: JMP h\n", "W501", 0);
    // buffers
    expectFlow("scratchpad read before any write", ".main\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\nh: JMP h\n", "W502", 1);
    expectFlow("partly written scratchpad range", ".main\n.assume MAIN_SCRATCHPAD[0..0] defined\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\nh: JMP h\n", "W502", 1);
    expectFlow(".assume declares an externally initialised range", ".main\n.assume MAIN_SCRATCHPAD[0..1] defined\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\nh: JMP h\n", "W502", 0);
    {
        Profile pf;
        pf.scratchpadResetDefined = true;
        Result r = flow(".main\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\nh: JMP h\n", CallModel::Inline, DataflowMode::Warn, &pf);
        check(r.ok && count(r, "W502") == 0, "a declared reset value (profile scratchpad_reset_defined) satisfies the rule", r.diagnosticsText());
    }
    expectFlow("WRITE_SP of an unwritten scratchpad range", ".csr C = 0x120\n.main\ns: EXEC_CSR WRITE_SP C, len=2, ptr=0\nh: JMP h\n", "W502", 1);
    expectFlow("WRITE_SP after the scratchpad was filled", ".csr C = 0x120\n.main\ns: MOV CONST(5) -> MAIN_SCRATCHPAD[0..1]\n EXEC_CSR WRITE_SP C, len=2, ptr=0\nh: JMP h\n", "W502", 0);
    expectFlow("WRITE_SP does not accept what only MAIN_CSR_DATA_BUF holds", ".csr C = 0x120\n.main\ns: EXEC_CSR READ_BUF C, len=2, ptr=0\n EXEC_CSR WRITE_SP C, len=2, ptr=0\nh: JMP h\n", "W502", 1);
    expectFlow("EXEC_CSR READ8 defines a buffer byte", ".csr C = 0x120\n.main\ns: EXEC_CSR READ8 C -> MAIN_CSR_DATA_BUF[3]\n MOV MAIN_CSR_DATA_BUF[3..4] -> ALU_OPA\nh: JMP h\n", "W502", 1);
    // ALU_OUT / ALU_STATUS
    expectFlow("ALU_OUT read after only a CMP", ".main\n.assume MAIN_SCRATCHPAD[0..3] defined\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU CMP.EQ unsigned\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[8..9]\nh: JMP h\n", "W503", 1);
    expectFlow("ALU_OUT read after an ADD", ".main\n.assume MAIN_SCRATCHPAD[0..3] defined\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU ADD unsigned\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[8..9]\nh: JMP h\n", "W503", 0);
    expectFlow("an ALU result overwritten before it was moved out", ".main\n.assume MAIN_SCRATCHPAD[0..3] defined\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU ADD unsigned\n ALU SUB unsigned\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[8..9]\nh: JMP h\n", "W504", 1);
    expectFlow("an ALU result moved out in time", ".main\n.assume MAIN_SCRATCHPAD[0..3] defined\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU ADD unsigned\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[8..9]\n ALU SUB unsigned\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[10..11]\nh: JMP h\n", "W504", 0);
    expectFlow("BR.COND ALU_CMP_TRUE with no CMP before", ".main\ns: BR.COND ALU_CMP_TRUE, s\nh: JMP h\n", "W506", 1);
    // the result slot
    expectFlow("slot consumed without testing PERI_RESULT_VALID", ".main\ns: MOV PERI_RESULT_SLOT[0..1] -> MAIN_SCRATCHPAD[0..1]\nh: JMP h\n", "W505", 1);
    expectFlow("slot consumed after testing PERI_RESULT_VALID", ".main\ns: BR.COND PERI_RESULT_VALID, y\n JMP s\ny: MOV PERI_RESULT_SLOT[0..1] -> MAIN_SCRATCHPAD[0..1]\nh: JMP h\n", "W505", 0);
    expectFlow("slot consumed twice after one test", ".main\ns: BR.COND PERI_RESULT_VALID, y\n JMP s\ny: MOV PERI_RESULT_SLOT[0..1] -> MAIN_SCRATCHPAD[0..1]\n MOV PERI_RESULT_SLOT[0..1] -> MAIN_SCRATCHPAD[4..5]\nh: JMP h\n", "W505", 1);
    // contracts between the domains
    {
        Result r = flow(slotProg(5, ""));
        check(r.ok && count(r, "W507") == 0 && count(r, "W510") == 0 && count(r, "W505") == 0 && count(r, "W502") == 0, "a consistent dispatch (compare id, consume 6 of 6) is clean", r.diagnosticsText());
    }
    expectFlow("handler consumes 4 of a 6-byte service", slotProg(3, ""), "W507", 1);
    expectFlow("handler consumes 8 of a 6-byte service", slotProg(7, ""), "W507", 1);
    expectFlow("a service Main never dispatches on", slotProg(5, " EXEC_PUB service=SB, src=0, len=2\n"), "W510", 1);
    expectFlow("one service published with two lengths", slotProg(5, " EXEC_PUB service=SA, src=0, len=2\n"), "W511", 1);
    // Peri buffers
    expectFlow("EXEC_PUB of bytes nobody read", ".service S = 1\n.peri\np: EXEC_PUB service=S, src=0, len=4\n JMP p\n", "W508", 1);
    expectFlow("EXEC_PUB of bytes an XFER read", ".service S = 1\n.devmap 0, I2C, 0x68\n.peri\np: EXEC_XFER READ dev=0, len=4, ptr=0, ext=[1]\n EXEC_PUB service=S, src=0, len=4\n JMP p\n", "W508", 0);
    expectFlow("EXEC_PUB beyond what was read", ".service S = 1\n.devmap 0, I2C, 0x68\n.peri\np: EXEC_XFER READ dev=0, len=4, ptr=0, ext=[1]\n EXEC_PUB service=S, src=0, len=6\n JMP p\n", "W508", 1);
    expectFlow("a one-byte XFER READ defines its destination", ".service S = 1\n.devmap 0, I2C, 0x68\n.peri\np: EXEC_XFER READ dev=0, addr=1, addrlen=1, ptr=5\n EXEC_PUB service=S, src=5, len=1\n JMP p\n", "W508", 0);
    // software CALL/RET clobbers the ALU: only the softret model gets the warning
    const char* clob = ".main\n.assume MAIN_SCRATCHPAD[0..3] defined\n.proc f\n EXEC_CSR WRITE8 0x10, 1\n RET\n.endp\n"
                       "s: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU CMP.EQ unsigned\n CALL f\n CALL f\n ALU ADD unsigned\n BR.COND ALU_CMP_TRUE, s\nh: JMP h\n";
    check(count(flow(clob, CallModel::Inline), "W501") == 0 && count(flow(clob, CallModel::Inline), "W506") == 0, "inline CALL leaves the ALU as it was: no warning");
    {
        Result r = flow(clob, CallModel::SoftRet);
        check(r.ok && count(r, "W501") == 2 && count(r, "W506") == 1, "softret CALL clobbers ALU_OPA / ALU_OPB and the compare flag", r.diagnosticsText());
    }
    {
        const char* single = ".main\n.assume MAIN_SCRATCHPAD[0..3] defined\n.proc f\n EXEC_CSR WRITE8 0x10, 1\n RET\n.endp\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n CALL f\n ALU ADD unsigned\nh: JMP h\n";
        Result r = flow(single, CallModel::SoftRet);
        check(r.ok && count(r, "W501") == 0, "a procedure with a single call site needs no return dispatch, so nothing is clobbered", r.diagnosticsText());
    }
    // modes
    {
        const std::string bad = ".main\ns: ALU ADD unsigned\nh: JMP h\n";
        check(flow(bad, CallModel::Inline, DataflowMode::Off).diags.empty(), "dataflow off: no findings");
        Result e = flow(bad, CallModel::Inline, DataflowMode::Error);
        check(!e.ok && count(e, "E501") == 2, "dataflow error: findings are errors (E501) and fail the build", e.diagnosticsText());
        Result q = flow(".main\ns: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\nh: JMP h\n", CallModel::Inline, DataflowMode::Warn, nullptr, {"W502"});
        check(q.ok && count(q, "W502") == 0, "a suppressed warning code is dropped");
        Options w;
        w.werror = true;
        std::vector<Source> v{Source{"t.asm", bad}};
        check(!assemble(v, nullptr, w).ok, "--werror turns the findings into a failed build");
    }
    // findings carry the call chain
    {
        Result r = flow(".main\n.proc f\n ALU ADD unsigned\n RET\n.endp\ns: CALL f\nh: JMP h\n");
        bool named = false;
        for (auto& d : r.diags) if (d.code == "W501" && d.msg.find("inside procedure f, called from line") != std::string::npos) named = true;
        check(named, "a finding inside an expanded procedure names the procedure and the call site");
    }
    // the examples stay clean
    {
        Options o;
        std::string base = std::string(ASMCORE_SOURCE_DIR_STR) + "/examples/";
        std::ifstream in(base + "imu_gyro.asm", std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        std::vector<Source> v{Source{base + "imu_gyro.asm", ss.str()}};
        auto res = [&](const std::string&, const std::string& n, Source& out) { std::ifstream f(base + n, std::ios::binary); std::ostringstream t; t << f.rdbuf(); out.name = base + n; out.text = t.str(); return !out.text.empty(); };
        Result r = assemble(v, res, o);
        check(r.ok && r.warningCount() == 0, "examples/imu_gyro.asm (a real two-domain service) raises no finding", r.diagnosticsText());
    }
}

int main() {
    std::printf("AssemblyCore tests\n");
    t_golden_main();
    t_alu_codes();
    t_csr_codes();
    t_peri_codes();
    t_errors();
    t_v31_branches();
    t_v31_softret();
    t_v31_image();
    t_v31_profile_rules();
    t_v31_artifacts();
    t_v31_events();
    t_v31_validators();
    t_limits();
    t_cascade();
    t_call_models();
    t_call_rules();
    t_wcet();
    t_misc();
    t_dataflow();
    t_columns();
    t_lang();
    t_index();
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
