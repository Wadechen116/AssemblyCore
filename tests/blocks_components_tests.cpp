// Component blocks (stage M2b-1): numbers with attributes, expressions, regions with ranges, windows, the text <-> component conversion, the
// rules of what may be plugged where, and compatibility with tools that know only the text.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <random>
#include <sstream>

#include "../core/src/util.h"
#include "asmcore/asmcore.h"
#include "asmcore/blocks.h"
#include "asmcore/blocks_edit.h"

#ifndef ASMCORE_SOURCE_DIR
#define ASMCORE_SOURCE_DIR "."
#endif

using namespace asmcore;
using namespace asmcore::blocks;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& why = "") {
    if (ok) { g_pass++; std::printf("  PASS  %s\n", what.c_str()); return; }
    g_fail++;
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), why.c_str());
}
static void section(const char* s) { std::printf("\n%s\n", s); }

static std::string squeezed(const std::string& s) {
    std::string o;
    for (char c : s) if (!std::isspace((unsigned char)c)) o += c;
    return o;
}
static std::string genText(const Document& d) { return generate(d).text; }
static const Block& first(const Document& d) { return d.sections[0].blocks[0]; }
static std::string problemsOf(const Document& d) {
    std::string s;
    for (const BlockProblem& p : validate(d)) s += p.message + "; ";
    return s;
}
static bool hasProblem(const Document& d, const std::string& needle, bool error = true) {
    for (const BlockProblem& p : validate(d)) if (p.error == error && p.message.find(needle) != std::string::npos) return true;
    return false;
}
static int countComponents(const Block& b) {
    int n = 0;
    for (const auto& vl : b.values) for (const Block& c : vl.second) n += 1 + countComponents(c);
    return n;
}

// ---- numbers --------------------------------------------------------------------------------------------------------------------------
static void t_numbers() {
    section("number literals");
    long long v = 0;
    check(parseNumberLiteral("123", v) && v == 123 && parseNumberLiteral("0x7F", v) && v == 127 && parseNumberLiteral("7Fh", v) && v == 127 && parseNumberLiteral("0b101", v) && v == 5 && parseNumberLiteral("101b", v) && v == 5 && parseNumberLiteral("'A'", v) && v == 65, "decimal, 0x, 7Fh, 0b, 101b, 'A'");
    check(!parseNumberLiteral("", v) && !parseNumberLiteral("abc", v) && !parseNumberLiteral("1+2", v) && !parseNumberLiteral("-5", v) && !parseNumberLiteral("5 6", v), "not literals: empty, a name, an expression, a negative, two words");

    section("a number block and its attributes");
    Document d = importAsm(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n");
    const Block& mov = first(d);
    const Block* src = mov.value("src");
    check(src && src->type == "num" && src->get("value") == "5" && src->get("width") == "16", "CONST(5, 2) is a number of 16 bits");
    check(genText(d) == ".main\n    MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n", "...and gives the same text", genText(d));
    // each attribute
    Document e = d;
    Block* n = const_cast<Block*>(e.sections[0].blocks[0].value("src"));
    n->set("width", "8");
    n->set("sign", "unsigned");
    n->set("value", "300");
    check(hasProblem(e, "300 does not fit 8 bits (0..255)"), "300 does not fit 8 unsigned bits", problemsOf(e));
    n->set("value", "-1");
    check(hasProblem(e, "does not fit 8 bits"), "-1 does not fit unsigned", problemsOf(e));
    n->set("sign", "signed");
    n->set("value", "-128");
    check(!hasProblem(e, "does not fit"), "-128 fits signed 8 bits", problemsOf(e));
    n->set("value", "128");
    check(hasProblem(e, "does not fit 8 bits (-128..127)"), "128 does not fit signed 8 bits", problemsOf(e));
    n->set("sign", "");
    n->set("value", "200");
    check(!hasProblem(e, "does not fit"), "with no sign stated both -128..255 are accepted (as the assembler does)", problemsOf(e));
    n->set("width", "12");
    check(hasProblem(e, "the width of a number is 8, 16, 24, 32 or 64"), "a width must be one of the offered", problemsOf(e));
    n->set("width", "16");
    n->set("kind", "decimal");
    check(hasProblem(e, "int, float or double"), "kind must be int, float or double", problemsOf(e));
    n->set("kind", "");
    n->set("width", "");
    n->set("value", "0x1F");
    check(genText(e).find("CONST(0x1F)") != std::string::npos, "no width stated: CONST without bytes (the destination decides)", genText(e));

    section("float and double");
    n->set("kind", "float");
    n->set("value", "3.14");
    check(genText(e).find("CONST(0x4048F5C3, 4)") != std::string::npos && !hasProblem(e, "bytes"), "float 3.14 is its IEEE-754 pattern in 4 bytes", genText(e));
    n->set("value", "-2.5");
    check(genText(e).find("0xC0200000") != std::string::npos, "float -2.5 = 0xC0200000", genText(e));
    n->set("value", "abc");
    check(hasProblem(e, "is not a float number"), "a float needs a number", problemsOf(e));
    n->set("kind", "double");
    n->set("value", "1.5");
    check(hasProblem(e, "holds 1 to 4 bytes") && genText(e).find("0x3FF8000000000000") != std::string::npos, "a double needs 8 bytes: refused as a constant, with the reason", problemsOf(e));
    n->set("kind", "int");
    n->set("width", "64");
    n->set("value", "1");
    check(hasProblem(e, "holds 1 to 4 bytes"), "a 64-bit constant is refused with the reason", problemsOf(e));
    n->set("width", "32");
    check(!hasProblem(e, "holds 1 to 4 bytes"), "...32 bits is fine", problemsOf(e));
}

// ---- expressions ------------------------------------------------------------------------------------------------------------------------
struct Tree {
    char kind = 'n';                         // n number, v name, b binary, u unary, p paren
    std::string text;                        // number / name / operator
    std::vector<Tree> kids;
};

static long long lookupVal(const std::string& n) { return n == "a" ? 3 : n == "b" ? 5 : 7; }

static long long evalTree(const Tree& t) {
    switch (t.kind) {
        case 'n': { long long v = 0; parseNumberLiteral(t.text, v); return v; }
        case 'v': return lookupVal(t.text);
        case 'p': return evalTree(t.kids[0]);
        case 'u': { long long x = evalTree(t.kids[0]); return t.text == "-" ? -x : t.text == "~" ? ~x : x; }
        default: {
            const long long x = evalTree(t.kids[0]), y = evalTree(t.kids[1]);
            const std::string& o = t.text;
            if (o == "+") return x + y;
            if (o == "-") return x - y;
            if (o == "*") return x * y;
            if (o == "/") return x / y;
            if (o == "%") return x % y;
            if (o == "<<") return (long long)((unsigned long long)x << y);
            if (o == ">>") return x >> y;
            if (o == "&") return x & y;
            if (o == "^") return x ^ y;
            return x | y;
        }
    }
}

static Tree randomTree(std::mt19937& rng, int depth, bool allowParen) {
    auto pick = [&](int n) { return (int)(rng() % (unsigned)n); };
    Tree t;
    if (depth == 0 || pick(4) == 0) {
        if (pick(2)) { t.kind = 'n'; static const char* lits[] = {"1", "2", "3", "7", "10", "0x10", "255", "'A'", "101b", "0Fh"}; t.text = lits[pick(10)]; }
        else { t.kind = 'v'; t.text = std::string(1, "abc"[pick(3)]); }
        return t;
    }
    const int k = pick(8);
    if (k == 0) { t.kind = 'u'; static const char* u[] = {"-", "~", "+"}; t.text = u[pick(3)]; t.kids.push_back(randomTree(rng, depth - 1, allowParen)); return t; }
    if (k == 1 && allowParen) { t.kind = 'p'; t.kids.push_back(randomTree(rng, depth - 1, allowParen)); return t; }
    t.kind = 'b';
    static const char* ops[] = {"+", "-", "*", "/", "%", "<<", ">>", "&", "^", "|"};
    t.text = ops[pick(10)];
    t.kids.push_back(randomTree(rng, depth - 1, allowParen));
    Tree r;
    if (t.text == "/" || t.text == "%") { r.kind = 'n'; static const char* nz[] = {"2", "3", "7"}; r.text = nz[pick(3)]; }          // never a division by zero
    else if (t.text == "<<" || t.text == ">>") { r.kind = 'n'; static const char* sh[] = {"0", "1", "2", "3"}; r.text = sh[pick(4)]; }
    else r = randomTree(rng, depth - 1, allowParen);
    t.kids.push_back(r);
    return t;
}

// The text with spaces and (for 'p') parentheses exactly as the tree says; no parentheses are added for precedence.
static std::string flatText(const Tree& t, std::mt19937& rng) {
    auto sp = [&]() { return std::string((rng() % 3) ? " " : ""); };
    switch (t.kind) {
        case 'n': case 'v': return t.text;
        case 'p': return "(" + sp() + flatText(t.kids[0], rng) + sp() + ")";
        case 'u': return t.text + flatText(t.kids[0], rng);
        default: return flatText(t.kids[0], rng) + sp() + t.text + sp() + flatText(t.kids[1], rng);
    }
}

static Block buildBlock(Document& d, const Tree& t) {
    switch (t.kind) {
        case 'n': { Block b = d.make("num"); b.set("value", t.text); return b; }
        case 'v': { Block b = d.make("name"); b.set("name", t.text); return b; }
        case 'p': { Block b = d.make("expr_paren"); b.setValue("x", buildBlock(d, t.kids[0])); return b; }
        case 'u': { Block b = d.make("expr_un"); b.set("op", t.text); b.setValue("x", buildBlock(d, t.kids[0])); return b; }
        default: { Block b = d.make("expr_bin"); b.set("op", t.text); b.setValue("a", buildBlock(d, t.kids[0])); b.setValue("b", buildBlock(d, t.kids[1])); return b; }
    }
}

static bool evalText(const std::string& text, long long& out) {
    int64_t v = 0;
    std::string err;
    SymbolLookup lk = [](const std::string& n, int64_t& x) { x = lookupVal(n); return n == "a" || n == "b" || n == "c"; };
    if (!evalExpr(text, lk, v, err)) return false;
    out = v;
    return true;
}

static void t_expressions() {
    section("expressions: text -> blocks -> text, and blocks -> text with the assembler's precedence");
    struct Case { const char* text; };
    int exact = 0, total = 0;
    std::string bad;
    for (const char* t : {"1 + 2 * 3", "(1 + 2) * 3", "a << 2 | b", "-5", "~a", "10 - 3 - 2", "10 - (3 - 2)", "1 | 2 ^ 3 & 4", "a * (b + c) % 3", "(((7)))", "2 << 1 + 1", "-(-a)", "0x10 + 'A' - 7Fh"}) {
        total++;
        Document d = importAsm(std::string(".main\n EXEC_CSR WRITE8 5, ") + t + "\n");
        const Block& b = first(d);
        const bool made = b.value("imm") != nullptr;
        const bool same = squeezed(genText(d)).find(squeezed(std::string("5,") + t)) != std::string::npos;
        if (made && same) exact++;
        else bad += std::string(t) + (made ? "" : " [not split]") + (same ? "" : " [text differs]") + "; ";
    }
    check(exact == total, "every expression in the battery becomes blocks and gives back its text", bad);
    {
        Document d = importAsm(".main\n EXEC_CSR WRITE8 5, 1 + 2 * 3\n");
        const Block* plus = first(d).value("imm");
        check(plus && plus->type == "expr_bin" && plus->get("op") == "+" && plus->value("b") && plus->value("b")->get("op") == "*", "the tree has the assembler's shape: + above *");
    }
    {
        Document d = importAsm(".main\n EXEC_CSR WRITE8 5, 1 +\n");
        check(first(d).value("imm") == nullptr && first(d).get("imm") == "1 +", "text that is not an expression stays text (nothing is lost)");
    }
    // fuzz: random trees without parentheses -> blocks -> text: the assembler reads the same value as the tree
    std::mt19937 rng(7);
    int wrong = 0, rendered = 0;
    std::string first_wrong;
    for (int i = 0; i < 600; i++) {
        Document d;
        Tree t = randomTree(rng, 4, false);
        Block b = d.make("csr_write8");
        b.set("csr", "5");
        const Block top = buildBlock(d, t);
        b.setValue("imm", top);
        const std::string text = valueText(top);
        long long fromText = 0;
        const long long expect = evalTree(t);
        rendered++;
        if (!evalText(text, fromText) || fromText != expect) { wrong++; if (first_wrong.empty()) first_wrong = text + " expected " + std::to_string(expect); }
    }
    check(wrong == 0, "600 random trees: the text the blocks give means the same as the tree (parentheses only where precedence needs them)", first_wrong);
    // fuzz: random text with parentheses and spacing -> blocks -> the very same text, and the same value
    wrong = 0;
    first_wrong.clear();
    int split = 0;
    for (int i = 0; i < 600; i++) {
        Tree t = randomTree(rng, 4, true);
        const std::string text = flatText(t, rng);
        Document d = importAsm(".main\n EXEC_CSR WRITE8 5, " + text + "\n");
        const Block& b = first(d);
        if (b.value("imm")) split++;
        long long v1 = 0, v2 = 0;
        const std::string back = squeezed(genText(d));
        if (!evalText(text, v1)) continue;                                 // (the grammar allows what the assembler refuses, e.g. a shift by a negative: skip those)
        const std::string out = genText(d);
        const size_t at = out.find("WRITE8 5, ");
        const std::string produced = at == std::string::npos ? "" : out.substr(at + 10);
        if (squeezed(produced) != squeezed(text) || !evalText(produced, v2) || v1 != v2) { wrong++; if (first_wrong.empty()) first_wrong = text + " -> " + produced; }
    }
    check(wrong == 0 && split > 500, "600 random expressions with parentheses and spacing: split into blocks and given back unchanged", first_wrong + " (split " + std::to_string(split) + ")");
}

// ---- regions and windows ----------------------------------------------------------------------------------------------------------------
static void t_regions() {
    section("regions");
    Document d = importAsm(".main\n MOV scratch[slot..+1] -> ALU_OPA\n MOV MAIN_CONST_POOL[@k] -> MAIN_SCRATCHPAD[4]\n MOV ALU_OUT[0..3] -> SCRATCH[8..11]\n");
    const Block& m0 = d.sections[0].blocks[0];
    check(m0.value("src") && m0.value("src")->type == "region_main_scratchpad" && m0.value("src")->get("alias") == "scratch", "an alias is kept as written (scratch = the scratchpad)");
    check(m0.value("src")->value("first") && m0.value("src")->value("first")->type == "name" && m0.value("src")->value("last") && m0.value("src")->value("last")->type == "expr_un", "[slot..+1]: a name, and +1 as a unary plus");
    check(m0.value("dst") && m0.value("dst")->type == "region_alu_opa" && m0.value("dst")->values.empty(), "ALU_OPA has no range");
    const Block& m1 = d.sections[0].blocks[1];
    check(m1.value("src") && m1.value("src")->type == "region_main_const_pool" && m1.value("src")->get("item") == "k", "MAIN_CONST_POOL[@k] is the constant-pool item k");
    check(m1.value("dst") && m1.value("dst")->value("first") && !m1.value("dst")->value("last") && m1.value("dst")->get("last").empty(), "[4] is a single byte (no last)");
    const Block& m2 = d.sections[0].blocks[2];
    check(m2.value("src") && m2.value("src")->type == "region_alu_out" && m2.value("dst")->get("alias") == "SCRATCH", "ALU_OUT and SCRATCH");
    const std::string g = genText(d);
    check(g.find("MOV scratch[slot..+1] -> ALU_OPA") != std::string::npos && g.find("MOV MAIN_CONST_POOL[@k] -> MAIN_SCRATCHPAD[4]") != std::string::npos && g.find("MOV ALU_OUT[0..3] -> SCRATCH[8..11]") != std::string::npos, "all three give their text back", g);
    // the regions come from the ISA table: direction
    Document e = importAsm(".main\n MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\n");
    const Block* dst = first(e).value("dst");
    check(dst && !hasProblem(e, ""), "a plain copy has no problems", problemsOf(e));
    // ranges
    Document r = importAsm(".main\n MOV MAIN_SCRATCHPAD[5..2] -> ALU_OPA\n MOV MAIN_SCRATCHPAD[30..40] -> ALU_OPA\n MOV ALU_OUT -> ALU_OPA\n");
    check(hasProblem(r, "backwards"), "[5..2] is backwards", problemsOf(r));
    check(hasProblem(r, "byte 40 is outside MAIN_SCRATCHPAD (0..31)"), "[30..40] leaves the 32 bytes", problemsOf(r));
    check(hasProblem(r, "needs a byte range"), "ALU_OUT alone needs a range", problemsOf(r));
}

static void t_windows() {
    section("windows: a region range gives ptr and len");
    Document d = importAsm(".main\n EXEC_CSR WRITE_SP AE_CTRL, len=4, ptr=4\n EXEC_CSR READ_BUF AE_STAT, len=2, ptr=0\n.peri\n EXEC_PUB service=1, src=3, len=2\n");
    const Block& w = d.sections[0].blocks[0];
    check(w.value("window") && w.value("window")->type == "region_main_scratchpad", "len=4, ptr=4 is the scratchpad range 4..7");
    check(w.value("window")->value("first") && w.value("window")->value("first")->get("value") == "4" && w.value("window")->value("last")->get("value") == "7", "first 4, last 7");
    check(d.sections[0].blocks[1].value("window") && d.sections[0].blocks[1].value("window")->type == "region_main_csr_data_buf", "READ_BUF reads into the CSR buffer range");
    check(d.sections[1].blocks[0].value("window") && d.sections[1].blocks[0].value("window")->type == "region_peri_rx_buf", "EXEC_PUB takes a range of the RX buffer");
    const std::string g = genText(d);
    check(g.find("WRITE_SP AE_CTRL, len=4, ptr=4") != std::string::npos && g.find("READ_BUF AE_STAT, len=2, ptr=0") != std::string::npos && g.find("EXEC_PUB service=1, src=3, len=2") != std::string::npos, "the text is given back", g);
    // plugging another window changes both numbers
    Document e = d;
    Block win = e.make("region_main_scratchpad");
    Block a = e.make("num"), z = e.make("num");
    a.set("value", "2");
    z.set("value", "9");
    win.setValue("first", a);
    win.setValue("last", z);
    Editor ed(e);
    Outcome o = ed.plugValue(e.sections[0].blocks[0].id, "window", win);
    check(o.ok && genText(e).find("WRITE_SP AE_CTRL, len=8, ptr=2") != std::string::npos, "range 2..9: len=8, ptr=2", genText(e));
    check(e.sections[0].blocks[0].get("len") == "8" && e.sections[0].blocks[0].get("ptr") == "2", "...and the text mirrors follow (an older tool reads them)");
    // names: ptr is the first, len is last - first + 1
    Document f = importAsm(".main\n EXEC_CSR WRITE_SP AE_CTRL, len=n, ptr=base\n");
    check(!first(f).value("window") && genText(f).find("len=n, ptr=base") != std::string::npos, "names in len / ptr: stays text (no window), nothing is lost");
    Document g2 = f;
    Block w2 = g2.make("region_main_scratchpad");
    Block n1 = g2.make("name"), n2 = g2.make("name");
    n1.set("name", "base");
    n2.set("name", "top");
    w2.setValue("first", n1);
    w2.setValue("last", n2);
    Editor ed2(g2);
    o = ed2.plugValue(g2.sections[0].blocks[0].id, "window", w2);
    check(o.ok && genText(g2).find("len=top - base + 1, ptr=base") != std::string::npos, "with names: len = last - first + 1", genText(g2));
    // typing in len removes the window and keeps ptr as the window said
    o = ed.setField(e.sections[0].blocks[0].id, "len", "3");
    check(o.ok && !e.sections[0].blocks[0].value("window") && e.sections[0].blocks[0].get("ptr") == "2" && genText(e).find("len=3, ptr=2") != std::string::npos, "typing a length replaces the window (the pointer keeps what the window said)", genText(e));
    // relative end
    Document h = importAsm(".main\n EXEC_CSR WRITE_SP AE_CTRL, len=2, ptr=6\n");
    check(first(h).value("window") != nullptr, "a numeric pair always becomes a window");
}

// ---- the rules ----------------------------------------------------------------------------------------------------------------------------
static Block region(Document& d, const char* type, const char* a = nullptr, const char* z = nullptr) {
    Block r = d.make(type);
    if (a) { Block n = d.make("num"); n.set("value", a); r.setValue("first", n); }
    if (z) { Block n = d.make("num"); n.set("value", z); r.setValue("last", n); }
    return r;
}
static std::string rulesOf(const Outcome& o) { std::string s; for (auto& v : o.violations) s += v.rule + ": " + v.reason + " | "; return s; }

static void t_rules() {
    section("what may be plugged where");
    Document d;
    Section s;
    s.domain = "main";
    s.blocks.push_back(d.make("mov"));
    d.sections.push_back(s);
    const std::string mov = d.sections[0].blocks[0].id;
    Editor ed(d);
    // types: the source takes readable regions and numbers, the destination writable regions
    Outcome o = ed.plugValue(mov, "dst", region(d, "region_alu_out", "0", "3"), true);
    check(!o.ok || o.fit == Fit::No, "ALU out cannot be a destination (it is read-only)", rulesOf(o));
    o = ed.plugValue(mov, "src", region(d, "region_alu_opa"), true);
    check(!o.ok || o.fit == Fit::No, "ALU A cannot be a source (it is write-only)", rulesOf(o));
    o = ed.plugValue(mov, "dst", region(d, "region_main_csr_data_buf", "0", "1"), true);
    check(!o.ok || o.fit == Fit::No, "the CSR buffer cannot be a destination (v31: MOV may only read it)", rulesOf(o));
    o = ed.plugValue(mov, "dst", region(d, "region_main_scratchpad", "0", "1"));
    check(o.ok && o.fit == Fit::Ok, "the scratchpad can be a destination", rulesOf(o));
    o = ed.plugValue(mov, "src", region(d, "region_alu_out", "0", "1"));
    check(o.ok && o.fit == Fit::Ok, "ALU out can be a source", rulesOf(o));
    // a constant may only go to the scratchpad
    Block num = d.make("num");
    num.set("value", "5");
    num.set("width", "16");
    o = ed.plugValue(mov, "dst", region(d, "region_alu_opa"), true);
    check(o.ok && o.fit == Fit::Ok, "(ALU out -> ALU A is a fine move)", rulesOf(o));
    o = ed.plugValue(mov, "src", num);
    check(o.ok && genText(d).find("MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]") != std::string::npos, "a number in the source is a constant: CONST(5, 2)", genText(d));
    o = ed.plugValue(mov, "dst", region(d, "region_alu_opa"), true);
    bool h7c = false;
    for (auto& v : o.violations) h7c = h7c || v.rule == "H7c";
    check(o.fit == Fit::No && h7c, "...which may not go straight to ALU A (H7c: the scratchpad first)", rulesOf(o));
    // lengths
    o = ed.plugValue(mov, "dst", region(d, "region_main_scratchpad", "0", "4"), true);
    bool h7d = false;
    for (auto& v : o.violations) h7d = h7d || v.rule == "H7d";
    check(o.fit == Fit::No && h7d, "a 2-byte constant into 5 bytes: the lengths must match (H7d)", rulesOf(o));
    // a number with no width into a longer destination is fine (the destination decides)
    Block bare = d.make("num");
    bare.set("value", "5");
    o = ed.plugValue(mov, "src", bare);
    o = ed.plugValue(mov, "dst", region(d, "region_main_scratchpad", "0", "4"));
    check(o.ok && o.fit == Fit::Ok && genText(d).find("MOV CONST(5) -> MAIN_SCRATCHPAD[0..4]") != std::string::npos, "a number without a width takes the destination's length", genText(d) + rulesOf(o));
    // wide constants
    Block wide = d.make("num");
    wide.set("value", "1");
    wide.set("width", "64");
    o = ed.plugValue(mov, "src", wide, true);
    bool h7g = false;
    for (auto& v : o.violations) h7g = h7g || v.rule == "H7g";
    check(o.fit == Fit::No && h7g, "a 64-bit number is refused as a constant (H7g) and the reason says why", rulesOf(o));
    Block fl = d.make("num");
    fl.set("value", "1.5");
    fl.set("kind", "float");
    o = ed.plugValue(mov, "src", fl, true);
    check(o.ok && o.fit == Fit::Ok, "a float is a 4-byte constant", rulesOf(o));
    // sockets that take only an int: a float cannot go there
    Document e;
    Section es;
    es.domain = "main";
    es.blocks.push_back(e.make("csr_write8"));
    e.sections.push_back(es);
    Editor ee(e);
    o = ee.plugValue(e.sections[0].blocks[0].id, "imm", fl, true);
    check(o.fit == Fit::No, "a float cannot be the value of EXEC_CSR WRITE8", rulesOf(o));
    o = ee.plugValue(e.sections[0].blocks[0].id, "imm", region(e, "region_main_scratchpad", "0", "1"), true);
    check(!o.ok || o.fit == Fit::No, "a region is not an int", rulesOf(o));
    // a plug into an occupied socket: the old block floats beside it (not lost)
    Block n1 = e.make("num");
    n1.set("value", "1");
    Block n2 = e.make("num");
    n2.set("value", "2");
    ee.plugValue(e.sections[0].blocks[0].id, "imm", n1);
    const size_t before = e.sections.size();
    o = ee.plugValue(e.sections[0].blocks[0].id, "imm", n2);
    check(o.ok && e.sections.size() == before + 1 && e.sections.back().domain == "floating" && e.sections.back().blocks[0].get("value") == "1" && genText(e).find("WRITE8") != std::string::npos, "an occupied socket: the old number floats beside the block (H7f)", genText(e));
    // the alu block: sockets depend on the operation
    Document a = importAsm(".main\n ALU EQ signed\n ALU AND signed\n ALU SHL\n ALU SHR 9\n ALU ADD 3\n");
    check(hasProblem(a, "EQ and NE have no signed mode"), "EQ cannot be signed (E203)", problemsOf(a));
    check(hasProblem(a, "a logic operation has no signed/unsigned mode", false), "AND with a sign: a warning (W410)", problemsOf(a));
    check(hasProblem(a, "a shift needs an amount"), "SHL without an amount", problemsOf(a));
    check(hasProblem(a, "only a shift has an amount", false), "an amount on ADD is ignored: a warning", problemsOf(a));
    // typed text replaces the component it mirrored
    Document t = importAsm(".main\n MOV CONST(1, 2) -> MAIN_SCRATCHPAD[0..1]\n");
    Editor et(t);
    o = et.setField(t.sections[0].blocks[0].id, "src", "MAIN_CONST_POOL[@c]");
    check(o.ok && !t.sections[0].blocks[0].value("src") && genText(t).find("MAIN_CONST_POOL[@c] -> MAIN_SCRATCHPAD[0..1]") != std::string::npos, "typing in an operand replaces its component", genText(t));
    o = et.unplugValue(t.sections[0].blocks[0].id, "dst", 400, 40);
    check(o.ok && !t.sections[0].blocks[0].value("dst") && t.sections[0].blocks[0].get("dst").empty() && t.sections.back().domain == "floating", "taking a component out leaves a HOLE (no text) and the component floats", genText(t));
    check(genText(t).find("; (incomplete) MOV MAIN_CONST_POOL[@c] ->") != std::string::npos && genText(t).find("\n    MOV") == std::string::npos, "an instruction with a hole is written as a comment, never as half an instruction", genText(t));
    check(hasProblem(t, "'to' is empty"), "...and the block says what is missing", problemsOf(t));
    // keepText leaves the text
    Document t2 = importAsm(".main\n MOV CONST(1, 2) -> MAIN_SCRATCHPAD[0..1]\n");
    Editor et2(t2);
    o = et2.unplugValue(t2.sections[0].blocks[0].id, "dst", 400, 40, true);
    check(o.ok && t2.sections[0].blocks[0].get("dst") == "MAIN_SCRATCHPAD[0..1]" && genText(t2).find("\n    MOV CONST(1, 2) -> MAIN_SCRATCHPAD[0..1]") != std::string::npos, "(keepText leaves the text where it was)", genText(t2));
    // a window taken out empties ptr and len
    Document t3 = importAsm(".main\n EXEC_CSR WRITE_SP AE_CTRL, len=4, ptr=4\n");
    Editor et3(t3);
    o = et3.unplugValue(t3.sections[0].blocks[0].id, "window", 0, 0);
    check(o.ok && t3.sections[0].blocks[0].get("len").empty() && t3.sections[0].blocks[0].get("ptr").empty() && genText(t3).find("(incomplete)") != std::string::npos, "a window taken out leaves a hole: ptr and len are empty", genText(t3));
}

static void t_drag_ops() {
    section("drag support: lift blocks out, plug a floating component back, one undo step");
    Document d = importAsm(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start\n.proc f\n CALL g\n RET\n.endp\n");
    EditHistory hist(d);
    Editor ed(d, Registry::defaults(), RuleOptions(), &hist);
    const std::string movId = d.sections[0].blocks[1].id, jmpId = d.sections[0].blocks[3].id;
    // take a block and everything below it
    Outcome o = ed.liftBlocks(d.sections[0].blocks[2].id, true, 300, 40);
    check(o.ok && d.sections.size() == 2 && d.sections[1].domain == "floating" && d.sections[1].blocks.size() == 3 && d.sections[0].blocks.size() == 2 && d.sections[1].x == 300, "lifting a block takes it and the blocks below it onto the canvas");
    check(genText(d).find("ALU ADD") == std::string::npos, "...so they are no longer part of the program");
    // a single block
    Outcome o2 = ed.liftBlocks(d.sections[1].blocks[1].id, false, 10, 10);
    check(o2.ok && d.sections.size() == 3 && d.sections[2].blocks.size() == 1 && d.sections[1].blocks.size() == 2, "with takeBelow off only that block is taken");
    // lifting the first block of a floating stack changes nothing (it already is one)
    const size_t n = d.sections.size();
    Outcome o3 = ed.liftBlocks(d.sections[2].blocks[0].id, true, 0, 0);
    check(o3.ok && d.sections.size() == n && o3.section == 2, "the first block of a floating stack is already a stack of its own");
    // dry run changes nothing
    const std::string before = serializeDocument(d);
    Outcome dry = ed.liftBlocks(d.sections[0].blocks[1].id, true, 5, 5, true);
    check(dry.ok && serializeDocument(d) == before, "a dry run lifts nothing");
    // components: lift one out of a socket, plug a floating one back
    Document e = importAsm(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n");
    EditHistory h2(e);
    Editor ee(e, Registry::defaults(), RuleOptions(), &h2);
    const std::string host = e.sections[0].blocks[0].id;
    const std::string numId = e.sections[0].blocks[0].value("src")->id;
    ee.beginGroup("drag:1");
    Outcome u = ee.unplugValue(host, "src", 200, 60);
    check(u.ok && e.sections.size() == 2, "the number leaves the socket and floats");
    Outcome bad = ee.plugSection(1, host, "dst", true);
    check(!bad.ok || bad.fit == Fit::No, "a number does not fit the destination (a region is needed)", bad.error);
    Outcome good = ee.plugSection(1, host, "src");
    check(good.ok && e.sections.size() == 1 && e.sections[0].blocks[0].value("src") && e.sections[0].blocks[0].value("src")->id == numId, "dropped back into the source socket it is plugged again (same block)");
    ee.endGroup();
    check(genText(e).find("MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]") != std::string::npos, "and the program is whole again", genText(e));
    check(h2.size() == 2 && h2.undoLabel() != "", "lift and drop with one group key are ONE undo step", std::to_string(h2.size()));
    check(h2.undo() && e.sections.size() == 1 && genText(e).find("MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]") != std::string::npos, "undoing it restores the start");
    // plugSection refuses what is not a lone component
    Document g = importAsm(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP x\n");
    Editor eg(g);
    eg.liftBlocks(g.sections[0].blocks[1].id, true, 50, 50);
    Outcome notComp = eg.plugSection(1, g.sections[0].blocks[0].id, "src", true);
    check(!notComp.ok && !notComp.error.empty(), "a stack of instructions cannot be plugged into a socket", notComp.error);
    // the occupant is not lost when something is dropped on an occupied socket
    Document h;
    h = importAsm(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n");
    Editor eh(h);
    Block other = h.make("num");
    other.set("value", "9");
    other.set("width", "16");
    eh.newFloating({other}, 100, 100);
    Outcome swap = eh.plugSection(1, h.sections[0].blocks[0].id, "src");
    check(swap.ok && h.sections.size() == 2 && h.sections[1].blocks[0].get("value") == "5" && genText(h).find("CONST(9, 2)") != std::string::npos, "dropping on an occupied socket: the old number floats, the new one is in", genText(h));
}

// ---- compatibility --------------------------------------------------------------------------------------------------------------------
// What an older tool is: definitions that know no sockets (so it generates from the text fields only).
static Registry oldRegistry() {
    Registry r = Registry::defaults();
    Registry old;
    for (const BlockDef& d : r.all()) {
        if (!d.valueType.empty()) continue;                         // no component blocks
        BlockDef c = d;
        c.inputs.clear();
        for (FieldDef& f : c.fields) f.when.clear();
        old.add(c);
    }
    return old;
}

static void t_compat() {
    section("compatibility with tools that know only the text");
    const std::string prog = ".equ LIMIT = 4\n.main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU SHL 3 unsigned\n EXEC_CSR WRITE_SP AE_CTRL, len=4, ptr=4\n EXEC_CSR SET8 AE_CTRL, LIMIT + 1\n JMP start, safe\n";
    Document d = importAsm(prog);
    int comps = 0;
    for (const Block& b : d.sections[1].blocks) comps += countComponents(b);
    check(comps >= 10, "the program has components", std::to_string(comps));
    const std::string file = serializeDocument(d);
    check(file.find("\"parts\"") != std::string::npos && file.find("\"values\"") == std::string::npos, "the file has a \"parts\" key (not the M1 \"values\")");
    check(file.find("\"src\": \"CONST(5, 2)\"") != std::string::npos || file.find("\"src\":\"CONST(5, 2)\"") != std::string::npos, "and still the operand text as a field (the mirror)", file.substr(0, 200));
    // an older tool: reads the same file (the key it does not know, "parts", is ignored) and generates the same text
    Document oldRead;
    std::string err;
    const Registry old = oldRegistry();
    std::string asOld = file;
    for (size_t p = asOld.find("\"parts\""); p != std::string::npos; p = asOld.find("\"parts\"", p + 1)) asOld.replace(p, 7, "\"xparts\"");
    check(asOld != file && parseDocument(asOld, oldRead, err, old), "the older tool reads the file", err);
    check(generate(oldRead, old).text == genText(d), "...and generates exactly the same assembler text", generate(oldRead, old).text);
    // an older tool edits the text; the newer tool takes the text
    std::string edited = file;
    const size_t at = edited.find("MAIN_SCRATCHPAD[0..1]");
    check(at != std::string::npos, "(found the destination text)");
    edited.replace(at, std::string("MAIN_SCRATCHPAD[0..1]").size(), "MAIN_SCRATCHPAD[8..9]");
    Document merged;
    check(parseDocument(edited, merged, err), "the newer tool reads the edited file", err);
    check(genText(merged).find("-> MAIN_SCRATCHPAD[8..9]") != std::string::npos && genText(merged).find("MAIN_SCRATCHPAD[0..1]") == std::string::npos, "the edited text wins over the components (they are split again from it)", genText(merged));
    check(merged.sections[1].blocks[1].value("dst") && merged.sections[1].blocks[1].value("dst")->value("first")->get("value") == "8", "...and the components follow the text");
    // a file from before components (no parts at all)
    std::string noParts;
    {
        Document stripped = d;
        std::function<void(std::vector<Block>&)> strip = [&](std::vector<Block>& v) { for (Block& b : v) { b.values.clear(); for (auto& st : b.stacks) strip(st.second); } };
        for (Section& s : stripped.sections) strip(s.blocks);
        noParts = serializeDocument(stripped, old);
    }
    check(noParts.find("\"parts\"") == std::string::npos, "(a file as written before components)");
    Document upgraded;
    check(parseDocument(noParts, upgraded, err), "an old file opens", err);
    check(genText(upgraded) == genText(d), "...generates the same text", genText(upgraded));
    check(countComponents(upgraded.sections[1].blocks[1]) > 0, "...and is split into components in memory");
    auto noIds = [](std::string x) {                                  // ids are handed out in a different order; compare the rest
        std::string o;
        for (size_t i = 0; i < x.size();) {
            if (x.compare(i, 5, "\"id\":") == 0) { const size_t q1 = x.find('"', i + 5), q2 = x.find('"', q1 + 1); i = q2 + 1; o += "\"id\":\"_\""; continue; }
            if (x.compare(i, 9, "\"next_id\"") == 0) { i = x.find_first_of(",}", i); o += "\"next_id\":0"; continue; }
            o += x[i++];
        }
        return o;
    };
    check(noIds(serializeDocument(upgraded)) == noIds(file), "saving it writes exactly the components a fresh import makes (nothing else changes)");
    // a file with a stale component (text mirror differs)
    Document stale = d;
    stale.sections[1].blocks[0].set("name", "renamed");
    check(genText(stale).find("renamed:") != std::string::npos, "(sanity: a plain field edit)");
    // the version stays 1
    check(file.find("\"version\":1") != std::string::npos || file.find("\"version\": 1") != std::string::npos, "the format version is still 1 (an addition, not a change)");
}

// ---- symbols -----------------------------------------------------------------------------------------------------------------------------
static bool hasRef(const Document& d, const std::string& needle, bool error, const SymbolTable* ext = nullptr) {
    for (const BlockProblem& p : validate(d, Registry::defaults(), ext)) if (p.kind == "reference" && p.error == error && p.message.find(needle) != std::string::npos) return true;
    return false;
}
static std::string refProblems(const Document& d, const SymbolTable* ext = nullptr) {
    std::string s;
    for (const BlockProblem& p : validate(d, Registry::defaults(), ext)) if (p.kind == "reference") s += std::string(p.error ? "E: " : "W: ") + p.message + "; ";
    return s;
}

static void t_symbols() {
    section("the symbol table: definitions produce typed symbols");
    Document d = importAsm(".equ LIMIT = 4\n.equ N, 8\nSIZE EQU 5\n.csr AE_CTRL = 0x120\n.service SVC = 3, len=2\n.const TBL db 1, 2, 3\n.devmap 0, I2C, 0x50, sensor\n.include \"x.inc\"\n.main\nstart: JMP start\n.proc f\ninner: RET\n.endp\n.peri\np: JMP p\n");
    SymbolTable t;
    t.addDocument(d);
    const Symbol* s = t.find("LIMIT");
    check(s && s->kind == "int" && s->value == "4", ".equ NAME = value");
    check(t.find("N", "int") && t.find("N")->value == "8" && t.find("SIZE", "int") && t.find("SIZE")->value == "5", ".equ NAME, value and the 8051 NAME EQU value");
    check(t.find("AE_CTRL", "csr") && t.find("SVC", "service") && t.find("TBL", "const") && t.find("sensor", "dev") && t.find("sensor")->value == "0", ".csr, .service, .const and a named .devmap entry");
    check(t.find("AE_CTRL", "int") && !t.find("AE_CTRL", "service") && !t.find("start", "int"), "a CSR is a number too; a code position is not");
    check(t.ofKind("csr").size() == 1 && t.ofKind("label.flow").size() == 3, "the names of one kind");
    check(t.openWorld(), "an .include means names may be defined outside");
    const Symbol* hit = nullptr;
    check(t.look("start", "label.flow", "main", "") == SymbolTable::Look::Found, "a flow label is found from its own domain");
    check(t.look("start", "label.flow", "peri", "") == SymbolTable::Look::OtherDomain, "...not from the other domain");
    check(t.look("inner", "label.flow", "main", "", &hit) == SymbolTable::Look::Hidden && hit && hit->proc == "f", "a label inside a procedure is not visible outside it");
    check(t.look("inner", "label.flow", "main", "f") == SymbolTable::Look::Found, "...but inside it");
    check(t.look("f", "label.proc", "main", "") == SymbolTable::Look::Found && t.look("nope", "label.flow", "main", "") == SymbolTable::Look::Missing, "procedures; an unknown name is missing");
    SymbolTable h;
    h.addAsm(".csr HW_CTRL = 0x10\n.equ HW_N = 2\n");
    check(h.find("HW_CTRL", "csr") && h.find("HW_N", "int"), "assembler text (the hardware description's include) adds symbols");
    SymbolTable none;
    Document plain = importAsm(".main\n JMP x\n");
    none.addDocument(plain);
    check(!none.openWorld(), "a program without includes is a closed world");
    Document withHw = plain;
    withHw.hardware = "board.achw";
    SymbolTable hwTab;
    hwTab.addDocument(withHw);
    check(hwTab.openWorld(), "a program with a hardware description is an open world");

    section("names become typed references");
    Document e = importAsm(".csr AE_CTRL = 0x120\n.service SVC = 3\n.equ K = 1\n.main\n EXEC_CSR WRITE8 AE_CTRL, K\n EXEC_CSR WRITE8 0x120, 1\n EXEC_CSR WRITE8 UNDEF, 1\n.peri\n EXEC_PUB service=SVC, src=0, len=2\n");
    const Block& w1 = e.sections[1].blocks[0];
    check(w1.value("csr") && w1.value("csr")->type == "csr_ref" && w1.value("csr")->get("name") == "AE_CTRL", "a name defined by .csr is a CSR reference");
    check(w1.value("imm") && w1.value("imm")->type == "name", "a .equ name in an int socket is a name");
    check(e.sections[1].blocks[1].value("csr") && e.sections[1].blocks[1].value("csr")->type == "num", "a number stays a number");
    check(e.sections[1].blocks[2].value("csr") && e.sections[1].blocks[2].value("csr")->type == "name", "an undefined name is still a name (and will be flagged)");
    check(e.sections[2].blocks[0].value("service") && e.sections[2].blocks[0].value("service")->type == "service_ref", "a name defined by .service is a service reference in EXEC_PUB");
    const std::string g = genText(e);
    check(g.find("WRITE8 AE_CTRL, K") != std::string::npos && g.find("service=SVC, src=0, len=2") != std::string::npos, "and the text is the same", g);

    section("checking references");
    check(hasRef(e, "'UNDEF' is not defined", false) && !hasRef(e, "'AE_CTRL'", false) && !hasRef(e, "'K'", false), "an undefined name is a warning; the defined ones are fine", refProblems(e));
    Document wrongKind = e;
    wrongKind.sections[1].blocks[0].value("csr");
    {
        Block* host = &wrongKind.sections[1].blocks[0];
        Block bad = wrongKind.make("csr_ref");
        bad.set("name", "SVC");
        host->setValue("csr", bad);
    }
    check(hasRef(wrongKind, "'SVC' is a service, not a csr", true), "a CSR reference to a name that is a service: an error", refProblems(wrongKind));
    Document lbl = importAsm(".main\nstart: JMP start\n EXEC_CSR WRITE8 start, 1\n");
    check(hasRef(lbl, "'start' is a code position, not a number", true), "a label used as a number: an error", refProblems(lbl));
    Document jumps = importAsm(".main\n JMP nowhere\n JMP inner\n.proc f\ninner: RET\n.endp\n.peri\n JMP mainlabel\n.main\nmainlabel: JMP mainlabel\n");
    check(hasRef(jumps, "the label 'nowhere' is not defined", false), "a jump to an undefined label: a warning", refProblems(jumps));
    check(hasRef(jumps, "a branch cannot enter a procedure", true), "a jump into a procedure: an error (E305)", refProblems(jumps));
    check(hasRef(jumps, "'mainlabel' belongs to the other domain", true), "a Peri jump to a Main label: an error", refProblems(jumps));
    Document hint = importAsm(".include \"defs.inc\"\n.main\n JMP nowhere\n");
    bool hinted = false;
    for (const BlockProblem& p : validate(hint)) hinted = hinted || p.message.find("included file") != std::string::npos;
    check(hinted, "with an .include the message says the name may come from there");
    SymbolTable ext;
    ext.addAsm(".csr EXT_CTRL = 5\n");
    Document use = importAsm(".main\n EXEC_CSR WRITE8 EXT_CTRL, 1\n");
    check(hasRef(use, "'EXT_CTRL' is not defined", false) && !hasRef(use, "'EXT_CTRL'", false, &ext), "symbols from outside (the hardware description) are known to the check", refProblems(use, &ext));

    section("names from outside become references");
    SymbolTable outside;
    outside.addAsm(".csr EXT = 1\n.service XS = 2\n");
    Document ra = importAsm(".main\n EXEC_CSR WRITE8 EXT, 1\n.peri\n EXEC_PUB service=XS, src=0, len=1\n");
    check(ra.sections[0].blocks[0].value("csr") && ra.sections[0].blocks[0].value("csr")->type == "name", "(unknown to the document: a plain name)");
    const int retyped = retypeReferences(ra, Registry::defaults(), outside);
    check(retyped == 2 && ra.sections[0].blocks[0].value("csr")->type == "csr_ref" && ra.sections[1].blocks[0].value("service")->type == "service_ref", "a name that the outside defines as a CSR / service becomes that reference", std::to_string(retyped));
    Document rb;
    std::string rerr;
    check(parseDocument(serializeDocument(ra), rb, rerr, Registry::defaults(), &outside) && rb.sections[0].blocks[0].value("csr")->type == "csr_ref", "...and a file read with the outside symbols known is split that way at once", rerr);
    check(genText(ra) == genText(rb) && genText(ra).find("WRITE8 EXT, 1") != std::string::npos, "the text is the same either way", genText(ra));

    section("calls: no recursion, and the procedure must exist");
    Document calls = importAsm(".main\n CALL a\n CALL ghost\n.proc a\n CALL b\n RET\n.endp\n.proc b\n CALL a\n RET\n.endp\n.proc c\n CALL c\n RET\n.endp\n.proc ok1\n CALL ok2\n RET\n.endp\n.proc ok2\n RET\n.endp\n");
    check(hasRef(calls, "which leads back to", true), "a calls b calls a: recursion is an error", refProblems(calls));
    check(hasRef(calls, "'c' calls itself", true), "a procedure calling itself", refProblems(calls));
    check(hasRef(calls, "the procedure 'ghost' is not defined", false), "a call to an unknown procedure: a warning", refProblems(calls));
    check(!hasRef(calls, "'ok1'", true) && !hasRef(calls, "'ok2'", true), "an ordinary chain is fine", refProblems(calls));
    int cycleErrors = 0;
    for (const BlockProblem& p : validate(calls)) if (p.error && p.message.find("recursion") != std::string::npos) cycleErrors++;
    check(cycleErrors == 2, "one error per cycle (not one per call)", std::to_string(cycleErrors));
    Document cross = importAsm(".main\n.proc m\n RET\n.endp\n.peri\n CALL m\n");
    check(hasRef(cross, "belongs to the other domain", true), "Peri cannot call a Main procedure", refProblems(cross));
}

// ---- classes ------------------------------------------------------------------------------------------------------------------------------
static void t_classes() {
    section("classes: instructions and components are different kinds of block");
    const Registry& reg = Registry::defaults();
    bool all = true, coherent = true;
    std::string bad;
    for (const BlockDef& d : reg.all()) {
        if (d.cls.empty()) { all = false; bad += d.type + " "; }
        if ((d.cls == "component") != !d.valueType.empty()) { coherent = false; bad += d.type + "! "; }
    }
    check(all && coherent, "every block has a class; exactly the value blocks are components", bad);
    bool instr = true;
    for (const char* t : {"mov", "alu", "csr_write8", "csr_set8", "csr_clr8", "csr_read8", "csr_write_sp", "csr_read_buf", "br_cond", "jmp", "br_eret", "br_peri", "call", "ret", "xfer", "pub", "cascade_begin", "cascade_end"})
        instr = instr && reg.find(t) && reg.find(t)->cls == "instruction";
    check(instr, "move, calculate, the CSR instructions, the branches, call / return and the Peri instructions are instructions");
    bool comps = true;
    for (const char* t : {"num", "name", "expr_bin", "region_main_scratchpad", "region_alu_opa", "alu_op", "csr_ref", "label_ref"}) comps = comps && reg.find(t) && reg.find(t)->cls == "component";
    check(comps, "numbers, names, expressions, regions, options and references are components");
    check(reg.find("proc")->cls == "structure" && reg.find("label")->cls == "structure" && reg.find("directive")->cls == "definition" && reg.find("comment")->cls == "text" && reg.find("raw")->cls == "text", "procedures and labels are structure; directives definitions; comments and raw text are text");
    // the drawers of the two classes never mix, so a toolbox can list them apart
    std::set<std::string> instrDrawers, compDrawers;
    for (const BlockDef* d : reg.ofClass("instruction")) instrDrawers.insert(d->category);
    for (const BlockDef* d : reg.ofClass("component")) compDrawers.insert(d->category);
    bool disjoint = true;
    for (const std::string& c : compDrawers) if (instrDrawers.count(c)) disjoint = false;
    check(disjoint && compDrawers.count("Values") && compDrawers.count("Regions") && compDrawers.count("Options") && compDrawers.count("References"), "the drawers of instructions and of components are different (Values, Regions, Options, References vs Data, Math, CSR, Control, Peripheral)");
    check(reg.ofClass("instruction").size() >= 16 && reg.ofClass("component").size() >= 24, "the counts", std::to_string(reg.ofClass("instruction").size()) + " / " + std::to_string(reg.ofClass("component").size()));
}

// ---- stage M2b-3: bit masks, cascades, flag branches --------------------------------------------------------------------------------------
static bool hasHint(const Document& d, const std::string& needle, bool error, const char* kind) {
    for (const BlockProblem& p : validate(d)) if (p.error == error && p.kind == kind && p.message.find(needle) != std::string::npos) return true;
    return false;
}

static void t_m2b3() {
    section("bit masks: (1 << n) is a bit");
    Document d = importAsm(".equ N = 2\n.main\n EXEC_CSR SET8 5, (1 << 3) | (1 << 5)\n EXEC_CSR CLR8 5, (1<<N)\n EXEC_CSR SET8 5, (2 << 1)\n EXEC_CSR SET8 5, (1 << 3 + 1)\n");
    const Block& a = d.sections[1].blocks[0];
    check(a.value("imm") && a.value("imm")->type == "expr_bin" && a.value("imm")->get("op") == "|" && a.value("imm")->value("a")->type == "bit_mask" && a.value("imm")->value("b")->type == "bit_mask", "(1 << 3) | (1 << 5) is two bits or-ed");
    check(a.value("imm")->value("a")->value("n") && a.value("imm")->value("a")->value("n")->get("value") == "3", "a bit holds the bit number");
    const Block& b = d.sections[1].blocks[1];
    check(b.value("imm") && b.value("imm")->type == "bit_mask" && b.value("imm")->value("n")->type == "name", "(1<<N) with a name");
    check(d.sections[1].blocks[2].value("imm") && d.sections[1].blocks[2].value("imm")->type == "expr_paren", "(2 << 1) is not a bit: a parenthesised expression");
    check(d.sections[1].blocks[3].value("imm") && d.sections[1].blocks[3].value("imm")->type == "bit_mask" && d.sections[1].blocks[3].value("imm")->value("n")->type == "expr_bin", "(1 << 3 + 1): the bit number is an expression");
    const std::string g = genText(d);
    check(g.find("SET8 5, (1 << 3) | (1 << 5)") != std::string::npos && g.find("CLR8 5, (1 << N)") != std::string::npos && g.find("SET8 5, (2 << 1)") != std::string::npos && g.find("(1 << 3 + 1)") != std::string::npos, "all give their text back (the assembler reads the same masks)", g);
    Document built;
    Section sec;
    sec.domain = "main";
    Block csr = built.make("csr_set8");
    csr.set("csr", "5");
    Block mask = built.make("expr_bin");
    mask.set("op", "|");
    for (const char* sock : {"a", "b"}) {
        Block bit = built.make("bit_mask");
        Block n = built.make("num");
        n.set("value", sock[0] == 'a' ? "1" : "4");
        bit.setValue("n", n);
        mask.setValue(sock, bit);
    }
    csr.setValue("imm", mask);
    sec.blocks.push_back(csr);
    built.sections.push_back(sec);
    check(genText(built).find("EXEC_CSR SET8 5, (1 << 1) | (1 << 4)") != std::string::npos, "bits built by hand: set bits 1 and 4", genText(built));

    section("cascades: numbering, exits, selector");
    Document ok = importAsm(".main\n CASCADE_BEGIN 1, b0\nb0: JMP x\n CASCADE_END 0, done\nb1: JMP y\n CASCADE_END 1, done\ndone: JMP done\n");
    check(!hasHint(ok, "", true, "structure") && !hasHint(ok, "", false, "structure"), "a well-formed cascade has no problems", problemsOf(ok));
    Document order = importAsm(".main\n CASCADE_BEGIN 0, b0\nb0: JMP x\n CASCADE_END 1, done\ndone: JMP done\n");
    check(hasHint(order, "expected 0", true, "structure"), "END numbered out of order (E304)", problemsOf(order));
    Document tooHigh = importAsm(".main\n CASCADE_BEGIN 5, b0\nb0: JMP x\n CASCADE_END 0, done\ndone: JMP done\n");
    check(hasHint(tooHigh, "above the last block index", true, "structure"), "selector above the last block (E304)", problemsOf(tooHigh));
    Document noEnd = importAsm(".main\n CASCADE_BEGIN 0, b0\nb0: JMP x\n");
    check(hasHint(noEnd, "without a CASCADE_END", true, "structure"), "BEGIN without END", problemsOf(noEnd));
    Document exits = importAsm(".main\n CASCADE_BEGIN 0, b0\nb0: JMP x\n CASCADE_END 0, e1\n CASCADE_END 1, e2\ne1: JMP e1\ne2: JMP e2\n");
    check(hasHint(exits, "different exit labels", false, "structure"), "different exits: a warning (W405)", problemsOf(exits));

    section("branches that read the ALU flag");
    Document good = importAsm(".main\nl: JMP l\n ALU CMP.EQ\n ; a comment between\n BR.COND ALU_CMP_TRUE, l\n");
    check(!hasHint(good, "ALU flag", false, "hint"), "the ALU operation right before (a comment in between is fine)", problemsOf(good));
    Document stale = importAsm(".main\nl: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n BR.COND ALU_CMP_TRUE, l\n");
    check(hasHint(stale, "the block before it is not an ALU operation", false, "hint"), "something else before it: a hint (W506)", problemsOf(stale));
    Document first = importAsm(".main\n BR.COND ALU_OUT_OV16, l\nl: JMP l\n");
    check(hasHint(first, "no ALU operation came before", false, "hint"), "first in its stack: a hint", problemsOf(first));
    Document afterLabel = importAsm(".main\nloop: BR.COND ALU_CMP_TRUE, loop\n");
    Document lbl = importAsm(".main\nl: JMP l\nloop:\n BR.COND ALU_CMP_TRUE, loop\n");
    check(!hasHint(lbl, "ALU flag", false, "hint"), "right after a label (a loop head) there is nothing to say", problemsOf(lbl));
    (void)afterLabel;
    Document other = importAsm(".main\nl: JMP l\n ALU CMP.EQ\n MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n BR.COND ALWAYS, l\n");
    check(!hasHint(other, "ALU flag", false, "hint"), "other conditions are not asked for an ALU operation", problemsOf(other));
}

// ---- EXEC_XFER ----------------------------------------------------------------------------------------------------------------------------
static void t_xfer() {
    section("EXEC_XFER: the three forms are split into parts");
    Document d = importAsm(".devmap 0, I2C, 0x50, sensor\n.peri\n EXEC_XFER WRITE dev=0, addr=0x10, addrlen=1, data=5\n EXEC_XFER READ dev=sensor, addr=0x10, addrlen=1, ptr=2\n EXEC_XFER READ dev=0, len=4, ptr=0, ext=[1, 2]\n EXEC_XFER READ addr=1, dev=0, ptr=1\n");
    const Section& s = d.sections[1];
    const Block& w = s.blocks[0];
    check(w.value("dev") && w.value("addr") && w.value("addrlen") && w.value("data") && !w.value("ptr") && !w.value("len"), "a one-byte WRITE: dev, addr, addrlen, data");
    check(w.value("addr")->get("value") == "0x10" && w.value("data")->get("value") == "5", "...as numbers");
    const Block& r1 = s.blocks[1];
    check(r1.value("dev") && r1.value("dev")->type == "dev_ref" && r1.value("dev")->get("name") == "sensor" && r1.value("ptr"), "a one-byte READ: a named device is a device reference");
    const Block& r2 = s.blocks[2];
    check(r2.value("dev") && r2.value("len") && r2.value("ptr") && !r2.value("addr") && r2.get("ext") == "[1, 2]", "a multi-byte READ: dev, len, ptr; the address bytes stay text in `ext`");
    const Block& weird = s.blocks[3];
    check(!weird.value("dev") && weird.get("args") == "addr=1, dev=0, ptr=1", "parts in another order than the definition's stay text (nothing changes)");
    const std::string g = genText(d);
    check(g.find("EXEC_XFER WRITE dev=0, addr=0x10, addrlen=1, data=5") != std::string::npos && g.find("READ dev=sensor, addr=0x10, addrlen=1, ptr=2") != std::string::npos
          && g.find("READ dev=0, len=4, ptr=0, ext=[1, 2]") != std::string::npos && g.find("READ addr=1, dev=0, ptr=1") != std::string::npos, "all four give their text back", g);
    // the file: the mirror is the args text; the parts are in "parts"
    const std::string file = serializeDocument(d);
    check(file.find("dev=0, addr=0x10, addrlen=1, data=5") != std::string::npos, "the args text is written as the mirror");
    Document back;
    std::string err;
    check(parseDocument(file, back, err) && genText(back) == g, "and reads back to the same text", err);

    section("EXEC_XFER: the rules of each form");
    auto bad = [&](const std::string& body, const std::string& needle, bool error = true) {
        Document e = importAsm(".peri\n" + body + "\n");
        return hasProblem(e, needle, error);
    };
    check(bad(" EXEC_XFER WRITE len=2, ptr=0, dev=0", "multi-byte WRITE is not available") || bad(" EXEC_XFER WRITE dev=0, len=2, ptr=0", "multi-byte WRITE is not available"), "a multi-byte WRITE: no transmit buffer");
    check(bad(" EXEC_XFER WRITE dev=0, addr=1, addrlen=1", "needs the byte to write"), "a write without data");
    check(bad(" EXEC_XFER READ dev=0, addr=1, addrlen=1", "needs the buffer byte"), "a read without ptr");
    check(bad(" EXEC_XFER READ dev=9, ptr=0", "0 to 7"), "device 9");
    check(bad(" EXEC_XFER READ dev=0, len=30, ptr=5", "more than the 32 bytes"), "ptr + len over 32");
    check(bad(" EXEC_XFER READ dev=0, addr=0x400, addrlen=2, ptr=0", "10-bit address"), "a 10-bit address");
    check(bad(" EXEC_XFER READ dev=0, addr=1, addrlen=0, ptr=0", "addrlen is 0"), "addr given but addrlen 0");
    check(bad(" EXEC_XFER READ dev=0, addr=1, len=2, ptr=0", "no addr / addrlen", false), "a multi-byte read with an address: a warning (use ext)");
    check(!bad(" EXEC_XFER READ dev=0, len=4, ptr=0", "needs"), "a correct multi-byte read has no problems");

    section("EXEC_XFER: editing the parts");
    Document e = importAsm(".peri\n EXEC_XFER WRITE dev=0, addr=1, addrlen=1, data=5\n EXEC_XFER READ addr=1, dev=0, ptr=1\n");
    Editor ed(e);
    Block nd = e.make("num");
    nd.set("value", "7");
    Outcome o = ed.plugValue(e.sections[0].blocks[0].id, "data", nd);
    check(o.ok && genText(e).find("WRITE dev=0, addr=1, addrlen=1, data=7") != std::string::npos && e.sections[0].blocks[0].get("args") == "dev=0, addr=1, addrlen=1, data=7", "plugging a number into data: the text and the mirror follow", genText(e));
    Block n2 = e.make("num");
    n2.set("value", "2");
    o = ed.plugValue(e.sections[0].blocks[1].id, "dev", n2);
    check(!o.ok && !o.error.empty() && genText(e).find("READ addr=1, dev=0, ptr=1") != std::string::npos, "text that cannot be split refuses a plug (nothing is lost)", o.error);
    o = ed.setField(e.sections[0].blocks[0].id, "args", "dev=1, data=9");
    check(o.ok && !e.sections[0].blocks[0].value("dev") && genText(e).find("WRITE dev=1, data=9") != std::string::npos, "typing the args text replaces all the parts");
}

// ---- every sample ------------------------------------------------------------------------------------------------------------------------
static std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void walkBlocks(const std::vector<Block>& v, const std::function<void(const Block&)>& f) {
    for (const Block& b : v) { f(b); for (const auto& st : b.stacks) walkBlocks(st.second, f); }
}

static void t_samples() {
    section("splitting the real programs");
    namespace fs = std::filesystem;
    int files = 0, movs = 0, movSplit = 0, ops = 0, opsSplit = 0, windows = 0, windowsSplit = 0, comps = 0;
    std::string unsplit;
    for (const char* sub : {"sample", "examples"}) {
        const fs::path root = fs::path(ASMCORE_SOURCE_DIR) / sub;
        if (!fs::exists(root)) continue;
        for (auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file() || e.path().extension() != ".asm") continue;
            if (e.path().generic_string().find("/out/") != std::string::npos) continue;
            files++;
            Document d = importAsm(slurp(e.path()));
            for (const Section& sec : d.sections) walkBlocks(sec.blocks, [&](const Block& b) {
                comps += countComponents(b);
                if (b.type == "mov") {
                    movs++;
                    if (b.value("src") && b.value("dst")) movSplit++;
                    else if (unsplit.size() < 400) unsplit += b.get("src") + " -> " + b.get("dst") + "; ";
                }
                if (b.type == "csr_write8" || b.type == "csr_set8" || b.type == "csr_clr8") { ops += 2; opsSplit += (b.value("csr") ? 1 : 0) + (b.value("imm") ? 1 : 0); }
                if (b.type == "csr_write_sp" || b.type == "csr_read_buf" || b.type == "pub") { windows++; if (b.value("window")) windowsSplit++; }
            });
        }
    }
    std::printf("        %d files: %d components; MOV %d/%d split, CSR operands %d/%d, windows %d/%d\n", files, comps, movSplit, movs, opsSplit, ops, windowsSplit, windows);
    check(files >= 15, "found the sample programs", std::to_string(files));
    check(movs > 0 && movSplit * 100 >= movs * 90, "at least 90 % of the MOV operands of the real programs become components", unsplit);
    check(ops > 0 && opsSplit * 100 >= ops * 90, "at least 90 % of the CSR operands become components");
    check(windows == 0 || windowsSplit * 100 >= windows * 80, "most len / ptr pairs become windows");
}

int main() {
    std::printf("asmcore component blocks tests\n");
    t_numbers();
    t_expressions();
    t_regions();
    t_windows();
    t_rules();
    t_compat();
    t_drag_ops();
    t_m2b3();
    t_classes();
    t_symbols();
    t_xfer();
    t_samples();
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
