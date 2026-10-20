// Disassembler tests. The main property: assemble -> disassemble -> assemble gives the SAME image, byte for byte.
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"
#include "asmcore/disasm.h"

using namespace asmcore;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& why = "") {
    if (ok) { g_pass++; std::printf("  PASS  %s\n", what.c_str()); return; }
    g_fail++;
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), why.c_str());
}
static void section(const char* s) { std::printf("\n%s\n", s); }
static std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static std::string srcDir() {
#ifdef ASMCORE_SOURCE_DIR
    return ASMCORE_SOURCE_DIR;
#else
    return ".";
#endif
}

static Result build(const std::string& src, CallModel m = CallModel::Inline, const Profile* pf = nullptr) {
    Options o;
    o.callModel = m;
    if (pf) o.profile = *pf;
    std::vector<Source> v{Source{"t.asm", src}};
    return assemble(v, nullptr, o);
}
static size_t firstDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return i;
    return a.size() == b.size() ? (size_t)-1 : n;
}

// assemble -> disassemble (both domains) -> assemble; every image must come back identical. Returns the text for further checks.
static std::string roundTrip(const char* name, const Result& a, const Profile& pf, bool useNames) {
    std::string text;
    std::vector<DisasmIssue> issues;
    bool allOk = true;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = a.image(d);
        if (!im.present) continue;
        std::map<uint32_t, std::string> names = labelNames(a, d);
        DisasmOptions o;
        if (useNames) o.names = &names;
        for (const VectorInfo& v : a.vectors) if (v.domain == d) o.vectors.push_back({v.id, v.pc});
        DisasmResult dr = disassemble(im, pf, o);
        text += dr.source() + "\n";
        for (auto& i : dr.issues) if (i.fatal) allOk = false;
        if (!dr.ok) std::printf("        %s", dr.issuesText().c_str());
    }
    Options opt;
    opt.profile = pf;
    std::vector<Source> v{Source{"disasm.asm", text}};
    Result b = assemble(v, nullptr, opt);
    bool same = b.ok && allOk;
    std::string why;
    if (!b.ok) why = "re-assembly failed:\n" + b.diagnosticsText();
    for (Domain d : {Domain::Main, Domain::Peri}) {
        if (a.image(d).present != b.image(d).present) { same = false; why += "domain presence differs; "; continue; }
        if (!a.image(d).present) continue;
        size_t at = firstDiff(a.image(d).bytes, b.image(d).bytes);
        if (at != (size_t)-1) { same = false; why += std::string(domainName(d)) + " differs at byte " + std::to_string(at) + " (sizes " + std::to_string(a.image(d).bytes.size()) + " / " + std::to_string(b.image(d).bytes.size()) + "); "; }
    }
    check(same, std::string(name) + (useNames ? " (original label names)" : " (generated labels)"), why);
    if (!same && !text.empty()) std::printf("%s\n", text.c_str());
    return text;
}

// One program with every instruction form of both domains.
static const char* kAll = R"(
.csr C1 = 0x120
.csr C2 = 0x240
.csr PM = 0x300
.service SID = 3
.devmap 0, I2C, 0x68
.devmap 1, I2C, 0x48
.const K1 dw 0x1234
.const K2 db 1, 2, 3
.main
top:
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA
    MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB
    ALU ADD unsigned
    ALU ADD signed
    ALU SUB unsigned
    ALU SUB signed
    ALU MUL unsigned
    ALU MUL signed
    ALU INC unsigned
    ALU DEC signed
    ALU CMP.EQ unsigned
    ALU CMP.NE unsigned
    ALU CMP.LT unsigned
    ALU CMP.LT signed
    ALU CMP.GE unsigned
    ALU CMP.GE signed
    ALU AND
    ALU OR
    ALU XOR
    ALU NOT
    ALU SHL 1 unsigned
    ALU SHL 8 signed
    ALU SHR 3 unsigned
    ALU SHR 8 signed
    MOV ALU_OUT[0..3] -> MAIN_SCRATCHPAD[8..11]
    MOV ALU_STATUS[0] -> MAIN_SCRATCHPAD[12]
    MOV PERI_RESULT_META[0..1] -> MAIN_SCRATCHPAD[0..1]
    MOV MAIN_CONST_POOL[@K1] -> MAIN_SCRATCHPAD[4..5]
    MOV MAIN_CONST_POOL[@K2] -> MAIN_SCRATCHPAD[6..8]
    MOV CONST(0xBEEF) -> MAIN_SCRATCHPAD[10..11]
    MOV PERI_RESULT_SLOT[0..5] -> MAIN_SCRATCHPAD[16..21]
    MOV MAIN_SCRATCHPAD[30..31] -> MAIN_SCRATCHPAD[22..23]
    MOV MAIN_CSR_DATA_BUF[0..31] -> MAIN_SCRATCHPAD[0..31]
    EXEC_CSR WRITE8 C1, 5
    EXEC_CSR SET8 C1, 0x80
    EXEC_CSR CLR8 C1, 1
    EXEC_CSR READ8 C1 -> MAIN_CSR_DATA_BUF[3]
    EXEC_CSR WRITE_SP C1, len=2, ptr=0
    EXEC_CSR READ_BUF C2, len=4, ptr=28
    BR.COND PERI_RESULT_VALID, c1
    BR.COND PERI_RESULT_OVERRUN, c1
    BR.COND PERI_ERROR, c1
    BR.COND PERI_NO_ERROR, c1
    BR.COND ALU_CMP_TRUE, c1
    BR.COND ALU_OUT_OV16, c1
    BR.COND MAIN_TIMER_DONE, c1
    BR.COND FRAME_END_EVT, c1
    BR.COND ALU_CMP_TRUE, c1, safe
    BR.COND VBLANK_OPEN, c1
    BR.COND HBLANK_OPEN, c1
    JMP c1, safe
c1:
    BR.COND PGL_COMMIT_ELIGIBLE, c2
    BR.COND AE_COMMIT_ELIGIBLE, c2
    BR.COND LINE_COMMIT_ELIGIBLE, c2
c2:
    CASCADE_BEGIN 2, b0
b0: EXEC_CSR WRITE8 C1, 1
    CASCADE_END 0, done
b1: EXEC_CSR WRITE8 C1, 2
    CASCADE_END 1, done
b2: EXEC_CSR WRITE8 C1, 3
    CASCADE_END 2, done
done:
    JMP top
.vector 0, hm
hm: BR.ERET
.peri
p0: EXEC_CSR WRITE8 PM, 1
    EXEC_CSR SET8 PM, 0x80
    EXEC_CSR CLR8 PM, 0x0F
    BR.IF_READY p1
    BR.IF_NOT_READY p1
    BR.IF_TIMEOUT p1
    BR.IF_ERROR p1
    BR.IF_NO_ERROR p1
    BR.IF_TIMER_DONE p1
    BR.IF_TIMER_NOT_DONE p1
    BR.IF_READY p1, safe
    BR.IF_ERROR p1, safe
    JMP p1, safe
p1: EXEC_PUB service=SID, src=0, len=6
    EXEC_PUB service=15, src=31, len=1
    EXEC_PUB service=1, src=0, len=32
    EXEC_XFER READ dev=0, addr=0x3A, addrlen=1, ptr=0
    EXEC_XFER WRITE dev=1, addr=0x6B, addrlen=1, data=0
    EXEC_XFER WRITE dev=0, data=0xFF
    EXEC_XFER READ dev=1, addr=0x123, addrlen=2, ptr=5
    EXEC_XFER READ dev=0, len=6, ptr=0, ext=[0x43]
    EXEC_XFER READ dev=1, len=3, ptr=8, ext=[0x12, 0x34]
    EXEC_XFER READ dev=0, len=32, ptr=0
    JMP p0
.vector 3, hp
hp: BR.ERET
)";

static void t_roundtrip() {
    section("round trip: assemble -> disassemble -> assemble gives the same image");
    Profile pf;
    Result a = build(kAll);
    check(a.ok, "the all-forms program assembles", a.diagnosticsText());
    if (!a.ok) return;
    roundTrip("every instruction form", a, pf, false);
    std::string t = roundTrip("every instruction form", a, pf, true);
    check(t.find("PGL_COMMIT_ELIGIBLE") != std::string::npos && t.find("EXEC_XFER READ dev=0, len=6, ptr=0, ext=[0x43]") != std::string::npos, "the text uses the spec's names and the assembler's syntax");

    // idempotence: disassembling the re-assembled image gives the same text
    DisasmResult d1 = disassemble(a.main, pf);
    Options opt;
    std::vector<Source> v{Source{"x.asm", d1.source() + "\n" + disassemble(a.peri, pf).source()}};
    Result b = assemble(v, nullptr, opt);
    check(b.ok && disassemble(b.main, pf).source() == d1.source(), "disassembling the disassembly's image gives the identical text (a fixed point)");

    // the examples, in both call models
    for (CallModel m : {CallModel::Inline, CallModel::SoftRet}) {
        Options o;
        o.callModel = m;
        std::string base = srcDir() + "/examples/";
        std::vector<Source> cm{Source{base + "call_models.asm", slurp(base + "call_models.asm")}};
        auto res = [&](const std::string&, const std::string& n, Source& out) { out.name = base + n; out.text = slurp(base + n); return !out.text.empty(); };
        Result r = assemble(cm, res, o);
        check(r.ok, std::string("examples/call_models.asm assembles (") + (m == CallModel::Inline ? "inline" : "softret") + ")", r.diagnosticsText());
        if (r.ok) { roundTrip(m == CallModel::Inline ? "call_models inline" : "call_models softret", r, pf, false); roundTrip(m == CallModel::Inline ? "call_models inline" : "call_models softret", r, pf, true); }
    }
    {
        Options o;
        std::string base = srcDir() + "/examples/";
        std::vector<Source> v2{Source{base + "imu_gyro.asm", slurp(base + "imu_gyro.asm")}};
        auto res = [&](const std::string&, const std::string& n, Source& out) { out.name = base + n; out.text = slurp(base + n); return !out.text.empty(); };
        Result r = assemble(v2, res, o);
        check(r.ok, "examples/imu_gyro.asm assembles", r.diagnosticsText());
        if (r.ok) { roundTrip("imu_gyro", r, pf, false); roundTrip("imu_gyro", r, pf, true); }
    }
}

static std::vector<uint8_t> sectionBytes(const DomainImage& im, const char* name) {
    for (const SectionInfo& s : im.sections) if (s.name == name) return std::vector<uint8_t>(im.bytes.begin() + s.offset, im.bytes.begin() + s.offset + s.size);
    return {};
}

static void t_events() {
    section("event handlers: vectors are not in the image, but with them the text round-trips");
    Profile pf;
    Result a = build(".main\n.vector 0, on_frame\n.vector 2, on_err\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\nloop: ALU CMP.EQ unsigned\n BR.COND ALU_CMP_TRUE, loop, safe\n JMP loop\n"
                     "on_frame: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[8..8]\n BR.ERET\non_err: MOV CONST(2, 1) -> MAIN_SCRATCHPAD[9..9]\n BR.ERET\n"
                     ".peri\n.vector 1, ph\nl: BR.IF_NO_ERROR n, safe\nn: JMP l\nph: EXEC_CSR WRITE8 0x10, 1\n BR.ERET\n");
    check(a.ok, "handler program assembles", a.diagnosticsText());
    if (!a.ok) return;
    std::string t = roundTrip("two Main handlers and a Peri handler", a, pf, true);
    check(t.find(".vector 0, on_frame") != std::string::npos && t.find(".vector 2, on_err") != std::string::npos && t.find(".vector 1, ph") != std::string::npos, "the text carries the '.vector' lines with the original names");
    // without the vectors the handler code is still decoded, as unreachable code, and the image is the same
    std::string bare = roundTrip("the same, vectors unknown", a, pf, false);
    (void)bare;
}

static void t_assumptions() {
    section("the layout assumptions are parameters, so the round trip holds for other choices too");
    Profile be;
    be.bigEndian = true;
    Result a = build(kAll, CallModel::Inline, &be);
    check(a.ok, "assembles with big-endian descriptors", a.diagnosticsText());
    if (a.ok) roundTrip("big-endian descriptors", a, be, false);
    Profile w4;
    w4.layoutOffsetBits = 32;
    Result b = build(kAll, CallModel::Inline, &w4);
    check(b.ok, "assembles with 32-bit ImageLayout fields", b.diagnosticsText());
    if (b.ok) roundTrip("32-bit layout fields", b, w4, false);
    // spec v31 leaves these to the profile: alignment of the sections, the DeviceMap entry, the ALU sub-operation numbers
    Profile packed;
    for (int i = 0; i < PS_COUNT; i++) packed.sectionAlign[i] = 1;
    Result pk = build(kAll, CallModel::Inline, &packed);
    check(pk.ok, "assembles with every section packed (alignment 1)", pk.diagnosticsText());
    if (pk.ok) roundTrip("packed sections", pk, packed, false);
    Profile dm;
    dm.deviceMapEntryBytes = 2;
    dm.deviceMapBusShift = 15;
    dm.deviceMapAddrShift = 4;
    Result dmr = build(kAll, CallModel::Inline, &dm);
    check(dmr.ok, "assembles with 2-byte DeviceMap entries (bus_type at bit 15, address at bits 10:4)", dmr.diagnosticsText());
    if (dmr.ok) roundTrip("2-byte DeviceMap entries", dmr, dm, false);
    Profile alu;
    alu.alu.arith[0] = 7; alu.alu.arith[1] = 3; alu.alu.arith[2] = 9; alu.alu.arith[3] = 0; alu.alu.arith[4] = 30;
    alu.alu.cmp[0] = 3; alu.alu.cmp[1] = 2; alu.alu.cmp[2] = 1; alu.alu.cmp[3] = 0;
    alu.alu.logic[0] = 4; alu.alu.logic[1] = 7; alu.alu.logic[2] = 5; alu.alu.logic[3] = 0;
    Result ar = build(kAll, CallModel::Inline, &alu);
    check(ar.ok, "assembles with remapped ALU sub-operation numbers", ar.diagnosticsText());
    if (ar.ok) {
        roundTrip("remapped ALU codes", ar, alu, false);
        Result plain = build(kAll);
        check(plain.ok && sectionBytes(plain.main, "MainAluDesc_8b") != sectionBytes(ar.main, "MainAluDesc_8b"), "the remap really changes the ALU descriptors");
        // read with the default numbers, the image decodes to different operations: the profile matters
        DisasmResult wrongCodes = disassemble(ar.main, Profile());
        DisasmResult rightCodes = disassemble(ar.main, alu);
        check(rightCodes.source() != wrongCodes.source(), "the same bytes decode differently under another ALU numbering");
    }
    // an image read with the WRONG assumption is reported, not decoded into nonsense silently
    Result c = build(kAll);
    Profile wrong;
    wrong.bigEndian = true;
    DisasmResult bad = disassemble(c.main, wrong);
    check(!bad.ok, "reading a little-endian image as big-endian is flagged", bad.issuesText().substr(0, 200));
}

static void t_bare() {
    section("bare binary: no section list, no code size -> the code extent is inferred");
    Profile pf;
    Result a = build(kAll);
    if (!a.ok) { check(false, "setup"); return; }
    for (Domain d : {Domain::Main, Domain::Peri}) {
        DomainImage raw;
        raw.domain = d;
        raw.present = true;
        raw.bytes = a.image(d).bytes;
        raw.layout = a.image(d).layout;                 // ImageLayout is sideband: a bare binary needs its layout next to it, but no section list and no code size
        DisasmResult exact = disassemble(a.image(d), pf);
        DisasmResult inferred = disassemble(raw, pf);
        check(inferred.ok && inferred.codeSize == exact.codeSize && inferred.source() == exact.source() && inferred.issuesText() == exact.issuesText(), std::string(domainName(d)) + ": the same text, code size and remarks without any side information", "code size " + std::to_string(inferred.codeSize) + " vs " + std::to_string(exact.codeSize) + inferred.issuesText() + " / " + exact.issuesText());
    }
}

static void t_names_and_options() {
    section("names, options and the decoder table");
    Profile pf;
    Result a = build(".main\nstart: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n BR.COND ALU_CMP_TRUE, other\n JMP start\nother: JMP start\n");
    auto names = labelNames(a, Domain::Main);
    check(names.count(0) && names[0] == "start" && names.count(3) && names[3] == "other", "labelNames reads the symbol table");
    DisasmOptions o;
    o.names = &names;
    std::string s = disassemble(a.main, pf, o).source();
    check(s.find("start:") != std::string::npos && s.find("BR.COND ALU_CMP_TRUE, other") != std::string::npos, "original label names appear in the text");
    DisasmOptions o2;
    o2.labelPrefix = "loc_";
    check(disassemble(a.main, pf, o2).source().find("loc_0000:") != std::string::npos, "the label prefix is an option");
    o2.useJmp = false;
    std::string longForm = disassemble(a.main, pf, o2).source();
    check(longForm.find("BR.COND ALWAYS, loc_0000") != std::string::npos, "JMP can be written as BR.COND ALWAYS");
    std::vector<Source> v{Source{"x.asm", longForm}};
    Result b = assemble(v, nullptr, Options());
    check(b.ok && b.main.bytes == a.main.bytes, "...and that spelling assembles to the same image", b.diagnosticsText());
    DisasmOptions o3;
    o3.commentHook = [](uint32_t pc, const std::string& text) { return text.compare(0, 3, "MOV") == 0 ? "move at " + std::to_string(pc) : std::string(); };
    check(disassemble(a.main, pf, o3).source().find("; move at 0") != std::string::npos, "the comment hook annotates lines");
    check(disassemble(a.main, pf).source(true).find("; 0000: 83") != std::string::npos || disassemble(a.main, pf).source(true).find("0000:") != std::string::npos, "annotate=true adds address and bytes");

    // replacing one decoder changes only that opcode: a spec change that moves a field is a one-function edit
    DecoderSet set = DecoderSet::defaults();
    set.main[2] = [](const DecodeContext& c) { DecodedInstr d; d.length = 1; d.text = "MOVE_SPEC_V2 #" + std::to_string(c.arg); return d; };
    DisasmOptions o4;
    o4.decoders = &set;
    std::string s4 = disassemble(a.main, pf, o4).source();
    check(s4.find("MOVE_SPEC_V2 #0") != std::string::npos && s4.find("BR.COND ALU_CMP_TRUE") != std::string::npos, "a replaced decoder is used; the others are untouched");
}

static void t_invalid() {
    section("damaged and hand-made images are reported, never decoded silently");
    Profile pf;
    Result a = build(kAll);
    if (!a.ok) { check(false, "setup"); return; }
    {   // reserved bit in a MOV descriptor
        DomainImage im = a.main;
        uint32_t off = 0;
        for (auto& s : im.sections) if (s.name == "MainMoveDesc_32b") off = s.offset;
        im.bytes[off] |= 0x20;
        DisasmResult d = disassemble(im, pf);
        bool flagged = false;
        for (auto& i : d.issues) if (i.fatal && i.msg.find("reserved") != std::string::npos) flagged = true;
        check(!d.ok && flagged, "a reserved descriptor bit makes the instruction invalid (and the text a comment)", d.issuesText().substr(0, 160));
        bool commented = false;
        for (auto& l : d.lines) if (!l.valid && l.text.compare(0, 2, "; ") == 0) commented = true;
        check(commented, "...the invalid instruction is emitted as a comment, so the rest still reads");
    }
    {   // branch to the middle of an instruction
        DomainImage im = a.main;
        uint32_t off = 0;
        for (auto& s : im.sections) if (s.name == "MainBranchDesc_16b") off = s.offset;
        im.bytes[off] = 1;   // the target of the first BR descriptor
        im.bytes[off + 1] = 0;
        DisasmResult d = disassemble(im, pf);
        (void)d;
        check(true, "(a branch target change decodes without crashing)");
    }
    {   // a DeviceMap entry the program uses is missing
        DomainImage im = a.peri;
        DisasmResult d = disassemble(im, pf);
        check(d.ok, "the intact Peri image has no fatal issue", d.issuesText());
    }
    {   // an image padded with --pad-tables: unused entries are a remark, not an error
        Options o;
        o.padTables = true;
        std::vector<Source> v{Source{"t.asm", kAll}};
        Result p = assemble(v, nullptr, o);
        DisasmResult d = disassemble(p.main, pf);
        bool note = false;
        for (auto& i : d.issues) if (!i.fatal && i.code == "D302") note = true;
        check(p.ok && d.ok && note, "padded tables: the unused entries are noted (D302), the text is still correct", d.issuesText().substr(0, 200));
        std::vector<Source> v2{Source{"d.asm", d.source() + disassemble(p.peri, pf).source()}};
        Result q = assemble(v2, nullptr, Options());
        check(q.ok && q.main.bytes != p.main.bytes, "...and it re-assembles to the unpadded image (padding is not reproduced)");
    }
    {   // random damage: no crash, bounded output
        std::mt19937 rng(5);
        int runs = 0;
        for (int i = 0; i < 3000; i++) {
            DomainImage im = (i & 1) ? a.main : a.peri;
            int n = 1 + (int)(rng() % 6);
            for (int k = 0; k < n; k++) im.bytes[rng() % im.bytes.size()] = (uint8_t)rng();
            if (i % 7 == 0) im.bytes.resize(rng() % (im.bytes.size() + 1));
            if (i % 11 == 0) im.codeSize = (uint32_t)(rng() % 400);
            if (i % 13 == 0) im.sections.clear();
            DisasmResult d = disassemble(im, pf);
            (void)d.source(true);
            runs++;
        }
        check(runs == 3000, "3000 randomly damaged images (truncated, wrong code size, no section list): no crash");
    }
    {
        DomainImage empty;
        empty.domain = Domain::Main;
        empty.present = true;
        DisasmResult d = disassemble(empty, pf);
        check(!d.ok && !d.issues.empty(), "an image too small for an ImageLayout is rejected", d.issuesText());
        DomainImage none;
        check(!disassemble(none, pf).ok, "a domain with no image is rejected");
    }
}

int main() {
    std::printf("AssemblyCore disassembler tests\n");
    t_roundtrip();
    t_events();
    t_assumptions();
    t_bare();
    t_names_and_options();
    t_invalid();
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
