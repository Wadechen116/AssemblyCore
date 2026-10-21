// Block programs: text -> blocks -> text must keep the program (same images), the file format must round-trip, the definitions must cover the
// whole language.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "asmcore/asmcore.h"
#include "asmcore/blocks.h"
#include "asmcore/lang.h"

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
        if (d.title.empty() || d.category.empty()) ok = false;
        for (const FieldDef& f : d.fields) if (f.name.empty() || f.label.empty()) ok = false;
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
    std::function<void(const std::vector<Block>&)> walk = [&](const std::vector<Block>& v) { for (const Block& b : v) { if (!ids.insert(b.id).second || b.id.empty()) unique = false; for (auto& s : b.stacks) walk(s.second); } };
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
            if (e.path().string().find("/out/") != std::string::npos) continue;
            asms.push_back(e.path());
        }
        std::sort(asms.begin(), asms.end());
        for (const fs::path& p : asms) {
            files++;
            const std::string text = slurp(p);
            ImportStats st;
            Document d = importAsm(text, p.filename().string(), &st);
            const bool deliberateErrors = p.string().find("/07_errors/") != std::string::npos;        // those files are full of lines no block should cover
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
    check(rawTotal == 0, "the Level 1 blocks cover every line of the working programs (no Raw blocks; the deliberately broken 07_errors files keep their bad lines as Raw)", rawNames);
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

int main() {
    std::printf("AssemblyCore block program tests\n");
    t_coverage();
    t_import();
    t_generate();
    t_file();
    t_validate();
    t_operands();
    t_roundtrip();
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
