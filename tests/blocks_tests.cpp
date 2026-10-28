// Block programs: text -> blocks -> text must keep the program (same images), the file format must round-trip, the definitions must cover the
// whole language.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include "asmcore/asmcore.h"
#include "asmcore/blocks.h"
#include "asmcore/lang.h"
#include "asmcore/sim.h"

using namespace asmcore;
using namespace asmcore::blocks;
namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& why = "") {
    if (ok) { g_pass++; std::printf("  PASS  %s\n", what.c_str()); return; }
    g_fail++;
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), why.c_str());
}
static void section(const char* s) { std::printf("\n%s\n", s); }

#ifndef ASMCORE_SOURCE_DIR
#define ASMCORE_SOURCE_DIR "."
#endif
static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static std::vector<std::string> types(const Section& s) { std::vector<std::string> v; for (const Block& b : s.blocks) v.push_back(b.type); return v; }
static std::string join(const std::vector<std::string>& v) { std::string s; for (auto& x : v) s += x + " "; return s; }

static void t_coverage() {
    section("every instruction and directive of the language has a block");
    const lang::Language L = lang::Language::defaults();
    // one valid statement per mnemonic; a new mnemonic in the vocabulary fails this test until it gets a block (and a line here)
    const std::map<std::string, std::string> sample = {
        {"MOV", "MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA"}, {"ALU", "ALU ADD unsigned"}, {"EXEC_CSR", "EXEC_CSR WRITE8 C, 1"}, {"BR.COND", "BR.COND ALU_CMP_TRUE, t"},
        {"JMP", "JMP t"}, {"BR.ERET", "BR.ERET"}, {"CASCADE_BEGIN", "CASCADE_BEGIN 1, b0"}, {"CASCADE_END", "CASCADE_END 0, x"}, {"CALL", "CALL f"}, {"RET", "RET"},
        {"EXEC_PUB", "EXEC_PUB service=S, src=0, len=2"}, {"EXEC_XFER", "EXEC_XFER READ dev=0, len=2, ptr=0"}, {"BR.ALWAYS", "BR.ALWAYS t"},
        {"BR.IF_READY", "BR.IF_READY t"}, {"BR.IF_NOT_READY", "BR.IF_NOT_READY t"}, {"BR.IF_TIMEOUT", "BR.IF_TIMEOUT t"}, {"BR.IF_ERROR", "BR.IF_ERROR t"},
        {"BR.IF_NO_ERROR", "BR.IF_NO_ERROR t"}, {"BR.IF_TIMER_DONE", "BR.IF_TIMER_DONE t"}, {"BR.IF_TIMER_NOT_DONE", "BR.IF_TIMER_NOT_DONE t"}};
    std::string missing, raw;
    for (const lang::Entry* e : L.all(lang::Tok::Mnemonic)) {
        auto it = sample.find(e->name);
        if (it == sample.end()) { missing += e->name + " "; continue; }
        ImportStats st;
        importAsm(".main\n" + it->second + "\n", "", &st);
        if (st.raw) raw += e->name + " ";
    }
    check(missing.empty(), "the test has a sample line for every mnemonic of the vocabulary", "no sample for: " + missing);
    check(raw.empty(), "...and each becomes a real block, not a Raw block", "raw: " + raw);
    const std::set<std::string> structural = {".MAIN", ".PERI", ".PROC", ".ENDP", ".VECTOR", ".WCET", ".ASSUME"};
    int dir = 0;
    for (const lang::Entry* e : L.all(lang::Tok::Directive)) {
        if (structural.count(e->name) || e->name == "EQU") continue;
        ImportStats st;
        Document d = importAsm(".main\n" + std::string(e->name == ".INCLUDE" ? ".include \"x.inc\"" : e->name == ".END" ? ".end" : e->name + " A = 1") + "\n", "", &st);
        if (!st.raw && d.sections.size() == 1 && d.sections[0].blocks.size() == 1 && d.sections[0].blocks[0].type == "directive") dir++;
    }
    check(dir >= 8, "the definition directives (.equ .csr .const .service .devmap .csrbit .include .end ...) become 'directive' blocks", std::to_string(dir));
    // every definition is internally consistent
    bool ok = true;
    std::set<std::string> seen;
    for (const BlockDef& d : Registry::defaults().all()) {
        if (!seen.insert(d.type).second) ok = false;
        if (d.category.empty() || (d.valueType.empty() && d.title.empty())) ok = false;                     // components (values) may be untitled: expressions show only their parts
        for (const FieldDef& f : d.fields) if (f.name.empty() || (f.label.empty() && d.valueType.empty())) ok = false;
    }
    check(ok, "the definitions have unique types, titles, categories and field labels");
    const BlockDef* alu = Registry::defaults().find("alu");
    check(alu && std::find(alu->fields[0].options.begin(), alu->fields[0].options.end(), "CMP.GE") != alu->fields[0].options.end(), "the ALU drop-down is built from the vocabulary (CMP.GE is there)");
    const BlockDef* br = Registry::defaults().find("br_peri");
    check(br && std::find(br->fields[0].options.begin(), br->fields[0].options.end(), "IF_TIMER_DONE") != br->fields[0].options.end(), "...and so is the Peri condition list");
}

static void t_import() {
    section("importing text");
    const std::string src =
        "; header comment\n"
        ".include \"defs.inc\"\n"
        "\n"
        ".main\n"
        ".vector 0, on_evt\n"
        "start:   MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]   ; load five\n"
        "loop: skip: ALU CMP.EQ unsigned\n"
        "    BR.COND ALU_CMP_TRUE, loop, safe\n"
        "    ALU SHL 2 unsigned\n"
        "    EXEC_CSR WRITE_SP OUT, ptr=4, len=2\n"
        "    this is not an instruction\n"
        "    JMP start\n"
        ".proc leaf\n"
        "    RET\n"
        ".endp\n"
        "on_evt: BR.ERET\n"
        ".peri\n"
        "p: EXEC_XFER READ dev=0, len=2, ptr=0, ext=[0x43]\n"
        "   EXEC_PUB len=2, service=S, src=0\n"
        "   BR.IF_NO_ERROR p, safe\n"
        "NAME EQU 5\n";
    ImportStats st;
    Document d = importAsm(src, "demo", &st);
    check(d.sections.size() == 3 && d.sections[0].domain == "" && d.sections[1].domain == "main" && d.sections[2].domain == "peri", "three sections: the lines before .main, Main, Peri");
    check(types(d.sections[0]) == std::vector<std::string>({"comment", "directive"}), "prelude: comment, .include (the blank line before .main is the separator generate() writes itself)", join(types(d.sections[0])));
    const std::vector<std::string> mainTypes = types(d.sections[1]);
    check(mainTypes == std::vector<std::string>({"vector", "label", "mov", "label", "label", "alu", "br_cond", "alu", "csr_write_sp", "raw", "jmp", "proc", "label", "br_eret"}), "Main: vector, labels split from their statements, instructions, a proc", join(mainTypes));
    const Block& mov = d.sections[1].blocks[2];
    check(mov.get("src") == "CONST(5, 2)" && mov.get("dst") == "MAIN_SCRATCHPAD[0..1]" && mov.comment == "load five", "MOV fields and the end-of-line comment");
    const Block& br = d.sections[1].blocks[6];
    check(br.get("cond") == "ALU_CMP_TRUE" && br.get("target") == "loop" && br.get("safe") == "1", "BR.COND with its safe flag");
    check(d.sections[1].blocks[7].get("op") == "SHL" && d.sections[1].blocks[7].get("amount") == "2" && d.sections[1].blocks[7].get("mode") == "unsigned", "ALU op, amount and mode");
    check(d.sections[1].blocks[8].get("len") == "2" && d.sections[1].blocks[8].get("ptr") == "4", "keyword operands are taken apart in any order");
    const Block& rawb = d.sections[1].blocks[9];
    check(rawb.type == "raw" && rawb.get("text") == "this is not an instruction", "a line that is not understood is kept verbatim as a Raw block");
    const Block& proc = d.sections[1].blocks[11];
    check(proc.get("name") == "leaf" && proc.stack("BODY") && proc.stack("BODY")->size() == 1 && (*proc.stack("BODY"))[0].type == "ret", "a procedure holds its body");
    check(types(d.sections[2]) == std::vector<std::string>({"label", "xfer", "pub", "br_peri", "raw"}) , "Peri: XFER, PUB, a Peri branch; the 8051-style NAME EQU stays text", join(types(d.sections[2])));
    check(d.sections[2].blocks[3].get("cond") == "IF_NO_ERROR" && d.sections[2].blocks[3].get("safe") == "1", "BR.IF_NO_ERROR becomes a Peri branch block");
    check(st.raw == 2 && st.rawLines.size() == 2, "the import reports how many lines stayed Raw", std::to_string(st.raw));
    std::set<std::string> ids;
    bool unique = true;
    std::function<void(const std::vector<Block>&)> walk = [&](const std::vector<Block>& v) { for (const Block& b : v) { if (!ids.insert(b.id).second || b.id.empty()) unique = false; for (auto& s : b.stacks) walk(s.second); for (auto& vl : b.values) walk(vl.second); } };
    for (const Section& s : d.sections) walk(s.blocks);
    check(unique && ids.size() == d.blockCount(), "every block has its own id");
}

static void t_generate() {
    section("generating text");
    const std::string src =
        ".main\n"
        "start: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]   ; load five\n"
        "    BR.COND ALU_CMP_TRUE, start, safe\n"
        "    EXEC_CSR WRITE_SP OUT, ptr=4, len=2\n"
        ".proc f\n"
        "    RET\n"
        ".endp\n"
        ".peri\n"
        "    BR.IF_READY p, safe\n"
        "p:  JMP p\n";
    Document d = importAsm(src);
    Generated g = generate(d);
    const std::string expect =
        ".main\n"
        "start:\n"
        "    MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]    ; load five\n"
        "    BR.COND ALU_CMP_TRUE, start, safe\n"
        "    EXEC_CSR WRITE_SP OUT, len=2, ptr=4\n"
        ".proc f\n"
        "    RET\n"
        ".endp\n"
        "\n"
        ".peri\n"
        "    BR.IF_READY p, safe\n"
        "p:\n"
        "    JMP p\n";
    check(g.text == expect, "canonical text: labels on their own line, instructions indented, keyword operands in a fixed order", g.text);
    Document again = importAsm(g.text);
    check(generate(again).text == g.text, "generating, importing and generating again changes nothing (idempotent)");
    // the source map
    std::set<std::string> ids;
    for (const GenLine& l : g.lines) if (!l.blockId.empty()) ids.insert(l.blockId);
    check(g.lines.size() == 13 && ids.size() == 9, "every line maps to the block that produced it (structure lines map to none)", std::to_string(g.lines.size()) + " lines, " + std::to_string(ids.size()) + " blocks");
    const std::string movId = d.sections[0].blocks[1].id;
    check(g.linesOf(movId).size() == 1 && g.linesOf(movId)[0] == 3 && g.atLine(3)->blockId == movId, "...in both directions (block <-> line)");
    const std::string procId = d.sections[0].blocks[4].id;
    check(g.linesOf(procId).size() == 2, "a procedure owns its .proc and .endp lines");
    check(g.atLine(1) && g.atLine(1)->blockId.empty() && !g.atLine(99), "the .main line belongs to no block; a line past the end is none");
    // editing the model changes the text
    Block jmp = d.make("jmp");
    jmp.set("target", "start");
    jmp.set("safe", "1");
    d.sections[0].blocks.insert(d.sections[0].blocks.begin() + 2, jmp);
    check(generate(d).text.find("    JMP start, safe\n") != std::string::npos, "a block added to the model appears in the text");
    d.sections[0].blocks[1].disabled = true;
    check(generate(d).text.find("; MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n") != std::string::npos, "a disabled block is generated as a comment");
    // a block made from its definition has the default fields and works
    Document fresh;
    fresh.sections.push_back(Section());
    fresh.sections[0].domain = "main";
    Block mv = fresh.make("mov");
    mv.set("src", "CONST(1, 1)");
    mv.set("dst", "MAIN_SCRATCHPAD[0..0]");
    fresh.sections[0].blocks.push_back(mv);
    Block alu = fresh.make("alu");
    alu.set("op", "ADD");
    fresh.sections[0].blocks.push_back(alu);
    check(generate(fresh).text == ".main\n    MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n    ALU ADD\n", "optional parts of a template are left out when empty (ALU ADD, no amount or mode)", generate(fresh).text);
}

static void t_file() {
    section("the .acblk file");
    Document d = importAsm(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]  ; five\n BR.COND ALU_CMP_TRUE, start, safe\n.proc f\n RET\n.endp\n.peri\n JMP p\np: JMP p\n", "demo");
    d.hardware = "board.achw";
    d.sections[0].blocks[1].disabled = true;
    const std::string text = serializeDocument(d);
    check(text.find("\"format\": \"acblk\"") != std::string::npos && text.find("\"hardware\": \"board.achw\"") != std::string::npos, "the file names its format and hardware", text.substr(0, 200));
    Document back;
    std::string err;
    check(parseDocument(text, back, err), "it can be read back", err);
    check(serializeDocument(back) == text, "...and written again byte for byte");
    check(generate(back).text == generate(d).text && back.sections[0].blocks[1].disabled, "the blocks, their ids, flags and comments survive");
    check(back.nextId == d.nextId, "new blocks keep getting fresh ids");
    // damaged files say what is wrong
    Document bad;
    check(!parseDocument("{", bad, err) && err.find("line 1") != std::string::npos, "a damaged file: the message has the position", err);
    check(!parseDocument("[1,2]", bad, err), "not an object: refused", err);
    check(!parseDocument("{\"format\":\"other\"}", bad, err) && err.find("acblk") != std::string::npos, "another format is refused by name", err);
    check(!parseDocument("{\"format\":\"acblk\",\"version\":9,\"sections\":[]}", bad, err) && err.find("newer") != std::string::npos, "a newer version is refused with an explanation", err);
    check(!parseDocument("{\"format\":\"acblk\",\"version\":1,\"sections\":[{\"domain\":\"main\",\"blocks\":[{\"id\":\"b1\"}]}]}", bad, err) && err.find("sections[0].blocks[0]") != std::string::npos && err.find("type") != std::string::npos, "a block without a type: the message locates it", err);
    check(parseDocument("{\"format\":\"acblk\",\"version\":1,\"sections\":[{\"domain\":\"main\",\"blocks\":[{\"type\":\"ret\"},{\"type\":\"ret\",\"id\":\"b7\"}]}]}", bad, err) && bad.sections[0].blocks[0].id != "" && bad.sections[0].blocks[0].id != "b7", "blocks without an id get one", err);
    // a hand-written file with numbers as field values
    check(parseDocument("{\"format\":\"acblk\",\"version\":1,\"sections\":[{\"domain\":\"main\",\"blocks\":[{\"type\":\"csr_write8\",\"fields\":{\"csr\":\"X\",\"imm\":5}}]}]}", bad, err) && bad.sections[0].blocks[0].get("imm") == "5", "a number as a field value is accepted", err);
}

static void t_validate() {
    section("checking the structure");
    auto has = [](const std::vector<BlockProblem>& v, bool error, const std::string& needle) { for (auto& p : v) if (p.error == error && p.message.find(needle) != std::string::npos) return true; return false; };
    Document d = importAsm(".main\nstart: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n JMP start\n");
    check(validate(d).empty(), "an imported program has no structural problems");
    Document bad = d;
    bad.sections[0].blocks[1].type = "frobnicate";
    check(has(validate(bad), true, "unknown block type 'frobnicate'"), "unknown block type");
    bad = d;
    bad.sections[0].blocks[1].set("src", "");
    bad.sections[0].blocks[1].values.clear();                                       // the text and the components it was split into
    check(has(validate(bad), true, "'from' is empty"), "an empty required field, named as the block shows it");
    bad = d;
    Block x = bad.make("xfer");
    x.set("rw", "READ");
    x.set("args", "dev=0, len=1, ptr=0");
    bad.sections[0].blocks.push_back(x);
    check(has(validate(bad), true, "Peri block; it cannot be in the Main section"), "a Peri block in the Main section");
    bad = d;
    bad.sections[0].domain = "peri";
    check(has(validate(bad), true, "Main block; it cannot be in the Peri section"), "a Main block in the Peri section");
    bad = d;
    bad.sections.insert(bad.sections.begin(), Section());
    bad.sections[0].blocks.push_back(bad.make("ret"));
    check(has(validate(bad), true, "needs a .main or .peri section"), "an instruction before any section");
    bad = d;
    bad.sections[0].blocks[2].id = bad.sections[0].blocks[1].id;
    check(has(validate(bad), true, "is used by another block"), "two blocks with one id");
    bad = d;
    Block outer = bad.make("proc"), inner = bad.make("proc");
    outer.set("name", "a");
    inner.set("name", "b");
    outer.stack("BODY", true)->push_back(inner);
    bad.sections[0].blocks.push_back(outer);
    check(has(validate(bad), true, "procedures do not nest"), "a procedure inside a procedure");
    bad = d;
    bad.sections[0].blocks[1].fields.emplace_back("bogus", "1");
    check(has(validate(bad), false, "does not belong to this block"), "a field the block does not have is a warning (a newer file keeps working)");
    bad = d;
    Block rw = bad.make("alu");
    rw.set("op", "ADD");
    rw.set("mode", "wobbly");
    bad.sections[0].blocks.push_back(rw);
    check(!has(validate(bad), true, "wobbly"), "an open choice accepts other values (they are kept; the assembler decides)");
}

static void t_operands() {
    section("operands for editors");
    Operand o;
    check(parseOperand("MAIN_SCRATCHPAD[0..1]", o) && o.region == "MAIN_SCRATCHPAD" && o.first == "0" && o.last == "1" && o.isRange, "region with a byte range");
    check(parseOperand("PERI_RESULT_SLOT[ 3 ]", o) && o.first == "3" && o.last.empty() && formatOperand(o) == "PERI_RESULT_SLOT[3]", "a single byte");
    check(parseOperand("ALU_OPA", o) && !o.isRange && formatOperand(o) == "ALU_OPA", "a register without a range");
    check(!parseOperand("CONST(5, 2)", o) && !parseOperand("MAIN_CONST_POOL[@K]", o) && !parseOperand("MAIN_SCRATCHPAD[1..]", o) && !parseOperand("", o), "constants, @name and half ranges are not ranges (the editor shows them as text)");
    parseOperand("MAIN_SCRATCHPAD[0..1]", o);
    o.first = "4";
    o.last = "5";
    check(formatOperand(o) == "MAIN_SCRATCHPAD[4..5]", "formatting after an edit");
}

// ---- the real programs ----------------------------------------------------------------------------------------------------------------
static std::vector<std::string> codesOf(const Result& r) { std::vector<std::string> v; for (auto& d : r.diags) v.push_back(d.code); std::sort(v.begin(), v.end()); return v; }

static void t_roundtrip() {
    section("round trip over every example and sample program: same image, same diagnostics");
    const fs::path root = ASMCORE_SOURCE_DIR;
    int files = 0, same = 0, rawTotal = 0;
    std::string differing, rawNames;
    for (const char* dir : {"examples", "sample"}) {
        std::vector<fs::path> asms;
        for (auto& e : fs::recursive_directory_iterator(root / dir)) {
            if (!e.is_regular_file() || e.path().extension() != ".asm") continue;
            if (e.path().generic_string().find("/out/") != std::string::npos) continue;
            asms.push_back(e.path());
        }
        std::sort(asms.begin(), asms.end());
        for (const fs::path& p : asms) {
            files++;
            const std::string text = slurp(p);
            ImportStats st;
            Document d = importAsm(text, p.filename().string(), &st);
            const bool deliberateErrors = p.generic_string().find("/07_errors/") != std::string::npos ||        // those files are full of lines no block should cover,
                                          p.generic_string().find("/13_block_editor/") != std::string::npos;     // and this one keeps a line on purpose (a grey block to practise on)
            if (!deliberateErrors) rawTotal += st.raw;
            if (st.raw && !deliberateErrors) rawNames += p.filename().string() + "(" + std::to_string(st.raw) + ") ";
            const std::string regenerated = generate(d).text;
            // same includes for both: the file's own directory, its parent's "common", and the project's other files
            auto resolver = [&](const std::string&, const std::string& name, Source& out) {
                for (const fs::path& base : {p.parent_path(), p.parent_path() / ".." / "common", root / "examples"}) {
                    fs::path f = base / name;
                    if (fs::exists(f)) { out.name = f.string(); out.text = slurp(f); return true; }
                }
                return false;
            };
            // the multi-file project lists shared.inc first: give it the same way
            std::vector<Source> a{Source{p.string(), text}}, b{Source{p.string(), regenerated}};
            if (p.parent_path().filename() == "10_multi_file") {
                for (auto* v : {&a, &b}) v->insert(v->begin(), Source{"shared.inc", slurp(p.parent_path() / "shared.inc")});
            }
            if (p.parent_path().filename() == "11_hardware_designer") {
                for (auto* v : {&a, &b}) v->insert(v->begin(), Source{"board.gen.inc", slurp(p.parent_path() / "board.gen.inc")});
            }
            Options opt;
            Result ra = assemble(a, resolver, opt), rb = assemble(b, resolver, opt);
            const bool images = ra.main.bytes == rb.main.bytes && ra.peri.bytes == rb.peri.bytes;
            const bool diags = ra.ok == rb.ok && codesOf(ra) == codesOf(rb);
            const bool idem = generate(importAsm(regenerated)).text == regenerated;
            if (images && diags && idem) same++;
            else differing += p.filename().string() + (images ? "" : " [image]") + (diags ? "" : " [diagnostics]") + (idem ? "" : " [not idempotent]") + " ";
        }
    }
    check(files >= 15, "found the example and sample programs", std::to_string(files));
    check(same == files, "text -> blocks -> text assembles to the same images and the same diagnostics, and is stable", "differ: " + differing);
    check(rawTotal == 0, "the Level 1 blocks cover every line of the working programs (no Raw blocks; the deliberately broken 07_errors files and the practice sample 13 keep some lines as Raw)", rawNames);
    // the definition files too (they are directives, comments and blanks)
    int inc = 0, incOk = 0;
    for (auto& e : fs::recursive_directory_iterator(root / "sample")) {
        if (!e.is_regular_file() || e.path().extension() != ".inc") continue;
        inc++;
        const std::string text = slurp(e.path());
        Document d = importAsm(text);
        const std::string g = generate(d).text;
        // compare through an assembler run that includes the file after a trivial program
        Options opt;
        Result ra = assemble({Source{"a.inc", text}, Source{"p.asm", ".main\nl: JMP l\n"}}, nullptr, opt);
        Result rb = assemble({Source{"a.inc", g}, Source{"p.asm", ".main\nl: JMP l\n"}}, nullptr, opt);
        if (ra.main.bytes == rb.main.bytes && ra.peri.bytes == rb.peri.bytes && codesOf(ra) == codesOf(rb)) incOk++;
    }
    check(inc >= 2 && inc == incOk, "the .inc definition files round-trip as blocks too", std::to_string(incOk) + "/" + std::to_string(inc));
}

// A loop block: "for" counts with the scratchpad and the ALU, "forever" jumps back. The generated text assembles and runs the body the right number of times.
static Document loopProgram(const std::string& mode, const std::string& count, const std::string& counter = "", int nested = 0) {
    Document d = importAsm(".main\ns: MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]\n MOV CONST(1, 2) -> MAIN_SCRATCHPAD[2..3]\nh: JMP h\n", "loop");
    Block body = importAsm(".main\nx: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU ADD unsigned\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[0..1]\n", "b").sections[0].blocks[0];
    std::vector<Block> list = importAsm(".main\nx: MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU ADD unsigned\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[0..1]\n", "b").sections[0].blocks;
    list.erase(list.begin());                                                    // (without the label)
    std::function<void(Block&)> clearIds = [&](Block& x) {                        // (they came from another document: new ids, components included)
        x.id.clear();
        for (auto& st : x.stacks) for (Block& c : st.second) clearIds(c);
        for (auto& vl : x.values) for (Block& c : vl.second) clearIds(c);
    };
    for (Block& x : list) clearIds(x);
    (void)body;
    Block loop = d.make("loop");
    loop.set("mode", mode);
    loop.set("count", count);
    loop.set("counter", counter);
    *loop.stack("BODY", true) = list;
    std::vector<Block>& top = d.sections[0].blocks;
    Block cur = loop;
    for (int i = 0; i < nested; i++) { Block outer = d.make("loop"); outer.set("count", "2"); outer.set("counter", ""); outer.stack("BODY", true)->push_back(cur); cur = outer; }
    top.insert(top.begin() + 3, cur);
    d.assignMissingIds();
    return d;
}
static long long scratchWord(const Result& r, int at) {
    sim::Simulator sm(r, sim::SimConfig());
    sim::RunLimits lim;
    lim.maxCycles = 100000;
    lim.maxInstrs = 5000;
    sm.run(lim);
    int64_t v = 0;
    std::string err;
    sm.eval("scratch[" + std::to_string(at) + "] | (scratch[" + std::to_string(at + 1) + "] << 8)", v, err);
    return v;
}
static void t_loop() {
    std::printf("\nLoop blocks\n");
    const Registry& reg = Registry::defaults();
    const BlockDef* def = reg.find("loop");
    check(def && def->category == "Control" && def->domains == kMain && def->stacks.size() == 1, "the loop block exists: a Control block of Main with a slot for its body");
    Document d = loopProgram("for", "7");
    const std::string text = generate(d).text;
    check(text.find("MOV CONST(7, 2) -> MAIN_SCRATCHPAD[30..31]") != std::string::npos && text.find("BR.COND ALU_CMP_TRUE, loop_") != std::string::npos, "for 7 times: the limit is loaded and the loop exits on the compare", text);
    Result r = assemble({Source{"p.asm", text}}, nullptr, Options());
    check(r.ok, "the generated loop assembles", r.diagnosticsText());
    check(r.ok && scratchWord(r, 0) == 7, "...and runs its body 7 times", std::to_string(scratchWord(r, 0)));
    std::string vp;
    for (const BlockProblem& p : validate(d, reg)) vp += p.message + " | ";
    check(validate(d, reg).empty(), "a loop with a count has no problems", vp);
    // the generated lines all belong to the loop block (a build message lands on it), the body's to the blocks inside
    const Generated g = generate(d);
    int onLoop = 0;
    const std::string loopId = d.sections[0].blocks[3].id;
    for (const GenLine& l : g.lines) onLoop += l.blockId == loopId;
    check(onLoop >= 12, "the lines of the counting belong to the loop block", std::to_string(onLoop));
    // forever
    Document f = loopProgram("forever", "");
    const std::string ft = generate(f).text;
    check(ft.find("CONST(") != std::string::npos && ft.find("ALU CMP.EQ") == std::string::npos && ft.find("JMP loop_") != std::string::npos, "forever: just jumps back (no counting)", ft);
    std::string fp = assemble({Source{"p.asm", ft}}, nullptr, Options()).diagnosticsText();
    for (const BlockProblem& p : validate(f, reg)) fp += " | " + p.message;
    check(assemble({Source{"p.asm", ft}}, nullptr, Options()).ok && validate(f, reg).empty(), "...and assembles (the line after it is reported unreachable, as it is)", fp);
    // nesting: each loop gets its own counter bytes
    Document n = loopProgram("for", "3", "", 1);
    const std::string nt = generate(n).text;
    check(nt.find("MAIN_SCRATCHPAD[26..27]") != std::string::npos && nt.find("MAIN_SCRATCHPAD[20..21]") != std::string::npos, "a loop inside a loop counts in other bytes (26, then 20)", nt);
    Result nr = assemble({Source{"p.asm", nt}}, nullptr, Options());
    check(nr.ok && validate(n, reg).empty(), "...and the nest assembles", nr.diagnosticsText());
    check(nr.ok && scratchWord(nr, 0) == 6, "an inner body of 3 inside 2 outer rounds runs 6 times", std::to_string(scratchWord(nr, 0)));
    // problems
    Document bad = loopProgram("for", "5", "30");
    bool tooHigh = false;
    for (const BlockProblem& p : validate(bad, reg)) tooHigh |= p.message.find("0..26") != std::string::npos;
    check(tooHigh, "a counter place outside the scratchpad is a problem");
    Document clash = loopProgram("for", "3", "20", 1);
    clash.sections[0].blocks[3].stack("BODY")->at(0).set("counter", "22");
    bool overlap = false;
    for (const BlockProblem& p : validate(clash, reg)) overlap |= p.message.find("overlaps") != std::string::npos;
    check(overlap, "a loop inside a loop may not share its counter bytes");
    Document noCount = loopProgram("for", "");
    noCount.sections[0].blocks[3].set("count", "");
    bool empty = false;
    for (const BlockProblem& p : validate(noCount, reg)) empty |= p.message.find("times") != std::string::npos;
    check(empty, "a counting loop without a count is a problem");
    // the file keeps an empty body, and the mode / count
    Document e = loopProgram("for", "9");
    e.sections[0].blocks[3].stack("BODY")->clear();
    Document back;
    std::string err;
    check(parseDocument(serializeDocument(e), back, err) && back.sections[0].blocks[3].type == "loop" && back.sections[0].blocks[3].get("count") == "9" && back.sections[0].blocks[3].stack("BODY") != nullptr, "the file keeps a loop with an empty body, its mode and its count", err);
}

// if / while / wait until: lowered to labels + BR.COND + JMP; what they do is checked in the simulator.
static std::vector<Block> blocksOf(Document& d, const std::string& body) {
    std::vector<Block> list = importAsm(".main\nx:\n" + body, "b").sections[0].blocks;
    list.erase(list.begin());                                                      // (the label)
    std::function<void(Block&)> fresh = [&](Block& x) {
        x.id = d.newId();
        for (auto& st : x.stacks) for (Block& c : st.second) fresh(c);
        for (auto& vl : x.values) for (Block& c : vl.second) fresh(c);
    };
    for (Block& x : list) fresh(x);
    return list;
}
static std::string structuredText(const std::string& head, const std::string& type, const std::vector<std::pair<std::string, std::string>>& slots, const std::string& cond = "ALU_CMP_TRUE") {
    Document d = importAsm(".main\ns:\n" + head + "h: JMP h\n", "s");
    Block b = d.make(type);
    b.set("cond", cond);
    for (const auto& sl : slots) *b.stack(sl.first, true) = blocksOf(d, sl.second);
    std::vector<Block>& top = d.sections[0].blocks;
    size_t at = top.size() - 1;                                                    // before "h: JMP h"
    for (size_t i = 0; i < top.size(); i++) if (top[i].type == "label" && top[i].get("name") == "h") at = i;
    top.insert(top.begin() + (long)at, b);
    d.assignMissingIds();
    return generate(d).text;
}
static long long runWord(const std::string& text, int at) {
    Result r = assemble({Source{"p.asm", text}}, nullptr, Options());
    if (!r.ok) { std::printf("        %s\n", r.diagnosticsText().c_str()); return -1; }
    return scratchWord(r, at);
}
static void t_structured() {
    std::printf("\nStructured blocks: if / while / wait until\n");
    const Registry& reg = Registry::defaults();
    for (const char* ty : {"if", "while", "wait"}) {
        const BlockDef* def = reg.find(ty);
        check(def && def->category == "Control" && def->domains == kMain && !def->stacks.empty(), std::string("the '") + ty + "' block exists: a Control block of Main with slots");
    }
    const std::string two = "MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[2..3]\n";
    const std::string six = "MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n MOV CONST(6, 2) -> MAIN_SCRATCHPAD[2..3]\n";
    const std::string cmp = " MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU CMP.EQ unsigned\n";
    const std::vector<std::pair<std::string, std::string>> ifSlots = {{"THEN", "MOV CONST(1, 2) -> MAIN_SCRATCHPAD[4..5]\n"}, {"ELSE", "MOV CONST(2, 2) -> MAIN_SCRATCHPAD[4..5]\n"}};
    const std::string t1 = structuredText(two + cmp, "if", ifSlots), t2 = structuredText(six + cmp, "if", ifSlots);
    check(t1.find("BR.COND ALU_CMP_TRUE, if_") != std::string::npos && t1.find("_then:") != std::string::npos && t1.find("_end:") != std::string::npos, "if: BR.COND to the THEN part, the ELSE part falls through, then the end", t1);
    check(runWord(t1, 4) == 1 && runWord(t2, 4) == 2, "if: THEN runs when the condition holds, ELSE when it does not");
    check(runWord(structuredText(six + cmp, "if", {{"THEN", "MOV CONST(1, 2) -> MAIN_SCRATCHPAD[4..5]\n"}, {"ELSE", ""}}), 4) == 0, "if: an empty ELSE does nothing");
    // while: counter < limit
    const std::string init = "MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]\n MOV CONST(1, 2) -> MAIN_SCRATCHPAD[2..3]\n MOV CONST(3, 2) -> MAIN_SCRATCHPAD[4..5]\n";
    const std::string lt = " MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB\n ALU CMP.LT unsigned\n";
    const std::string inc = " MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\n ALU ADD unsigned\n MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[0..1]\n";
    const std::string w = structuredText(init, "while", {{"COND", lt}, {"BODY", inc}});
    check(runWord(w, 0) == 3, "while: the body runs while the condition holds (3 times: the counter reaches 3)", w);
    const std::string w0 = structuredText("MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]\n MOV CONST(1, 2) -> MAIN_SCRATCHPAD[2..3]\n MOV CONST(0, 2) -> MAIN_SCRATCHPAD[4..5]\n", "while", {{"COND", lt}, {"BODY", inc}});
    check(runWord(w0, 0) == 0, "while: never runs the body when the condition is false at the start");
    // wait until: COND works out the condition each round; here it counts and compares with the limit
    const std::string eq = " MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB\n ALU CMP.EQ unsigned\n";
    const std::string wt = structuredText(init, "wait", {{"COND", inc + eq}});
    check(runWord(wt, 0) == 3, "wait until: COND runs again and again until the condition holds", wt);
    check(wt.find("JMP wait_") != std::string::npos && wt.find("BR.COND ALU_CMP_TRUE, wait_") != std::string::npos, "wait until: BR.COND leaves, JMP goes round");
    const std::string hw = structuredText(init, "wait", {}, "ALWAYS");
    check(assemble({Source{"p.asm", hw}}, nullptr, Options()).ok, "wait until with a hardware condition and nothing to work out assembles");
    // checks
    Document noAlu = importAsm(".main\ns: MOV CONST(1, 2) -> MAIN_SCRATCHPAD[0..1]\nh: JMP h\n", "x");
    Block ib = noAlu.make("if");
    noAlu.sections[0].blocks.insert(noAlu.sections[0].blocks.begin() + 2, ib);
    bool hint = false;
    for (const BlockProblem& p : validate(noAlu, reg)) hint |= p.message.find("W506") != std::string::npos && p.blockId == ib.id;
    check(hint, "an if on the ALU flag with no ALU operation before it gets the W506 hint");
    Document wd = importAsm(".main\ns:\nh: JMP h\n", "x");
    Block wb = wd.make("while");
    wd.sections[0].blocks.insert(wd.sections[0].blocks.begin() + 1, wb);
    hint = false;
    for (const BlockProblem& p : validate(wd, reg)) hint |= p.message.find("nothing in COND") != std::string::npos;
    check(hint, "a while on the ALU flag with an empty COND gets a hint");
    wd.sections[0].blocks[1].set("cond", "ALWAYS");
    bool any = false;
    for (const BlockProblem& p : validate(wd, reg)) any |= p.blockId == wb.id;
    check(!any, "...and none for a hardware condition");
    // the file keeps the empty slots and the condition
    Document e = importAsm(".main\ns:\nh: JMP h\n", "x");
    Block eb = e.make("if");
    eb.set("cond", "PERI_RESULT_VALID");
    *eb.stack("THEN", true) = {};
    *eb.stack("ELSE", true) = {};
    e.sections[0].blocks.insert(e.sections[0].blocks.begin() + 1, eb);
    Document back;
    std::string err;
    const Block* bb = parseDocument(serializeDocument(e), back, err) ? &back.sections[0].blocks[1] : nullptr;
    check(bb && bb->type == "if" && bb->get("cond") == "PERI_RESULT_VALID" && bb->stack("THEN") && bb->stack("ELSE"), "the file keeps an if with empty slots and its condition", err);
}

// Variables and expressions: the generator picks the scratchpad bytes; the program is run in the simulator and the values are read back.
namespace vb {
Block num(Document& d, const std::string& v) { Block b = d.make("num"); b.set("value", v); return b; }
Block var(Document& d, const std::string& n) { Block b = d.make("v_var"); b.set("name", n); return b; }
Block bin(Document& d, const std::string& op, Block a, Block b2, const std::string& mode = "") {
    Block b = d.make("v_bin");
    b.set("op", op);
    if (!mode.empty()) b.set("mode", mode);
    b.setValue("a", a);
    b.setValue("b", b2);
    return b;
}
Block cmp(Document& d, const std::string& op, Block a, Block b2) { Block b = d.make("v_cmp"); b.set("op", op); b.setValue("a", a); b.setValue("b", b2); return b; }
Block set(Document& d, const std::string& n, Block v) { Block b = d.make("set"); b.set("name", n); b.setValue("value", v); return b; }
Block change(Document& d, const std::string& n, Block v) { Block b = d.make("change"); b.set("name", n); b.setValue("value", v); return b; }
// ".main s: <the blocks> h: JMP h"
Document program(const std::function<std::vector<Block>(Document&)>& make, const std::string& before = "") {
    Document d = importAsm(".main\ns:\n" + before + "h: JMP h\n", "v");
    std::vector<Block> blocks = make(d);
    std::vector<Block>& top = d.sections[0].blocks;
    size_t at = top.size() - 1;
    for (size_t i = 0; i < top.size(); i++) if (top[i].type == "label" && top[i].get("name") == "h") at = i;
    top.insert(top.begin() + (long)at, blocks.begin(), blocks.end());
    d.assignMissingIds();
    return d;
}
}  // namespace vb

static void t_variables() {
    std::printf("\nVariables and expressions\n");
    using namespace vb;
    const Registry& reg = Registry::defaults();
    for (const char* ty : {"set", "change"}) { const BlockDef* d = reg.find(ty); check(d && d->category == "Variables" && d->domains == kMain && d->inputs.size() == 1 && !d->inputs[0].optional, std::string("the '") + ty + "' block exists: a value is needed"); }
    for (const char* ty : {"v_var", "v_bin", "v_cmp"}) { const BlockDef* d = reg.find(ty); check(d && d->category == "Expressions" && !d->valueType.empty(), std::string("the '") + ty + "' component exists"); }
    check(reg.find("v_cmp")->valueType == "bool" && reg.find("v_bin")->valueType == "val", "a comparison is a bool, a calculation a val (a comparison does not fit where a number is wanted)");
    // x = 5; y = x + 3; change x by 2  ->  x = 7, y = 8
    Document a = program([](Document& d) { return std::vector<Block>{set(d, "x", num(d, "5")), set(d, "y", bin(d, "+", var(d, "x"), num(d, "3"))), change(d, "x", num(d, "2"))}; });
    const std::string ta = generate(a).text;
    check(runWord(ta, 0) == 7 && runWord(ta, 2) == 8, "set / change: x = 5, y = x + 3, x += 2 gives x = 7 and y = 8", ta);
    check(validate(a, reg).empty(), "...with no problems");
    // nested: z = (x + y) * 2 (the inner result is a working value), and the other operations
    Document b = program([](Document& d) {
        return std::vector<Block>{set(d, "x", num(d, "5")), set(d, "y", num(d, "6")), set(d, "z", bin(d, "*", bin(d, "+", var(d, "x"), var(d, "y")), num(d, "2"))),
                                  set(d, "m", bin(d, "&", var(d, "z"), num(d, "0x0F"))), set(d, "s", bin(d, "<<", var(d, "x"), num(d, "3"))), set(d, "d", bin(d, "-", var(d, "z"), num(d, "1"), "signed"))};
    });
    const std::string tb = generate(b).text;
    check(runWord(tb, 4) == 22 && runWord(tb, 6) == (22 & 15) && runWord(tb, 8) == 40 && runWord(tb, 10) == 21, "nested expressions: (x+y)*2 = 22, & 0x0F = 6, x << 3 = 40, z - 1 = 21", tb);
    check(validate(b, reg).empty(), "...with no problems");
    // if with a comparison plugged into "test"
    auto ifProgram = [](const std::string& op) {
        return program([&](Document& d) {
            Block f = d.make("if");
            f.setValue("test", cmp(d, op, var(d, "x"), var(d, "y")));
            *f.stack("THEN", true) = {set(d, "r", num(d, "1"))};
            *f.stack("ELSE", true) = {set(d, "r", num(d, "2"))};
            return std::vector<Block>{set(d, "x", num(d, "3")), set(d, "y", num(d, "5")), f};
        });
    };
    check(runWord(generate(ifProgram("<")).text, 4) == 1 && runWord(generate(ifProgram(">")).text, 4) == 2 && runWord(generate(ifProgram("<=")).text, 4) == 1 && runWord(generate(ifProgram(">=")).text, 4) == 2 &&
          runWord(generate(ifProgram("==")).text, 4) == 2 && runWord(generate(ifProgram("!=")).text, 4) == 1, "if with a comparison: < > <= >= == != all pick the right branch (x = 3, y = 5)");
    check(validate(ifProgram("<"), reg).empty(), "an if with a plugged test needs no ALU operation before it (no W506)");
    // while x < 10: x += 3  ->  12
    Document w = program([](Document& d) {
        Block l = d.make("while");
        l.setValue("test", cmp(d, "<", var(d, "x"), num(d, "10")));
        *l.stack("BODY", true) = {change(d, "x", num(d, "3"))};
        return std::vector<Block>{set(d, "x", num(d, "0")), l};
    });
    check(runWord(generate(w).text, 0) == 12, "while x < 10: x += 3 ends with x = 12", generate(w).text);
    // wait until: the work in COND, the test after it
    Document wt = program([](Document& d) {
        Block l = d.make("wait");
        l.setValue("test", cmp(d, "==", var(d, "x"), num(d, "4")));
        *l.stack("COND", true) = {change(d, "x", num(d, "1"))};
        return std::vector<Block>{set(d, "x", num(d, "0")), l};
    });
    check(runWord(generate(wt).text, 0) == 4, "wait until x == 4 (COND adds one each round) stops with x = 4");
    // the generator keeps off the bytes other blocks name, and off a counting loop's
    Document hand = program([](Document& d) { return std::vector<Block>{set(d, "x", num(d, "9"))}; }, "MOV CONST(1, 2) -> MAIN_SCRATCHPAD[0..3]\n");
    const std::string th = generate(hand).text;
    check(th.find("MOV CONST(9, 2) -> MAIN_SCRATCHPAD[4..5]") != std::string::npos, "a variable does not use bytes a MOV names (MAIN_SCRATCHPAD[0..3]): it goes to 4..5", th);
    Document lp = program([](Document& d) {
        Block l = d.make("loop");
        l.set("count", "2");
        *l.stack("BODY", true) = {change(d, "n", num(d, "1"))};
        return std::vector<Block>{set(d, "n", num(d, "0")), l};
    });
    const std::string tl = generate(lp).text;
    check(runWord(tl, 0) == 2 && tl.find("MAIN_SCRATCHPAD[26..27]") != std::string::npos && tl.find("CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]") != std::string::npos, "a variable and a counting loop live side by side (n at 0, the counter at 26..31): the loop adds 1 twice", tl);
    // problems
    auto problemsOf = [&](const Document& d, const std::string& needle) { for (const BlockProblem& p : validate(d, reg)) if (p.message.find(needle) != std::string::npos) return true; return false; };
    Document unset = program([](Document& d) { return std::vector<Block>{set(d, "y", var(d, "ghost"))}; });
    check(problemsOf(unset, "nothing sets the variable 'ghost'"), "a variable nothing sets is reported");
    Document badName = program([](Document& d) { return std::vector<Block>{set(d, "1x", num(d, "1"))}; });
    check(problemsOf(badName, "not a name for a variable"), "a bad variable name is reported");
    Document many = program([](Document& d) { std::vector<Block> v; for (int i = 0; i < 17; i++) v.push_back(set(d, "v" + std::to_string(i), num(d, "1"))); return v; });
    check(problemsOf(many, "no room in the scratchpad for the variable"), "17 variables do not fit in 32 bytes: the 17th is reported");
    Document shift = program([](Document& d) { return std::vector<Block>{set(d, "x", num(d, "1")), set(d, "y", bin(d, "<<", var(d, "x"), var(d, "x")))}; });
    check(problemsOf(shift, "a shift needs a plain number"), "a shift by something that is not a number is reported");
    Document noValue = program([](Document& d) { Block s = d.make("set"); s.set("name", "x"); return std::vector<Block>{s}; });
    check(problemsOf(noValue, "needs a block plugged in") && generate(noValue).text.find("(incomplete) set x") != std::string::npos, "a set without a value is incomplete (said on the block and in the text)");
    // the file keeps all of it
    Document back;
    std::string err;
    check(parseDocument(serializeDocument(b), back, err) && generate(back).text == tb, "the file keeps variables and expressions: the same program comes back", err);
}

// The cascade container: parts cut by "next block", run in order up to the selector. Sample 04's cascade is made again with it: the image must be the text version's.
static void t_cascade() {
    std::printf("\nCascade block\n");
    const Registry& reg = Registry::defaults();
    const BlockDef* cd = reg.find("cascade");
    check(cd && cd->category == "Control" && cd->domains == kMain && cd->stacks.size() == 1 && reg.find("cascade_next"), "the cascade and 'next block' blocks exist (Control, Main)");
    // sample 04: replace CASCADE_BEGIN ... CASCADE_END 2 (and their labels) by one cascade block
    const std::string text = slurp(fs::path(ASMCORE_SOURCE_DIR) / "sample" / "04_alu_cascade" / "alu_cascade.asm");
    const std::string defs = slurp(fs::path(ASMCORE_SOURCE_DIR) / "sample" / "common" / "defs.inc");
    auto build = [&](const std::string& src) { Options o; return assemble({Source{"defs.inc", defs}, Source{"a.asm", src}}, nullptr, o); };
    std::string noInclude = text;
    const size_t inc = noInclude.find(".include \"defs.inc\"");
    if (inc != std::string::npos) noInclude.erase(inc, std::string(".include \"defs.inc\"\n").size());
    const Result original = build(noInclude);
    check(original.ok, "sample 04 assembles", original.diagnosticsText());
    Document d = importAsm(noInclude, "c");
    std::vector<Block>* main = nullptr;
    for (Section& s : d.sections) if (s.domain == "main") main = &s.blocks;
    size_t from = 0, to = 0;
    std::vector<Block> parts;
    bool found = false;
    for (size_t i = 0; main && i < main->size(); i++) {
        if ((*main)[i].type == "cascade_begin") from = i;
        if ((*main)[i].type == "cascade_end" && (*main)[i].get("index") == "2") to = i;
    }
    for (size_t i = from + 1; main && i < to; i++) {
        const Block& x = (*main)[i];
        if (x.type == "csr_set8") parts.push_back(x);
        if (x.type == "cascade_end" && x.get("index") != "2") { Block nx = d.make("cascade_next"); parts.push_back(nx); }
    }
    if (main && to > from) {
        Block cs = d.make("cascade");
        cs.set("k", "2");
        *cs.stack("BODY", true) = parts;
        main->erase(main->begin() + (long)from, main->begin() + (long)to + 1);
        main->insert(main->begin() + (long)from, cs);
        found = true;
    }
    d.assignMissingIds();
    const std::string gen = generate(d).text;
    check(found && gen.find("CASCADE_BEGIN 2, cascade_") != std::string::npos && gen.find("CASCADE_END 2, cascade_") != std::string::npos, "the cascade block lowers to CASCADE_BEGIN / CASCADE_END with parts 0, 1, 2", gen);
    const Result blocks = build(gen);
    check(blocks.ok, "...and assembles", blocks.diagnosticsText());
    check(blocks.ok && original.ok && blocks.main.bytes == original.main.bytes, "sample 04 made with the cascade block has the image of the text version");
    check(validate(d, reg).empty() || [&] { for (const BlockProblem& p : validate(d, reg)) if (p.error) return false; return true; }(), "...with no errors");
    // what it does: selector k runs parts 0..k
    auto cascadeProgram = [&](const std::string& k) {
        return vb::program([&](Document& dd) {
            Block cs = dd.make("cascade");
            cs.set("k", k);
            std::vector<Block> body = blocksOf(dd, "MOV CONST(1, 2) -> MAIN_SCRATCHPAD[0..1]\n");
            body.push_back(dd.make("cascade_next"));
            for (Block& x : blocksOf(dd, "MOV CONST(2, 2) -> MAIN_SCRATCHPAD[2..3]\n")) body.push_back(x);
            body.push_back(dd.make("cascade_next"));
            for (Block& x : blocksOf(dd, "MOV CONST(3, 2) -> MAIN_SCRATCHPAD[4..5]\n")) body.push_back(x);
            *cs.stack("BODY", true) = body;
            return std::vector<Block>{cs};
        });
    };
    const std::string c0 = generate(cascadeProgram("0")).text, c1 = generate(cascadeProgram("1")).text, c2 = generate(cascadeProgram("2")).text;
    check(runWord(c0, 0) == 1 && runWord(c0, 2) == 0 && runWord(c0, 4) == 0, "selector 0 runs only the first part", c0);
    check(runWord(c1, 0) == 1 && runWord(c1, 2) == 2 && runWord(c1, 4) == 0, "selector 1 runs the first two");
    check(runWord(c2, 0) == 1 && runWord(c2, 2) == 2 && runWord(c2, 4) == 3, "selector 2 runs all three");
    // an empty cascade is still a valid program; problems are said on the block
    Document empty = vb::program([](Document& dd) { Block cs = dd.make("cascade"); *cs.stack("BODY", true) = {}; return std::vector<Block>{cs}; });
    check(build(generate(empty).text).ok, "an empty cascade assembles");
    auto said = [&](const Document& dd, const std::string& needle) { for (const BlockProblem& p : validate(dd, reg)) if (p.message.find(needle) != std::string::npos) return true; return false; };
    check(said(cascadeProgram("5"), "'run blocks up to' is 5"), "a selector past the last part is reported");
    Document branchy = vb::program([](Document& dd) {
        Block cs = dd.make("cascade");
        Block j = dd.make("jmp");
        j.set("target", "s");
        *cs.stack("BODY", true) = {j};
        return std::vector<Block>{cs};
    });
    check(said(branchy, "no branches, loops, labels or calls"), "a branch inside a cascade is reported");
    Document stray = vb::program([](Document& dd) { return std::vector<Block>{dd.make("cascade_next")}; });
    check(said(stray, "only makes sense inside a cascade"), "'next block' outside a cascade is reported");
    Document back;
    std::string err;
    check(parseDocument(serializeDocument(d), back, err) && generate(back).text == gen, "the file keeps the cascade and its parts", err);
}

int main() {
    std::printf("AssemblyCore block program tests\n");
    t_coverage();
    t_import();
    t_generate();
    t_file();
    t_validate();
    t_operands();
    t_roundtrip();
    t_loop();
    t_structured();
    t_variables();
    t_cascade();
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
