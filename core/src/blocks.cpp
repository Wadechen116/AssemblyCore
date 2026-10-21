// Block programs: model, definitions, text generation, checking, the .acblk file. See include/asmcore/blocks.h.
#include "asmcore/blocks.h"

#include <algorithm>
#include <set>
#include <sstream>

#include "asmcore/lang.h"
#include "blocks_internal.h"
#include "json_lite.h"
#include "util.h"

namespace asmcore {
namespace blocks {

// ---- Block ---------------------------------------------------------------------------------------------------------------------------
const std::string* Block::field(const std::string& name) const {
    for (const auto& kv : fields) if (kv.first == name) return &kv.second;
    return nullptr;
}
std::string Block::get(const std::string& name, const std::string& fallback) const {
    const std::string* f = field(name);
    return f ? *f : fallback;
}
void Block::set(const std::string& name, const std::string& value) {
    for (auto& kv : fields) if (kv.first == name) { kv.second = value; return; }
    fields.emplace_back(name, value);
}
std::vector<Block>* Block::stack(const std::string& name, bool create) {
    for (auto& kv : stacks) if (kv.first == name) return &kv.second;
    if (!create) return nullptr;
    stacks.emplace_back(name, std::vector<Block>());
    return &stacks.back().second;
}
const std::vector<Block>* Block::stack(const std::string& name) const {
    for (const auto& kv : stacks) if (kv.first == name) return &kv.second;
    return nullptr;
}

const Block* Block::value(const std::string& socket) const {
    for (const auto& kv : values) if (kv.first == socket) return kv.second.empty() ? nullptr : &kv.second[0];
    return nullptr;
}
void Block::setValue(const std::string& socket, const Block& b) {
    for (auto& kv : values) if (kv.first == socket) { kv.second.assign(1, b); return; }
    values.emplace_back(socket, std::vector<Block>(1, b));
}
void Block::clearValue(const std::string& socket) {
    for (auto& kv : values) if (kv.first == socket) kv.second.clear();
}

// ---- Document ------------------------------------------------------------------------------------------------------------------------
Block Document::make(const std::string& type) {
    Block b;
    b.id = newId();
    b.type = type;
    if (const BlockDef* d = Registry::defaults().find(type)) {
        for (const FieldDef& f : d->fields) b.fields.emplace_back(f.name, f.def);
        for (const std::string& s : d->stacks) b.stacks.emplace_back(s, std::vector<Block>());
    }
    return b;
}

namespace {
Block* findIn(std::vector<Block>& v, const std::string& id) {
    for (Block& b : v) {
        if (b.id == id) return &b;
        for (auto& st : b.stacks) if (Block* r = findIn(st.second, id)) return r;
        for (auto& vl : b.values) if (Block* r = findIn(vl.second, id)) return r;
    }
    return nullptr;
}
size_t countIn(const std::vector<Block>& v) {
    size_t n = 0;
    for (const Block& b : v) { n++; for (const auto& st : b.stacks) n += countIn(st.second); for (const auto& vl : b.values) n += countIn(vl.second); }
    return n;
}
void assignIn(std::vector<Block>& v, Document& d) {
    for (Block& b : v) {
        if (b.id.empty()) b.id = d.newId();
        for (auto& st : b.stacks) assignIn(st.second, d);
        for (auto& vl : b.values) assignIn(vl.second, d);
    }
}
}  // namespace

Block* Document::find(const std::string& id) {
    for (Section& s : sections) if (Block* b = findIn(s.blocks, id)) return b;
    return nullptr;
}
const Block* Document::find(const std::string& id) const { return const_cast<Document*>(this)->find(id); }
size_t Document::blockCount() const {
    size_t n = 0;
    for (const Section& s : sections) n += countIn(s.blocks);
    return n;
}
void Document::assignMissingIds() {
    for (Section& s : sections) assignIn(s.blocks, *this);
}

// ---- the definitions -----------------------------------------------------------------------------------------------------------------
void Registry::add(const BlockDef& d) { defs_.push_back(d); }
const BlockDef* Registry::find(const std::string& type) const {
    for (const BlockDef& d : defs_) if (d.type == type) return &d;
    return nullptr;
}

const Registry& Registry::defaults() {
    static const Registry reg = [] {
        Registry r;
        const lang::Language L = lang::Language::defaults();
        auto names = [&](lang::Tok kind, lang::Dom dom, const std::string& group = "") {
            std::vector<std::string> v;
            for (const lang::Entry* e : L.all(kind, dom)) if (group.empty() || e->group == group) v.push_back(e->name);
            return v;
        };
        auto F = [](const char* name, const char* label, FieldKind kind = FieldKind::Text, long long lo = 0, long long hi = 0) {
            FieldDef f;
            f.name = name; f.label = label; f.kind = kind; f.lo = lo; f.hi = hi;
            return f;
        };
        auto def = [&](const char* type, const char* title, const char* cat, int dom, std::vector<FieldDef> fields, const char* tmpl, const char* spec = "", const char* help = "") {
            BlockDef d;
            d.type = type; d.title = title; d.category = cat; d.domains = dom; d.fields = std::move(fields); d.tmpl = tmpl; d.spec = spec; d.help = help;
            r.add(d);
        };
        FieldDef flagSafe = F("safe", "event checkpoint", FieldKind::Flag);
        flagSafe.optional = true;
        flagSafe.help = "event_safe: a pending event may be dispatched when this branch retires (spec v31 16.3)";

        // ---- data and math (Main)
        def("mov", "move", "Data", kMain, {F("src", "from", FieldKind::Operand), F("dst", "to", FieldKind::Operand)}, "MOV {src} -> {dst}", "5", "Copy bytes between regions (or load a constant with CONST(value, bytes)).");
        {
            FieldDef op = F("op", "operation", FieldKind::Choice);
            for (const std::string& n : names(lang::Tok::SubOp, lang::Dom::None, "alu")) op.options.push_back(n);
            op.open = true;
            FieldDef amount = F("amount", "by", FieldKind::Int, 1, 8);
            amount.optional = true;
            FieldDef mode = F("mode", "as", FieldKind::Choice);
            mode.options = {"", "unsigned", "signed"};
            mode.open = true;
            mode.optional = true;
            def("alu", "calculate", "Math", kMain, {op, amount, mode}, "ALU {op}[ {amount}][ {mode}]", "6", "Run the ALU on ALU_OPA / ALU_OPB; the result is in ALU_OUT / ALU_STATUS.");
        }
        // ---- CSR
        def("csr_write8", "write CSR", "CSR", kBoth, {F("csr", "register"), F("imm", "value")}, "EXEC_CSR WRITE8 {csr}, {imm}", "4");
        def("csr_set8", "set CSR bits", "CSR", kBoth, {F("csr", "register"), F("imm", "mask")}, "EXEC_CSR SET8 {csr}, {imm}", "4");
        def("csr_clr8", "clear CSR bits", "CSR", kBoth, {F("csr", "register"), F("imm", "mask")}, "EXEC_CSR CLR8 {csr}, {imm}", "4");
        def("csr_read8", "read CSR byte", "CSR", kMain, {F("csr", "register"), F("dst", "into", FieldKind::Operand)}, "EXEC_CSR READ8 {csr} -> {dst}", "4", "Read one CSR byte into MAIN_CSR_DATA_BUF.");
        def("csr_write_sp", "write CSR from scratchpad", "CSR", kMain, {F("csr", "register"), F("len", "bytes", FieldKind::Int, 1, 32), F("ptr", "scratchpad byte", FieldKind::Int, 0, 31)},
            "EXEC_CSR WRITE_SP {csr}, len={len}, ptr={ptr}", "4.3", "Write bytes of MAIN_SCRATCHPAD to consecutive CSRs.");
        def("csr_read_buf", "read CSR bytes", "CSR", kMain, {F("csr", "register"), F("len", "bytes", FieldKind::Int, 1, 32), F("ptr", "buffer byte", FieldKind::Int, 0, 31)},
            "EXEC_CSR READ_BUF {csr}, len={len}, ptr={ptr}", "4.3", "Read consecutive CSRs into MAIN_CSR_DATA_BUF.");
        // ---- control
        {
            FieldDef cond = F("cond", "condition", FieldKind::Choice);
            cond.options = names(lang::Tok::Condition, lang::Dom::Main);
            cond.open = true;
            def("br_cond", "branch if", "Control", kMain, {cond, F("target", "to"), flagSafe}, "BR.COND {cond}, {target}{?safe:, safe}", "7");
        }
        def("jmp", "jump", "Control", kBoth, {F("target", "to"), flagSafe}, "JMP {target}{?safe:, safe}", "7");
        def("br_eret", "return from event", "Control", kBoth, {}, "BR.ERET", "16.5", "End an event handler: back to the interrupted flow.");
        {
            FieldDef cond = F("cond", "condition", FieldKind::Choice);
            for (const lang::Entry* e : L.all(lang::Tok::Mnemonic, lang::Dom::Peri))
                if (e->name.compare(0, 3, "BR.") == 0 && e->name != "BR.COND" && e->name != "BR.ERET") cond.options.push_back(e->name.substr(3));
            cond.open = true;
            def("br_peri", "branch if", "Control", kPeri, {cond, F("target", "to"), flagSafe}, "BR.{cond} {target}{?safe:, safe}", "11.2");
        }
        def("cascade_begin", "cascade begin", "Control", kMain, {F("sel", "run blocks up to"), F("block0", "first block")}, "CASCADE_BEGIN {sel}, {block0}", "7.6");
        def("cascade_end", "cascade end", "Control", kMain, {F("index", "block"), F("exit", "then go to")}, "CASCADE_END {index}, {exit}", "7.6");
        def("call", "call", "Control", kBoth, {F("target", "procedure")}, "CALL {target}", "");
        def("ret", "return", "Control", kBoth, {}, "RET", "");
        // ---- Peri
        {
            FieldDef rw = F("rw", "direction", FieldKind::Choice);
            rw.options = {"READ", "WRITE"};
            rw.open = true;
            def("xfer", "I2C transfer", "Peripheral", kPeri, {rw, F("args", "details")}, "EXEC_XFER {rw} {args}", "8", "dev=..., addr=..., addrlen=..., ptr=... / data=...");
            def("pub", "publish result", "Peripheral", kPeri, {F("service", "service"), F("src", "from RX byte", FieldKind::Int, 0, 31), F("len", "bytes", FieldKind::Int, 1, 32)}, "EXEC_PUB service={service}, src={src}, len={len}", "10");
        }
        // ---- structure and text
        {
            BlockDef p;
            p.type = "proc"; p.title = "procedure"; p.category = "Structure"; p.domains = kBoth;
            p.fields = {F("name", "name")};
            p.stacks = {"BODY"};
            p.structural = true;
            p.help = "A subroutine: CALL name ... RET.";
            r.add(p);
        }
        def("label", "label", "Structure", kBoth, {F("name", "name")}, "{name}:", "");
        def("vector", "event vector", "Structure", kBoth, {F("id", "event"), F("label", "handler")}, ".vector {id}, {label}", "16");
        def("wcet", "WCET query", "Structure", kMain, {F("name", "name"), F("from", "from"), F("to", "to")}, ".wcet {name}, {from}, {to}", "");
        def("assume", "assume defined", "Structure", kBoth, {F("text", "range")}, ".assume {text}", "");
        {
            FieldDef args = F("args", "arguments");
            args.optional = true;
            def("directive", "directive", "Definitions", kBoth, {F("name", "directive"), args}, "{name}[ {args}]", "", "Definitions: .equ .const .csr .csrbit .service .devmap .include ...  (the Hardware Designer owns most of them)");
        }
        def("comment", "comment", "Text", kBoth, {F("text", "text")}, "; {text}");
        def("blank", "blank line", "Text", kBoth, {}, "");
        def("raw", "assembler text", "Text", kBoth, {F("text", "text")}, "{text}", "", "A line the blocks do not cover (yet), kept exactly as written.");
        for (const char* t : {"proc", "label", "comment", "blank", "raw"}) (void)t;
        return r;
    }();
    return reg;
}

// ---- text generation -----------------------------------------------------------------------------------------------------------------
namespace {

// the text of a template for one block
std::string render(const std::string& tmpl, const Block& b, const Registry& reg);

// the expression a value block stands for
std::string expression(const Block& v, const Registry& reg) {
    const BlockDef* d = reg.find(v.type);
    return d ? render(d->tmpl, v, reg) : std::string("?");
}

std::string render(const std::string& tmpl, const Block& b, const Registry& reg) {
    std::string out;
    for (size_t i = 0; i < tmpl.size(); i++) {
        char c = tmpl[i];
        if (c == '{') {
            size_t e = tmpl.find('}', i);
            if (e == std::string::npos) { out += c; continue; }
            std::string name = tmpl.substr(i + 1, e - i - 1);
            if (!name.empty() && name[0] == '?') {                               // {?flag:text}
                size_t colon = name.find(':');
                std::string flag = name.substr(1, colon == std::string::npos ? std::string::npos : colon - 1);
                if (colon != std::string::npos && b.get(flag) == "1") out += name.substr(colon + 1);
            } else if (b.field(name)) out += b.get(name);
            else if (const Block* in = b.value(name)) out += expression(*in, reg);
            i = e;
        } else if (c == '[') {
            size_t e = tmpl.find(']', i);
            if (e == std::string::npos) { out += c; continue; }
            std::string inner = tmpl.substr(i + 1, e - i - 1);
            bool all = true;
            for (size_t k = 0; k < inner.size(); k++)
                if (inner[k] == '{') {
                    size_t ke = inner.find('}', k);
                    if (ke == std::string::npos) break;
                    const std::string key = inner.substr(k + 1, ke - k - 1);
                    if (b.get(key).empty() && !b.value(key)) all = false;
                    k = ke;
                }
            if (all) out += render(inner, b, reg);
            i = e;
        } else out += c;
    }
    return out;
}

struct Gen {
    const Registry& reg;
    Generated out;
    int line = 0;
    void emit(const std::string& text, const std::string& id) {
        out.text += text + "\n";
        out.lines.push_back({++line, id});
    }
    void block(const Block& b) {
        const BlockDef* d = reg.find(b.type);
        std::string stmt;
        int indent = 4;
        if (b.type == "blank") { emit("", b.id); return; }
        if (b.type == "comment") { emit("; " + b.get("text"), b.id); return; }
        if (b.type == "raw") {
            stmt = b.get("text");
            indent = !stmt.empty() && stmt[0] == '.' ? 0 : 4;
        } else if (b.type == "proc") {
            emit(".proc " + b.get("name") + (b.comment.empty() ? "" : "    ; " + b.comment), b.id);
            if (const std::vector<Block>* body = b.stack("BODY")) for (const Block& c : *body) block(c);
            emit(".endp", b.id);
            return;
        } else if (!d) {
            stmt = "; unknown block '" + b.type + "'";
            indent = 0;
        } else {
            stmt = render(d->tmpl, b, reg);
            if (b.type == "label" || b.type == "directive" || b.type == "vector" || b.type == "wcet" || b.type == "assume") indent = 0;
        }
        if (b.disabled) { emit("; " + stmt, b.id); return; }
        if (!b.comment.empty()) stmt += "    ; " + b.comment;
        emit(std::string((size_t)indent, ' ') + stmt, b.id);
    }
};

}  // namespace

std::vector<size_t> programOrder(const Document& doc) {
    std::vector<size_t> defs, code;
    for (size_t i = 0; i < doc.sections.size(); i++) {
        const std::string& d = doc.sections[i].domain;
        if (d.empty()) defs.push_back(i);
        else if (d == "main" || d == "peri") code.push_back(i);                      // "floating" stacks are not part of the program
    }
    auto before = [&](size_t a, size_t b) {
        const Section &x = doc.sections[a], &y = doc.sections[b];
        if (x.y != y.y) return x.y < y.y;
        return x.x < y.x;
    };
    std::stable_sort(defs.begin(), defs.end(), before);
    std::stable_sort(code.begin(), code.end(), before);
    defs.insert(defs.end(), code.begin(), code.end());
    return defs;
}

Generated generate(const Document& doc, const Registry& reg, const GenerateOptions& opt) {
    Gen g{reg, {}, 0};
    if (!opt.header.empty()) {
        std::istringstream hs(opt.header);
        for (std::string l; std::getline(hs, l);) g.emit("; " + l, "");
    }
    for (size_t si : programOrder(doc)) {
        const Section& s = doc.sections[si];
        if (!s.domain.empty()) {
            if (g.line > 0) g.emit("", "");                                  // one blank line between sections
            g.emit("." + s.domain + (s.comment.empty() ? "" : "    ; " + s.comment), "");
        }
        for (const Block& b : s.blocks) g.block(b);
    }
    return g.out;
}

const GenLine* Generated::atLine(int line) const {
    if (line < 1 || line > (int)lines.size()) return nullptr;
    return &lines[(size_t)line - 1];
}
std::vector<int> Generated::linesOf(const std::string& blockId) const {
    std::vector<int> v;
    for (const GenLine& l : lines) if (l.blockId == blockId) v.push_back(l.line);
    return v;
}

// ---- checking ------------------------------------------------------------------------------------------------------------------------
namespace {

void checkBlock(const Block& b, const std::string& domain, bool inProc, bool asValue, const Registry& reg, std::vector<BlockProblem>& out, std::set<std::string>& ids);

void checkList(const std::vector<Block>& v, const std::string& domain, bool inProc, const Registry& reg, std::vector<BlockProblem>& out, std::set<std::string>& ids) {
    for (const Block& b : v) checkBlock(b, domain, inProc, false, reg, out, ids);
}

void checkBlock(const Block& b, const std::string& domain, bool inProc, bool asValue, const Registry& reg, std::vector<BlockProblem>& out, std::set<std::string>& ids) {
    auto err = [&](const std::string& m, bool error = true) { out.push_back({error, b.id, m}); };
    if (b.id.empty()) err("the block has no id");
    else if (!ids.insert(b.id).second) err("the id " + b.id + " is used by another block");
    const BlockDef* d = reg.find(b.type);
    if (!d) { err("unknown block type '" + b.type + "'"); return; }
    if (asValue && d->valueType.empty()) err("'" + d->title + "' is a statement; it cannot be plugged into a value socket");
    if (!asValue && !d->valueType.empty()) err("'" + d->title + "' is a value; it must be plugged into a socket, not stand in a stack");
    const bool floating = domain == "floating";
    if (!floating && !asValue) {
        if (domain.empty() && (d->category == "Data" || d->category == "Math" || d->category == "CSR" || d->category == "Control" || d->category == "Peripheral"))
            err("'" + d->title + "' needs a .main or .peri section before it");
        else if (domain == "main" && !(d->domains & kMain)) err("'" + d->title + "' is a Peri block; it cannot be in the Main section");
        else if (domain == "peri" && !(d->domains & kPeri)) err("'" + d->title + "' is a Main block; it cannot be in the Peri section");
    }
    for (const FieldDef& f : d->fields) {
        const std::string* val = b.field(f.name);
        const std::string v2 = val ? *val : std::string();
        if (f.kind == FieldKind::Flag) continue;
        if (v2.empty() && !f.optional && b.type != "raw") err("'" + f.label + "' is empty");
        if (f.kind == FieldKind::Choice && !f.open && !v2.empty() && std::find(f.options.begin(), f.options.end(), v2) == f.options.end()) err("'" + v2 + "' is not one of the choices for '" + f.label + "'");
    }
    for (const auto& kv : b.fields) {
        bool known = false;
        for (const FieldDef& f : d->fields) if (f.name == kv.first) known = true;
        if (!known) err("the field '" + kv.first + "' does not belong to this block", false);
    }
    for (const auto& st : b.stacks) {
        if (std::find(d->stacks.begin(), d->stacks.end(), st.first) == d->stacks.end()) { err("'" + b.type + "' has no slot '" + st.first + "'"); continue; }
        if (b.type == "proc" && inProc) err("procedures do not nest");
        checkList(st.second, domain, inProc || b.type == "proc", reg, out, ids);
    }
    for (const InputDef& in : d->inputs) {
        const Block* c = b.value(in.name);
        if (!c) { if (!in.optional) err("'" + in.label + "' needs a block plugged in"); continue; }
        const BlockDef* cd = reg.find(c->type);
        if (cd && !cd->valueType.empty() && !in.accepts.empty() && std::find(in.accepts.begin(), in.accepts.end(), cd->valueType) == in.accepts.end()) {
            std::string acc;
            for (const std::string& a2 : in.accepts) acc += (acc.empty() ? "" : ", ") + a2;
            out.push_back({true, c->id, "'" + cd->title + "' (" + cd->valueType + ") does not fit into '" + in.label + "', which takes " + acc});
        }
        checkBlock(*c, domain, inProc, true, reg, out, ids);
    }
    for (const auto& kv : b.values) {
        bool known = false;
        for (const InputDef& in : d->inputs) if (in.name == kv.first) known = true;
        if (!known && !kv.second.empty()) err("'" + b.type + "' has no value socket '" + kv.first + "'");
    }
}
}  // namespace

std::vector<BlockProblem> validate(const Document& doc, const Registry& reg) {
    std::vector<BlockProblem> out;
    std::set<std::string> ids;
    for (const Section& s : doc.sections) {
        if (s.domain != "" && s.domain != "main" && s.domain != "peri" && s.domain != "floating") out.push_back({true, "", "unknown section '" + s.domain + "' (main, peri or floating)"});
        checkList(s.blocks, s.domain, false, reg, out, ids);
    }
    return out;
}

// ---- the file ------------------------------------------------------------------------------------------------------------------------
namespace {

json::Value toJson(const Block& b) {
    json::Value v = json::Value::makeObject();
    v.set("id", json::Value::makeString(b.id));
    v.set("type", json::Value::makeString(b.type));
    if (!b.fields.empty()) {
        json::Value f = json::Value::makeObject();
        for (const auto& kv : b.fields) f.set(kv.first, json::Value::makeString(kv.second));
        v.set("fields", f);
    }
    if (!b.comment.empty()) v.set("comment", json::Value::makeString(b.comment));
    if (b.disabled) v.set("disabled", json::Value::makeBool(true));
    if (b.collapsed) v.set("collapsed", json::Value::makeBool(true));
    bool anyValue = false;
    for (const auto& vl : b.values) if (!vl.second.empty()) anyValue = true;
    if (anyValue) {
        json::Value vs = json::Value::makeObject();
        for (const auto& vl : b.values) if (!vl.second.empty()) vs.set(vl.first, toJson(vl.second[0]));
        v.set("values", vs);
    }
    bool anyStack = false;
    for (const auto& st : b.stacks) if (!st.second.empty() || b.type == "proc") anyStack = true;
    if (anyStack) {
        json::Value s = json::Value::makeObject();
        for (const auto& st : b.stacks) {
            json::Value arr = json::Value::makeArray();
            for (const Block& c : st.second) arr.push(toJson(c));
            s.set(st.first, arr);
        }
        v.set("stacks", s);
    }
    return v;
}

bool fromJson(const json::Value& v, Block& b, const std::string& where, std::string& err) {
    if (v.kind != json::Value::Object) { err = where + ": a block must be an object"; return false; }
    auto str = [&](const char* key, std::string& out, bool required) {
        const json::Value* x = v.get(key);
        if (!x) { if (required) err = where + ": missing \"" + key + "\""; return !required; }
        if (x->kind != json::Value::String) { err = where + ": \"" + key + "\" must be a string"; return false; }
        out = x->str;
        return true;
    };
    if (!str("type", b.type, true)) return false;
    if (!str("id", b.id, false)) return false;
    str("comment", b.comment, false);
    if (const json::Value* d = v.get("disabled")) b.disabled = d->kind == json::Value::Bool && d->b;
    if (const json::Value* d = v.get("collapsed")) b.collapsed = d->kind == json::Value::Bool && d->b;
    if (const json::Value* f = v.get("fields")) {
        if (f->kind != json::Value::Object) { err = where + ": \"fields\" must be an object"; return false; }
        for (const auto& kv : f->obj) {
            std::string val;
            if (kv.second.kind == json::Value::String) val = kv.second.str;
            else if (kv.second.kind == json::Value::Number) val = std::to_string((long long)kv.second.num);
            else if (kv.second.kind == json::Value::Bool) val = kv.second.b ? "1" : "";
            else { err = where + ": field \"" + kv.first + "\" must be a string"; return false; }
            b.fields.emplace_back(kv.first, val);
        }
    }
    if (const json::Value* vs = v.get("values")) {
        if (vs->kind != json::Value::Object) { err = where + ": \"values\" must be an object"; return false; }
        for (const auto& kv : vs->obj) {
            Block c;
            if (!fromJson(kv.second, c, where + ".values." + kv.first, err)) return false;
            b.values.emplace_back(kv.first, std::vector<Block>(1, std::move(c)));
        }
    }
    if (const json::Value* s = v.get("stacks")) {
        if (s->kind != json::Value::Object) { err = where + ": \"stacks\" must be an object"; return false; }
        for (const auto& kv : s->obj) {
            if (kv.second.kind != json::Value::Array) { err = where + ": stack \"" + kv.first + "\" must be an array"; return false; }
            std::vector<Block> list;
            for (size_t i = 0; i < kv.second.arr.size(); i++) {
                Block c;
                if (!fromJson(kv.second.arr[i], c, where + "." + kv.first + "[" + std::to_string(i) + "]", err)) return false;
                list.push_back(std::move(c));
            }
            b.stacks.emplace_back(kv.first, std::move(list));
        }
    }
    return true;
}

unsigned maxNumericId(const std::vector<Block>& v) {
    unsigned m = 0;
    for (const Block& b : v) {
        if (b.id.size() > 1 && b.id[0] == 'b') m = std::max<unsigned>(m, (unsigned)std::strtoul(b.id.c_str() + 1, nullptr, 10));
        for (const auto& st : b.stacks) m = std::max(m, maxNumericId(st.second));
        for (const auto& vl : b.values) m = std::max(m, maxNumericId(vl.second));
    }
    return m;
}

}  // namespace

std::string serializeDocument(const Document& doc) {
    json::Value root = json::Value::makeObject();
    root.set("format", json::Value::makeString("acblk"));
    root.set("version", json::Value::makeNumber(doc.version));
    if (!doc.name.empty()) root.set("name", json::Value::makeString(doc.name));
    if (!doc.hardware.empty()) root.set("hardware", json::Value::makeString(doc.hardware));
    if (!doc.output.empty()) root.set("output", json::Value::makeString(doc.output));
    root.set("next_id", json::Value::makeNumber(doc.nextId));
    json::Value secs = json::Value::makeArray();
    for (const Section& s : doc.sections) {
        json::Value sv = json::Value::makeObject();
        sv.set("domain", json::Value::makeString(s.domain));
        if (!s.comment.empty()) sv.set("comment", json::Value::makeString(s.comment));
        sv.set("x", json::Value::makeNumber(s.x));
        sv.set("y", json::Value::makeNumber(s.y));
        json::Value arr = json::Value::makeArray();
        for (const Block& b : s.blocks) arr.push(toJson(b));
        sv.set("blocks", arr);
        secs.push(sv);
    }
    root.set("sections", secs);
    std::string out;
    json::write(root, out);
    return out + "\n";
}

bool parseDocument(const std::string& text, Document& out, std::string& error) {
    out = Document();
    json::Parser p(text);
    json::Value root;
    if (!p.parse(root)) { error = "not a valid .acblk file: " + p.error(); return false; }
    if (root.kind != json::Value::Object) { error = "not a .acblk file: a JSON object was expected"; return false; }
    const json::Value* fmt = root.get("format");
    if (!fmt || fmt->kind != json::Value::String || fmt->str != "acblk") { error = "not a .acblk file (\"format\" is not \"acblk\")"; return false; }
    if (const json::Value* ver = root.get("version")) {
        if (ver->kind != json::Value::Number || ver->num > 1) { error = "this .acblk file is a newer version (" + std::to_string((int)ver->num) + ") than this tool understands (1)"; return false; }
        out.version = (int)ver->num;
    }
    if (const json::Value* n = root.get("name")) if (n->kind == json::Value::String) out.name = n->str;
    if (const json::Value* h = root.get("hardware")) if (h->kind == json::Value::String) out.hardware = h->str;
    if (const json::Value* o = root.get("output")) if (o->kind == json::Value::String) out.output = o->str;
    const json::Value* secs = root.get("sections");
    if (!secs || secs->kind != json::Value::Array) { error = "\"sections\" (an array) is missing"; return false; }
    for (size_t i = 0; i < secs->arr.size(); i++) {
        const json::Value& sv = secs->arr[i];
        const std::string where = "sections[" + std::to_string(i) + "]";
        if (sv.kind != json::Value::Object) { error = where + ": a section must be an object"; return false; }
        Section s;
        if (const json::Value* d = sv.get("domain")) if (d->kind == json::Value::String) s.domain = d->str;
        if (const json::Value* c = sv.get("comment")) if (c->kind == json::Value::String) s.comment = c->str;
        if (const json::Value* px = sv.get("x")) if (px->kind == json::Value::Number) s.x = px->num;
        if (const json::Value* py = sv.get("y")) if (py->kind == json::Value::Number) s.y = py->num;
        const json::Value* blocks = sv.get("blocks");
        if (!blocks || blocks->kind != json::Value::Array) { error = where + ": \"blocks\" (an array) is missing"; return false; }
        for (size_t k = 0; k < blocks->arr.size(); k++) {
            Block b;
            if (!fromJson(blocks->arr[k], b, where + ".blocks[" + std::to_string(k) + "]", error)) return false;
            s.blocks.push_back(std::move(b));
        }
        out.sections.push_back(std::move(s));
    }
    unsigned maxId = 0;
    for (const Section& s : out.sections) maxId = std::max(maxId, maxNumericId(s.blocks));
    out.nextId = std::max<unsigned>(maxId + 1, root.get("next_id") && root.get("next_id")->kind == json::Value::Number ? (unsigned)root.get("next_id")->num : 1);
    out.assignMissingIds();
    return true;
}

namespace detail {
json::Value blockToJson(const Block& b) { return asmcore::blocks::toJson(b); }
bool blockFromJson(const json::Value& v, Block& b, const std::string& where, std::string& err) { return asmcore::blocks::fromJson(v, b, where, err); }
void reid(Block& b, Document& doc) {
    b.id = doc.newId();
    for (auto& st : b.stacks) for (Block& c : st.second) reid(c, doc);
    for (auto& vl : b.values) for (Block& c : vl.second) reid(c, doc);
}
}  // namespace detail

// ---- operands ------------------------------------------------------------------------------------------------------------------------
bool parseOperand(const std::string& text, Operand& out) {
    out = Operand();
    std::string t = trim(text);
    if (t.empty() || t.find('(') != std::string::npos || t.find('@') != std::string::npos) return false;
    size_t lb = t.find('[');
    if (lb == std::string::npos) {
        if (!isIdent(t)) return false;
        out.region = t;
        return true;
    }
    if (t.back() != ']') return false;
    out.region = trim(t.substr(0, lb));
    if (!isIdent(out.region)) return false;
    std::string inner = trim(t.substr(lb + 1, t.size() - lb - 2));
    if (inner.empty()) return false;
    size_t dots = inner.find("..");
    out.isRange = true;
    if (dots == std::string::npos) out.first = inner;
    else { out.first = trim(inner.substr(0, dots)); out.last = trim(inner.substr(dots + 2)); }
    return !out.first.empty() && (dots == std::string::npos || !out.last.empty());
}

std::string formatOperand(const Operand& op) {
    if (!op.isRange) return op.region;
    return op.region + "[" + op.first + (op.last.empty() ? "" : ".." + op.last) + "]";
}

}  // namespace blocks
}  // namespace asmcore
