// Component blocks (stage M2b): numbers, expressions, regions with ranges, option components; the text <-> component conversion.
// See docs/BLOCKS_COMPOSITION.md and AssemblyCoreIDE/docs/BLOCKS_INSTRUCTION_ANALYSIS.md for the design.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "asmcore/blocks.h"
#include "asmcore/lang.h"
#include "blocks_internal.h"
#include "isa.h"
#include "util.h"

namespace asmcore {
namespace blocks {

namespace {

std::string lowerOf(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string squeeze(const std::string& s) {                       // the text without any white space: what "the same text" means
    std::string o;
    for (char c : s) if (!std::isspace((unsigned char)c)) o += c;
    return o;
}

const char* kRegionAliases[][2] = {                               // alias, canonical (mirrors isa::regionByName)
    {"SCRATCHPAD", "MAIN_SCRATCHPAD"}, {"SCRATCH", "MAIN_SCRATCHPAD"}, {"CSR_DATA_BUF", "MAIN_CSR_DATA_BUF"}, {"CSRBUF", "MAIN_CSR_DATA_BUF"},
    {"RESULT_SLOT", "PERI_RESULT_SLOT"}, {"SLOT", "PERI_RESULT_SLOT"}, {"CONST_POOL", "MAIN_CONST_POOL"}, {"CONST", "MAIN_CONST_POOL"},
    {"RESULT_META", "PERI_RESULT_META"}, {"META", "PERI_RESULT_META"}, {"OPA", "ALU_OPA"}, {"OPB", "ALU_OPB"}, {"OUT", "ALU_OUT"}, {"STATUS", "ALU_STATUS"}};

const char* kPeriRx = "PERI_RX_BUF";                             // not a MOV region, but the source of EXEC_PUB and the target of EXEC_XFER

std::string regionBlockType(const std::string& canonical) { return "region_" + lowerOf(canonical); }

// The canonical region name of a block type "region_xxx", or "".
std::string canonicalOf(const BlockDef& d) {
    if (!isRegionType(d.valueType)) return std::string();
    return d.valueType.substr(7);
}

bool hasRange(const std::string& canonical) { return canonical != "ALU_OPA" && canonical != "ALU_OPB"; }

const char* kBinOps[] = {"+", "-", "*", "/", "%", "<<", ">>", "&", "^", "|"};

int binPrec(const std::string& op) {                                  // the assembler's expression grammar, low to high (util.cpp)
    if (op == "|") return 1;
    if (op == "^") return 2;
    if (op == "&") return 3;
    if (op == "<<" || op == ">>") return 4;
    if (op == "+" || op == "-") return 5;
    return 6;                                                         // * / %
}

}  // namespace

std::string regionType(const std::string& canonical) { return "region:" + canonical; }
bool isRegionType(const std::string& t) { return t.compare(0, 7, "region:") == 0; }

bool partVisible(const std::string& when, const Block& b) {
    if (when.empty()) return true;
    size_t p = when.find("!=");
    bool negate = p != std::string::npos;
    if (!negate) p = when.find('=');
    if (p == std::string::npos) return true;
    const std::string field = when.substr(0, p);
    const std::string list = when.substr(p + (negate ? 2 : 1));
    const std::string value = upper(b.get(field));
    size_t start = 0;
    bool in = false;
    for (;;) {
        size_t c = list.find(',', start);
        if (upper(list.substr(start, c == std::string::npos ? std::string::npos : c - start)) == value) in = true;
        if (c == std::string::npos) break;
        start = c + 1;
    }
    return negate ? !in : in;
}

bool parseNumberLiteral(const std::string& text0, long long& out) {
    const std::string text = trim(text0);
    if (text.empty()) return false;
    if (text.size() == 3 && text[0] == '\'' && text[2] == '\'') { out = (unsigned char)text[1]; return true; }
    if (!std::isdigit((unsigned char)text[0])) return false;
    for (char c : text) if (!std::isalnum((unsigned char)c) && c != '_') return false;
    int64_t v = 0;
    std::string err;
    if (!evalExpr(text, nullptr, v, err)) return false;
    out = v;
    return true;
}

// ---- the definitions of the components -------------------------------------------------------------------------------------------------
namespace detail {

void addComponentDefs(Registry& r, const lang::Language& L) {
    auto field = [](const char* name, const char* label, FieldKind kind = FieldKind::Text) {
        FieldDef f;
        f.name = name; f.label = label; f.kind = kind;
        return f;
    };
    auto choice = [&](const char* name, const char* label, std::vector<std::string> options, bool open = false) {
        FieldDef f = field(name, label, FieldKind::Choice);
        f.options = std::move(options);
        f.open = open;
        return f;
    };
    auto input = [](const char* name, const char* label, std::vector<std::string> accepts, bool optional = false) {
        InputDef i;
        i.name = name; i.label = label; i.accepts = std::move(accepts); i.optional = optional;
        return i;
    };
    auto value = [&](const char* type, const char* title, const char* cat, int dom, const std::string& valueType, std::vector<FieldDef> fields, std::vector<InputDef> inputs, const char* tmpl) {
        BlockDef d;
        d.type = type; d.title = title; d.category = cat; d.domains = dom; d.valueType = valueType;
        d.fields = std::move(fields); d.inputs = std::move(inputs); d.tmpl = tmpl;
        r.add(d);
    };
    const std::vector<std::string> intOnly = {kTypeInt};

    // ---- numbers and expressions (type "int")
    {
        FieldDef v = field("value", "");
        FieldDef kind = choice("kind", "kind", {"", "int", "float", "double"});
        kind.optional = true;
        FieldDef sign = choice("sign", "sign", {"", "signed", "unsigned"});
        sign.optional = true;
        FieldDef width = choice("width", "bits", {"", "8", "16", "24", "32", "64"});
        width.optional = true;
        value("num", "number", "Math", kBoth, kTypeInt, {v, kind, sign, width}, {input("value", "", intOnly, true)}, "{value}");
        r.edit("num")->semantic = "number";
    }
    value("name", "name", "Math", kBoth, kTypeInt, {field("name", "")}, {}, "{name}");
    {
        std::vector<std::string> ops(std::begin(kBinOps), std::end(kBinOps));
        value("expr_bin", "", "Math", kBoth, kTypeInt, {choice("op", "", ops)}, {input("a", "", intOnly), input("b", "", intOnly)}, "{a} {op} {b}");
        value("expr_un", "", "Math", kBoth, kTypeInt, {choice("op", "", {"-", "+", "~"})}, {input("x", "", intOnly)}, "{op}{x}");
        value("expr_paren", "", "Math", kBoth, kTypeInt, {}, {input("x", "", intOnly)}, "({x})");
        r.edit("expr_bin")->semantic = "binop";
        r.edit("expr_un")->semantic = "unop";
        r.edit("expr_paren")->semantic = "paren";
    }
    // ---- regions (one block per region, from the ISA table: direction and size are the table's)
    for (const isa::RegionInfo& ri : isa::kRegions) {
        std::vector<std::string> names = {ri.name};
        for (auto& a : kRegionAliases) if (std::strcmp(a[1], ri.name) == 0) names.push_back(a[0]);
        FieldDef alias = choice("alias", "as written", names, true);
        alias.optional = true;
        alias.when = "alias!=";
        std::vector<FieldDef> fields = {alias};
        std::vector<InputDef> inputs;
        if (hasRange(ri.name)) {
            FieldDef first = field("first", "");
            FieldDef last = field("last", "");
            first.optional = last.optional = true;
            fields.push_back(first);
            fields.push_back(last);
            inputs.push_back(input("first", "", intOnly, true));
            inputs.push_back(input("last", "to", intOnly, true));
            if (std::strcmp(ri.name, "MAIN_CONST_POOL") == 0) {
                FieldDef item = field("item", "constant");
                item.optional = true;
                fields.push_back(item);
            }
        }
        std::string title = ri.name;
        static const struct { const char* name; const char* title; } kTitles[] = {
            {"MAIN_SCRATCHPAD", "scratchpad"}, {"MAIN_CSR_DATA_BUF", "CSR buffer"}, {"PERI_RESULT_SLOT", "result slot"}, {"ALU_OPA", "ALU A"}, {"ALU_OPB", "ALU B"},
            {"ALU_OUT", "ALU out"}, {"ALU_STATUS", "ALU status"}, {"MAIN_CONST_POOL", "const pool"}, {"PERI_RESULT_META", "result info"}};
        for (auto& t : kTitles) if (title == t.name) title = t.title;
        value(regionBlockType(ri.name).c_str(), title.c_str(), "Data", kMain, regionType(ri.name), fields, inputs, "{alias}");
        r.edit(regionBlockType(ri.name))->semantic = "region";
    }
    {
        FieldDef alias = choice("alias", "as written", {kPeriRx}, true);
        alias.optional = true;
        alias.when = "alias!=";
        FieldDef first = field("first", ""), last = field("last", "");
        first.optional = last.optional = true;
        value(regionBlockType(kPeriRx).c_str(), "RX buffer", "Peripheral", kPeri, regionType(kPeriRx), {alias, first, last},
              {input("first", "", intOnly, true), input("last", "to", intOnly, true)}, "{alias}");
        r.edit(regionBlockType(kPeriRx))->semantic = "region";
    }
    // ---- references and options (components for the choice fields)
    value("label_ref", "label", "Control", kBoth, "label.flow", {field("name", "")}, {}, "{name}");
    value("proc_ref", "procedure", "Control", kBoth, "label.proc", {field("name", "")}, {}, "{name}");
    {
        std::vector<std::string> alu;
        for (const lang::Entry* e : L.all(lang::Tok::SubOp, lang::Dom::None)) if (e->group == "alu") alu.push_back(e->name);
        value("alu_op", "operation", "Math", kMain, "op.alu", {choice("value", "", alu, true)}, {}, "{value}");
        value("sign_mode", "sign", "Math", kMain, "sign", {choice("value", "", {"unsigned", "signed"}, true)}, {}, "{value}");
        std::vector<std::string> mc, pc;
        for (const lang::Entry* e : L.all(lang::Tok::Condition, lang::Dom::Main)) mc.push_back(e->name);
        for (const lang::Entry* e : L.all(lang::Tok::Mnemonic, lang::Dom::Peri))
            if (e->name.compare(0, 3, "BR.") == 0 && e->name != "BR.COND" && e->name != "BR.ERET") pc.push_back(e->name.substr(3));
        value("cond_main_c", "condition", "Control", kMain, "cond.main", {choice("value", "", mc, true)}, {}, "{value}");
        value("cond_peri_c", "condition", "Control", kPeri, "cond.peri", {choice("value", "", pc, true)}, {}, "{value}");
    }

    // ---- the sockets of the instructions (the same-named field is the text mirror)
    auto socket = [&](const char* type, const char* name, std::vector<std::string> accepts, const char* derive = "", const char* when = "") {
        BlockDef* d = r.edit(type);
        if (!d) return;
        InputDef in;
        in.name = name;
        in.accepts = std::move(accepts);
        for (const FieldDef& f : d->fields) if (f.name == name) { in.label = f.label; in.optional = true; }       // a mirror: the text may stand in
        in.optional = true;
        in.derive = derive;
        in.when = when;
        d->inputs.push_back(in);
    };
    std::vector<std::string> readable = {kTypeInt}, writable;
    for (const isa::RegionInfo& ri : isa::kRegions) {
        if (ri.readable) readable.push_back(regionType(ri.name));
        if (ri.writable) writable.push_back(regionType(ri.name));
    }
    socket("mov", "src", readable);
    socket("mov", "dst", writable);
    socket("alu", "op", {"op.alu"});
    socket("alu", "amount", intOnly, "", "op=SHL,SHR");
    socket("alu", "mode", {"sign"}, "", "op!=AND,OR,XOR,NOT");
    for (const char* t : {"csr_write8", "csr_set8", "csr_clr8"}) { socket(t, "csr", intOnly); socket(t, "imm", intOnly); }
    socket("csr_read8", "csr", intOnly);
    socket("csr_read8", "dst", {regionType("MAIN_CSR_DATA_BUF")});
    socket("csr_write_sp", "csr", intOnly);
    socket("csr_write_sp", "window", {regionType("MAIN_SCRATCHPAD")}, "range:ptr,len");
    socket("csr_read_buf", "csr", intOnly);
    socket("csr_read_buf", "window", {regionType("MAIN_CSR_DATA_BUF")}, "range:ptr,len");
    socket("br_cond", "cond", {"cond.main"});
    socket("br_cond", "target", {"label.flow"});
    socket("jmp", "target", {"label.flow"});
    socket("br_peri", "cond", {"cond.peri"});
    socket("br_peri", "target", {"label.flow"});
    socket("call", "target", {"label.proc"});
    socket("pub", "window", {regionType(kPeriRx)}, "range:src,len");
    // the window sockets have no mirror field of their own; their text lives in the fields they derive
    for (const char* t : {"csr_write_sp", "csr_read_buf", "pub"}) {
        BlockDef* d = r.edit(t);
        if (!d) continue;
        for (InputDef& in : d->inputs) if (in.name == "window") { in.label = "range"; }
    }
}

}  // namespace detail

// ---- text of components ------------------------------------------------------------------------------------------------------------------
namespace {

const InputDef* inputOf(const BlockDef& d, const std::string& name) {
    for (const InputDef& in : d.inputs) if (in.name == name) return &in;
    return nullptr;
}

bool takesConstant(const InputDef& in) {                      // a socket that takes a region AND an int: the int is a constant (CONST(...))
    bool region = false, integer = false;
    for (const std::string& a : in.accepts) { region = region || isRegionType(a); integer = integer || a == kTypeInt; }
    return region && integer;
}

bool simpleToken(const std::string& t) {
    if (t.empty()) return false;
    for (char c : t) if (!std::isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

std::string wrapIfNeeded(const std::string& text) { return simpleToken(text) ? text : "(" + text + ")"; }

std::string semanticOf(const Block& v, const Registry& reg) {
    const BlockDef* d = reg.find(v.type);
    return d ? d->semantic : std::string();
}

int precedenceOf(const Block& v, const Registry& reg) {
    const std::string sem = semanticOf(v, reg);
    if (sem == "binop") return binPrec(v.get("op"));
    if (sem == "unop") return 7;
    if (sem == "number")
        if (const Block* c = v.value("value")) return precedenceOf(*c, reg);
    return 8;
}

// the child's text, with parentheses where the assembler's precedence would otherwise change the meaning
std::string operandText(const Block& parent, const BlockDef& pd, const char* socket, int minPrec, const Registry& reg) {
    const std::string name = socket;
    if (const Block* c = parent.value(name)) {
        const std::string t = valueText(*c, reg, inputOf(pd, name));
        return precedenceOf(*c, reg) < minPrec ? "(" + t + ")" : t;
    }
    const std::string t = parent.get(name);                  // nothing plugged: the text typed (or "?" for a hole)
    if (t.empty()) return "?";
    return minPrec > 0 && !simpleToken(t) ? "(" + t + ")" : t;
}

std::string hex(unsigned long long v, int digits) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%0*llX", digits, v);
    return b;
}

// a number block's text: float and double are written as their IEEE-754 bit pattern (the assembler has integers only)
std::string numberBase(const Block& v, const Registry& reg) {
    std::string t = v.value("value") ? valueText(*v.value("value"), reg, nullptr) : v.get("value");
    const std::string kind = v.get("kind");
    if (kind == "float" || kind == "double") {
        char* end = nullptr;
        const double d = std::strtod(t.c_str(), &end);
        if (end != t.c_str() && *end == 0) {
            if (kind == "float") {
                const float f = (float)d;
                uint32_t bits;
                std::memcpy(&bits, &f, 4);
                return hex(bits, 8);
            }
            uint64_t bits;
            std::memcpy(&bits, &d, 8);
            return hex(bits, 16);
        }
    }
    return t;
}

int constBytes(const Block& v) {                              // the CONST width of a number block, 0 = not stated (the destination decides)
    const std::string kind = v.get("kind"), w = v.get("width");
    if (kind == "float" && (w.empty() || w == "32")) return 4;
    if (kind == "double") return 8;
    long long bits = 0;
    if (!w.empty() && parseNumberLiteral(w, bits)) return (int)(bits / 8);
    return 0;
}

std::string renderRegion(const Block& v, const BlockDef& d, const Registry& reg) {
    const std::string canonical = canonicalOf(d);
    const std::string alias = v.get("alias");
    const std::string name = alias.empty() ? canonical : alias;
    if (!hasRange(canonical) && canonical != kPeriRx) return name;
    const std::string item = v.get("item");
    if (!item.empty()) return name + "[@" + item + "]";
    const bool hasFirst = v.value("first") || !v.get("first").empty();
    if (!hasFirst) return name;
    const std::string first = operandText(v, d, "first", 0, reg);
    const bool hasLast = v.value("last") || !v.get("last").empty();
    if (!hasLast) return name + "[" + first + "]";
    return name + "[" + first + ".." + operandText(v, d, "last", 0, reg) + "]";
}

}  // namespace

std::string valueText(const Block& v, const Registry& reg, const InputDef* into) {
    const BlockDef* d = reg.find(v.type);
    if (!d) return "?";
    std::string text;
    const std::string sem = d->semantic;
    if (sem == "number") {
        text = numberBase(v, reg);
        if (into && takesConstant(*into)) {
            const int bytes = constBytes(v);
            text = "CONST(" + text + (bytes > 0 ? ", " + std::to_string(bytes) : std::string()) + ")";
        }
        return text;
    }
    if (sem == "binop") {
        const int p = binPrec(v.get("op"));
        text = operandText(v, *d, "a", p, reg) + " " + v.get("op") + " " + operandText(v, *d, "b", p + 1, reg);   // left-associative: the right side needs parentheses at equal precedence
    } else if (sem == "unop") {
        text = v.get("op") + operandText(v, *d, "x", 7, reg);
    } else if (sem == "paren") {
        text = "(" + operandText(v, *d, "x", 0, reg) + ")";
    } else if (sem == "region") {
        text = renderRegion(v, *d, reg);
    } else {
        text = detail::renderTemplate(d->tmpl, v, reg);
    }
    if (into && takesConstant(*into) && d->valueType == kTypeInt) text = "CONST(" + text + ")";    // an int expression in a source socket is a constant
    return text;
}

std::string partText(const Block& b, const BlockDef& d, const std::string& name, const Registry& reg) {
    if (const Block* c = b.value(name)) return valueText(*c, reg, inputOf(d, name));
    for (const InputDef& in : d.inputs) {
        if (in.derive.compare(0, 6, "range:") != 0) continue;
        const Block* w = b.value(in.name);
        if (!w) continue;
        const std::string spec = in.derive.substr(6);
        const size_t comma = spec.find(',');
        const std::string ptrField = spec.substr(0, comma), lenField = spec.substr(comma + 1);
        if (name != ptrField && name != lenField) continue;
        const BlockDef* wd = reg.find(w->type);
        if (!wd) continue;
        const std::string first = operandText(*w, *wd, "first", 6, reg);
        const bool hasLast = w->value("last") || !w->get("last").empty();
        const std::string lastRaw = hasLast ? operandText(*w, *wd, "last", 0, reg) : first;
        if (name == ptrField) return first == "?" ? "" : operandText(*w, *wd, "first", 0, reg);
        long long a = 0, z = 0;
        const bool numA = parseNumberLiteral(first, a);
        if (!hasLast) return "1";
        if (lastRaw[0] == '+') {                                                   // "first..+n": n more bytes after the first
            long long n = 0;
            if (parseNumberLiteral(lastRaw.substr(1), n)) return std::to_string(n + 1);
            return lastRaw.substr(1) + " + 1";
        }
        if (numA && parseNumberLiteral(lastRaw, z)) return std::to_string(z - a + 1);
        return wrapIfNeeded(lastRaw) + " - " + wrapIfNeeded(first) + " + 1";
    }
    return b.get(name);
}

// ---- text -> components ------------------------------------------------------------------------------------------------------------------
namespace {

struct Parse {
    Document& doc;
    const Registry& reg;
    const std::string& s;
    size_t p = 0;

    Parse(Document& d, const Registry& r, const std::string& text) : doc(d), reg(r), s(text) {}
    void ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) p++; }
    bool atEnd() { ws(); return p >= s.size(); }

    Block make(const char* type) { return doc.make(type); }
    void plug(Block& parent, const char* socket, Block child) {
        parent.setValue(socket, child);
    }

    // expression grammar of util.cpp: | < ^ < & < shifts < + - < * / % < unary < primary, all left-associative
    bool primary(Block& out) {
        ws();
        if (p >= s.size()) return false;
        const char c = s[p];
        if (c == '(') {
            p++;
            Block inner;
            if (!expr(inner, 0)) return false;
            ws();
            if (p >= s.size() || s[p] != ')') return false;
            p++;
            out = make("expr_paren");
            plug(out, "x", inner);
            return true;
        }
        if (c == '\'') {
            if (p + 2 < s.size() && s[p + 2] == '\'') {
                out = make("num");
                out.set("value", s.substr(p, 3));
                p += 3;
                return true;
            }
            return false;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t st = p;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '_')) p++;
            out = make("num");
            out.set("value", s.substr(st, p - st));
            return true;
        }
        if (std::isalpha((unsigned char)c) || c == '_' || c == '.' || c == '$') {
            size_t st = p;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '_' || s[p] == '.' || s[p] == '$')) p++;
            out = make("name");
            out.set("name", s.substr(st, p - st));
            return true;
        }
        return false;
    }
    bool unary(Block& out) {
        ws();
        if (p < s.size() && (s[p] == '-' || s[p] == '+' || s[p] == '~')) {
            const char c = s[p++];
            Block x;
            if (!unary(x)) return false;
            out = make("expr_un");
            out.set("op", std::string(1, c));
            plug(out, "x", x);
            return true;
        }
        return primary(out);
    }
    // the operator at p for the level `prec`, or ""
    std::string opAt(int prec) {
        ws();
        if (p >= s.size()) return "";
        const char c = s[p], n = p + 1 < s.size() ? s[p + 1] : 0;
        if (prec == 6 && (c == '*' || c == '/' || c == '%')) return std::string(1, c);
        if (prec == 5 && (c == '+' || c == '-')) return std::string(1, c);
        if (prec == 4 && ((c == '<' && n == '<') || (c == '>' && n == '>'))) return std::string(2, c);
        if (prec == 3 && c == '&') return "&";
        if (prec == 2 && c == '^') return "^";
        if (prec == 1 && c == '|') return "|";
        return "";
    }
    bool expr(Block& out, int level) {                          // level 0: the loosest
        return level == 0 ? binary(out, 1) : false;
    }
    bool binary(Block& out, int prec) {
        if (prec > 6) return unary(out);
        if (!binary(out, prec + 1)) return false;
        for (;;) {
            const std::string op = opAt(prec);
            if (op.empty()) return true;
            p += op.size();
            Block rhs;
            if (!binary(rhs, prec + 1)) return false;
            Block bin = make("expr_bin");
            bin.set("op", op);
            plug(bin, "a", out);
            plug(bin, "b", rhs);
            out = bin;
        }
    }
};

bool parseWholeExpr(Document& doc, const Registry& reg, const std::string& text, Block& out) {
    Parse ps(doc, reg, text);
    if (!ps.expr(out, 0)) return false;
    return ps.atEnd();
}

// Only a plain literal stays a "number" in an int socket; the same goes for names.
bool parseRegionOperand(Document& doc, const Registry& reg, const std::string& text0, Block& out) {
    const std::string text = trim(text0);
    const size_t lb = text.find('[');
    const std::string nameTxt = trim(lb == std::string::npos ? text : text.substr(0, lb));
    if (nameTxt.empty()) return false;
    std::string canonical;
    if (upper(nameTxt) == kPeriRx) canonical = kPeriRx;
    else if (const isa::RegionInfo* ri = isa::regionByName(upper(nameTxt))) canonical = ri->name;
    else return false;
    const BlockDef* d = reg.find(regionBlockType(canonical));
    if (!d) return false;
    out = doc.make(d->type.c_str());
    if (nameTxt != canonical) out.set("alias", nameTxt);
    if (lb == std::string::npos) return true;
    if (!hasRange(canonical) && canonical != kPeriRx) return false;
    if (text.back() != ']') return false;
    const std::string in = trim(text.substr(lb + 1, text.size() - lb - 2));
    if (in.empty()) return false;
    if (in[0] == '@') {
        if (canonical != "MAIN_CONST_POOL") return false;
        out.set("item", trim(in.substr(1)));
        return true;
    }
    const size_t dots = findTop(in, "..");
    const std::string a = trim(dots == std::string::npos ? in : in.substr(0, dots));
    const std::string z = dots == std::string::npos ? std::string() : trim(in.substr(dots + 2));
    Block first, last;
    if (!parseWholeExpr(doc, reg, a, first)) return false;
    out.setValue("first", first);
    if (dots != std::string::npos) {
        if (!parseWholeExpr(doc, reg, z, last)) return false;
        out.setValue("last", last);
    }
    return true;
}

// CONST(value[, bytes]) as a number block with a width
bool parseConstOperand(Document& doc, const Registry& reg, const std::string& text0, Block& out) {
    const std::string text = trim(text0);
    if (upper(text).compare(0, 6, "CONST(") != 0 || text.back() != ')') return false;
    const std::vector<std::string> parts = splitTop(text.substr(6, text.size() - 7));
    if (parts.empty() || parts.size() > 2) return false;
    out = doc.make("num");
    long long lit = 0;
    if (parseNumberLiteral(parts[0], lit) || (!parts[0].empty() && parts[0][0] == '\'' && parts[0].size() == 3)) out.set("value", trim(parts[0]));
    else {
        Block e;
        if (!parseWholeExpr(doc, reg, parts[0], e)) return false;
        out.setValue("value", e);
    }
    if (parts.size() == 2) {
        long long bytes = 0;
        if (!parseNumberLiteral(parts[1], bytes) || bytes < 1 || bytes > 4) return false;
        out.set("width", std::to_string(bytes * 8));
    }
    return true;
}

bool isComponentSocket(const InputDef& in) {                  // numbers and regions are split; option sockets (alu op, condition ...) keep their drop-downs
    for (const std::string& a : in.accepts) if (a == kTypeInt || isRegionType(a)) return true;
    return false;
}

int countBlocks(const Block& b) {
    int n = 1;
    for (const auto& vl : b.values) for (const Block& c : vl.second) n += countBlocks(c);
    return n;
}

// the text a parsed component stands for must be the text it came from
bool sameText(const std::string& a, const std::string& b) { return squeeze(a) == squeeze(b); }

}  // namespace

int explodeBlock(Block& b, Document& doc, const Registry& reg) {
    const BlockDef* d = reg.find(b.type);
    int made = 0;
    if (d) {
        for (const InputDef& in : d->inputs) {
            if (b.value(in.name) || !isComponentSocket(in)) continue;
            if (!in.derive.empty()) {
                // a window from the two numbers it stands for: ptr and len (literals only)
                const std::string spec = in.derive.substr(6);
                const size_t comma = spec.find(',');
                long long ptr = 0, len = 0;
                if (!parseNumberLiteral(b.get(spec.substr(0, comma)), ptr) || !parseNumberLiteral(b.get(spec.substr(comma + 1)), len) || len < 1) continue;
                const std::string region = in.accepts.empty() ? std::string() : in.accepts[0].substr(7);
                const BlockDef* rd = reg.find(regionBlockType(region));
                if (!rd) continue;
                Block w = doc.make(rd->type.c_str());
                Block first = doc.make("num"), last = doc.make("num");
                first.set("value", std::to_string(ptr));
                last.set("value", std::to_string(ptr + len - 1));
                w.setValue("first", first);
                w.setValue("last", last);
                Block probe = b;
                probe.setValue(in.name, w);
                if (sameText(partText(probe, *d, spec.substr(0, comma), reg), b.get(spec.substr(0, comma))) && sameText(partText(probe, *d, spec.substr(comma + 1), reg), b.get(spec.substr(comma + 1)))) {
                    made += countBlocks(w);
                    b.setValue(in.name, w);
                }
                continue;
            }
            const std::string text = trim(b.get(in.name));
            if (text.empty()) continue;
            bool wantsRegion = false;
            for (const std::string& a : in.accepts) wantsRegion = wantsRegion || isRegionType(a);
            Block child;
            bool ok = false;
            if (takesConstant(in)) {                                                   // a source: a region, CONST(...), or an int expression (a constant)
                ok = parseConstOperand(doc, reg, text, child) || parseRegionOperand(doc, reg, text, child);
            } else if (wantsRegion) {
                ok = parseRegionOperand(doc, reg, text, child);
            } else {
                ok = parseWholeExpr(doc, reg, text, child);
            }
            if (!ok) continue;
            const BlockDef* cd = reg.find(child.type);
            bool fits = false;
            for (const std::string& a : in.accepts) if (cd && a == cd->valueType) fits = true;
            if (!fits) continue;
            if (!sameText(valueText(child, reg, &in), text)) continue;               // only where the blocks give back the very same text
            made += countBlocks(child);
            b.setValue(in.name, child);
        }
    }
    for (auto& st : b.stacks) for (Block& c : st.second) made += explodeBlock(c, doc, reg);
    return made;
}

int explodeDocument(Document& doc, const Registry& reg) {
    int n = 0;
    for (Section& s : doc.sections) for (Block& b : s.blocks) n += explodeBlock(b, doc, reg);
    return n;
}

namespace {
// Drops the components whose text differs from the mirror (the text wins), everywhere.
void dropStale(Block& b, const Registry& reg) {
    const BlockDef* d = reg.find(b.type);
    if (d) {
        for (auto it = b.values.begin(); it != b.values.end();) {
            const InputDef* in = inputOf(*d, it->first);
            bool drop = false;
            if (in && !it->second.empty()) {
                const std::string* mirror = b.field(it->first);
                if (mirror && !sameText(valueText(it->second[0], reg, in), *mirror)) drop = true;
                if (in && !in->derive.empty()) {
                    const std::string spec = in->derive.substr(6);
                    const size_t comma = spec.find(',');
                    const std::string pf = spec.substr(0, comma), lf = spec.substr(comma + 1);
                    if ((b.field(pf) && !sameText(partText(b, *d, pf, reg), *b.field(pf))) || (b.field(lf) && !sameText(partText(b, *d, lf, reg), *b.field(lf)))) drop = true;
                }
            }
            if (drop) it = b.values.erase(it);
            else ++it;
        }
    }
    for (auto& st : b.stacks) for (Block& c : st.second) dropStale(c, reg);
}
}  // namespace

void reconcileDocument(Document& doc, const Registry& reg) {
    for (Section& s : doc.sections) for (Block& b : s.blocks) dropStale(b, reg);
    explodeDocument(doc, reg);
}

// ---- checking the components -------------------------------------------------------------------------------------------------------------
namespace detail {

namespace {
// "-5", "+3" and the plain literals: what a person types into a number block
bool signedLiteral(const std::string& text0, long long& out) {
    std::string t = trim(text0);
    bool neg = false;
    if (!t.empty() && (t[0] == '-' || t[0] == '+')) { neg = t[0] == '-'; t = trim(t.substr(1)); }
    long long v = 0;
    if (!parseNumberLiteral(t, v)) return false;
    out = neg ? -v : v;
    return true;
}

bool literalOf(const Block& v, const std::string& socket, long long& out) {       // a part that is a plain number (a number block or typed text)
    if (const Block* c = v.value(socket)) return c->type == "num" && !c->value("value") && c->get("kind").empty() && signedLiteral(c->get("value"), out);
    return signedLiteral(v.get(socket), out);
}

std::string rangeText(long long lo, long long hi) { return std::to_string(lo) + ".." + std::to_string(hi); }

// the bytes a region operand covers if it is known (numbers only); -1 otherwise
int knownLength(const Block& v, const Registry& reg) {
    const BlockDef* d = reg.find(v.type);
    if (!d) return -1;
    if (d->semantic == "region") {
        const std::string c = canonicalOf(*d);
        if (c == "ALU_OPA" || c == "ALU_OPB") return 2;
        long long a = 0, z = 0;
        if (!literalOf(v, "first", a)) return -1;
        if (!v.value("last") && v.get("last").empty()) return 1;
        return literalOf(v, "last", z) && z >= a ? (int)(z - a + 1) : -1;
    }
    if (semanticOf(v, reg) == "number") return constBytes(v) > 0 ? constBytes(v) : -1;
    return -1;
}

bool isConstantSource(const Block& v, const Registry& reg) {
    const BlockDef* d = reg.find(v.type);
    if (!d) return false;
    if (d->valueType == kTypeInt) return true;
    return canonicalOf(*d) == "MAIN_CONST_POOL";
}
}  // namespace

void componentProblems(const Block& b, const BlockDef& d, const Registry& reg, std::vector<BlockProblem>& out) {
    auto err = [&](const std::string& m, bool error = true) { out.push_back({error, b.id, m}); };
    if (d.semantic == "number") {
        const std::string kind = b.get("kind"), sign = b.get("sign"), width = b.get("width");
        if (!kind.empty() && kind != "int" && kind != "float" && kind != "double") err("a number is int, float or double, not '" + kind + "'");
        if (!sign.empty() && sign != "signed" && sign != "unsigned") err("the sign of a number is signed or unsigned, not '" + sign + "'");
        long long bits = 0;
        if (!width.empty() && !(parseNumberLiteral(width, bits) && (bits == 8 || bits == 16 || bits == 24 || bits == 32 || bits == 64))) err("the width of a number is 8, 16, 24, 32 or 64 bits, not '" + width + "'");
        if (!b.value("value")) {
            const std::string t = b.get("value");
            if (t.empty()) err("the number has no value");
            else if (kind == "float" || kind == "double") {
                char* end = nullptr;
                std::strtod(t.c_str(), &end);
                if (end == t.c_str() || *end != 0) err("'" + t + "' is not a " + kind + " number");
            } else {
                long long v = 0;
                if (signedLiteral(t, v) && bits >= 8 && bits <= 32) {
                    const long long umax = (1LL << bits) - 1, smin = -(1LL << (bits - 1)), smax = (1LL << (bits - 1)) - 1;
                    const bool ok = sign == "unsigned" ? (v >= 0 && v <= umax) : sign == "signed" ? (v >= smin && v <= smax) : (v >= smin && v <= umax);
                    if (!ok) err(t + " does not fit " + std::to_string(bits) + " bits (" + (sign == "signed" ? rangeText(smin, smax) : sign == "unsigned" ? rangeText(0, umax) : rangeText(smin, umax)) + ")");
                }
            }
        }
        return;
    }
    if (d.semantic == "region") {
        const std::string c = canonicalOf(d);
        long long a = 0, z = 0;
        const bool hasA = literalOf(b, "first", a), hasZ = literalOf(b, "last", z);
        int size = 0;
        for (const isa::RegionInfo& ri : isa::kRegions) if (c == ri.name) size = ri.size;
        if (c == kPeriRx) size = 32;
        if (hasA && hasZ && z < a) err("the range " + rangeText(a, z) + " is backwards (first must not be above last)");
        if (size > 0) {
            if (hasA && (a < 0 || a >= size)) err("byte " + std::to_string(a) + " is outside " + c + " (0.." + std::to_string(size - 1) + ")");
            if (hasZ && (z < 0 || z >= size)) err("byte " + std::to_string(z) + " is outside " + c + " (0.." + std::to_string(size - 1) + ")");
        }
        if (hasRange(c) && c != "MAIN_CONST_POOL" && !b.value("first") && b.get("first").empty()) err(c + " needs a byte range, e.g. " + c + "[0..1]");
        return;
    }
    if (b.type == "mov") {
        const Block* src = b.value("src");
        const Block* dst = b.value("dst");
        if (src && dst) {
            const BlockDef* dd = reg.find(dst->type);
            if (isConstantSource(*src, reg) && dd && dd->valueType != regionType("MAIN_SCRATCHPAD"))
                err("a constant can only be moved to the scratchpad (spec v31 5.1): load it into the scratchpad first, then move it on");       // H7c / E202
            const int ls = knownLength(*src, reg), ld = knownLength(*dst, reg);
            if (ls > 0 && ld > 0 && ls != ld && !(src->type == "num" && src->get("width").empty())) err("the two sides are " + std::to_string(ls) + " and " + std::to_string(ld) + " bytes long; they must be the same");
        }
    }
    if (b.type == "alu") {
        const std::string op = upper(b.get("op")), mode = b.get("mode");
        if ((op == "EQ" || op == "NE" || op == "CMP.EQ" || op == "CMP.NE") && upper(mode) == "SIGNED") err("EQ and NE have no signed mode (E203)");
        if ((op == "AND" || op == "OR" || op == "XOR" || op == "NOT") && !mode.empty()) err("a logic operation has no signed/unsigned mode (W410)", false);
        if (op == "SHL" || op == "SHR") {
            if (partText(b, d, "amount", reg).empty()) err("a shift needs an amount (1 to 8)");
        } else if (!partText(b, d, "amount", reg).empty()) err("only a shift has an amount", false);
    }
}

std::vector<BlockProblem> subtreeProblems(const Block& host, const Registry& reg) {
    std::vector<BlockProblem> out;
    const BlockDef* d = reg.find(host.type);
    if (!d) return out;
    componentProblems(host, *d, reg, out);
    for (const InputDef& in : d->inputs) {
        const Block* c = host.value(in.name);
        if (!c) continue;
        plugProblems(host, *d, in, *c, reg, out);
        for (const BlockProblem& p : subtreeProblems(*c, reg)) out.push_back(p);
    }
    return out;
}

void plugProblems(const Block& parent, const BlockDef& pd, const InputDef& in, const Block& child, const Registry& reg, std::vector<BlockProblem>& out) {
    (void)parent; (void)pd;
    auto err = [&](const std::string& m) { out.push_back({true, child.id, m}); };
    if (semanticOf(child, reg) != "number") return;
    const std::string kind = child.get("kind");
    if (takesConstant(in)) {
        const int bytes = constBytes(child);
        if (bytes > 4) err("a constant (CONST) holds 1 to 4 bytes; a " + std::string(kind == "double" ? "double" : "64-bit value") + " needs two moves (not available as one block yet)");
    } else if (kind == "float" || kind == "double") {
        err("a " + kind + " number can only be used as a constant (the source of a move); here the assembler needs an integer");
    }
}

}  // namespace detail

}  // namespace blocks
}  // namespace asmcore
