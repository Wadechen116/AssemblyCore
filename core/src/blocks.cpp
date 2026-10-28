// Block programs: model, definitions, text generation, checking, the .acblk file. See include/asmcore/blocks.h.
#include "asmcore/blocks.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <regex>
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
void Registry::add(const BlockDef& d) {
    for (BlockDef& e : defs_) if (e.type == d.type) { e = d; return; }          // a definition with the same type replaces the old one
    defs_.push_back(d);
}
BlockDef* Registry::edit(const std::string& type) {
    for (BlockDef& d : defs_) if (d.type == type) return &d;
    return nullptr;
}
std::vector<const BlockDef*> Registry::ofClass(const std::string& cls) const {
    std::vector<const BlockDef*> v;
    for (const BlockDef& d : defs_) if (d.cls == cls) v.push_back(&d);
    return v;
}
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
        {
            // Structured control flow. BR.COND jumps only when its condition holds, so each lowers to labels + BR.COND + JMP (see Gen::structured). The condition reads
            // what came right before it (an ALU compare for ALU_CMP_TRUE), the way a hand-written branch does.
            auto structure = [&](const char* type, const char* title, std::vector<std::string> slots, const char* help, const char* condLabel = "condition") {
                BlockDef s;
                s.type = type; s.title = title; s.category = "Control"; s.domains = kMain;
                FieldDef cnd = F("cond", condLabel, FieldKind::Choice);
                cnd.options = names(lang::Tok::Condition, lang::Dom::Main);
                cnd.open = true;
                cnd.def = "ALU_CMP_TRUE";                                                      // (the usual one: the ALU compare before it)
                s.fields = {cnd};
                s.stacks = std::move(slots);
                s.structural = true;
                s.help = help;
                r.add(s);
            };
            structure("if", "if", {"THEN", "ELSE"}, "Runs THEN when the condition holds, ELSE when it does not. Put the ALU compare right before it.");
            structure("while", "while", {"COND", "BODY"}, "Runs COND (the blocks that work out the condition, ending with the ALU compare) and then BODY, again and again while the condition holds.");
            structure("wait", "wait until", {"COND"}, "Runs COND again and again until the condition holds (COND: the blocks that work it out; may be empty for a hardware condition).");
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
        {
            // A repeat: the body between a label and a jump back. "for" counts with the scratchpad and the ALU (the way sample 01 does by hand), "forever" just jumps back.
            BlockDef l;
            l.type = "loop"; l.title = "loop"; l.category = "Control"; l.domains = kMain;
            FieldDef mode = F("mode", "repeat", FieldKind::Choice);
            mode.options = {"for", "forever"};
            mode.def = "for";
            FieldDef count = F("count", "times", FieldKind::Int, 1, 65535);
            count.def = "10";
            count.when = "mode=for";
            FieldDef counter = F("counter", "counter at", FieldKind::Int, 0, 26);
            counter.optional = true;
            counter.when = "mode=for";
            counter.help = "The first of 6 scratchpad bytes the count uses (counter, step, limit). Empty: chosen for you (26, then 20, 14 ... for loops inside loops).";
            l.fields = {mode, count, counter};
            l.stacks = {"BODY"};
            l.structural = true;
            l.help = "Repeats the blocks inside: a number of times (the count lives in 6 scratchpad bytes, the ALU does the counting) or forever.";
            r.add(l);
        }
        {
            // Variables: a name for 2 bytes of the scratchpad, chosen by the generator (see Gen::prepare); the value is an expression of runtime values (blocks of the "val" type).
            auto assign = [&](const char* type, const char* title, const char* socketLabel, const char* help) {
                BlockDef s;
                s.type = type; s.title = title; s.category = "Variables"; s.domains = kMain;
                FieldDef nm = F("name", "variable");
                nm.def = "x";
                s.fields = {nm};
                s.inputs.push_back(InputDef("value", socketLabel, {"val", kTypeInt}, false));
                s.tmpl = "{name} {value}";                                                    // (the order of the parts on the block; the lines come from Gen)
                s.structural = true;
                s.help = help;
                r.add(s);
            };
            assign("set", "set", "to", "Puts the value of an expression into a variable. The variable is a name for 2 bytes of the scratchpad that the generator picks.");
            assign("change", "change", "by", "Adds an expression to a variable (x = x + value).");
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
        detail::addComponentDefs(r, L);
        // the class of every built-in block: components are the value blocks; the rest by drawer
        for (const BlockDef& d0 : r.all()) {
            BlockDef* d = r.edit(d0.type);
            if (!d->valueType.empty()) d->cls = "component";
            else if (d->category == "Structure") d->cls = "structure";
            else if (d->category == "Definitions") d->cls = "definition";
            else if (d->category == "Text") d->cls = "text";
            else d->cls = "instruction";
        }
        return r;
    }();
    return reg;
}

// ---- text generation -----------------------------------------------------------------------------------------------------------------
namespace {

// the text of a template for one block
std::string render(const std::string& tmpl, const Block& b, const Registry& reg);
}  // namespace

namespace detail {
std::string renderTemplate(const std::string& tmpl, const Block& b, const Registry& reg) { return render(tmpl, b, reg); }
}  // namespace detail

namespace {

// the text of what a part of a block is: the component plugged in, the text of the mirror field, a field derived from a window
std::string partOf(const Block& b, const std::string& name, const Registry& reg) {
    const BlockDef* d = reg.find(b.type);
    return d ? partText(b, *d, name, reg) : b.get(name);
}
bool hasPart(const Block& b, const std::string& name, const Registry& reg) {
    if (b.value(name) || b.field(name)) return true;
    const BlockDef* d = reg.find(b.type);
    if (d) for (const InputDef& in : d->inputs) if (!in.derive.empty() && b.value(in.name)) return true;
    return false;
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
            } else if (hasPart(b, name, reg)) out += partOf(b, name, reg);
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
                    if (partOf(b, key, reg).empty()) all = false;
                    k = ke;
                }
            if (all) out += render(inner, b, reg);
            i = e;
        } else out += c;
    }
    return out;
}

// A comment may have several lines (the canvas lets a block carry a longer note). The first goes at the end of the line; the others follow as comment lines of
// their own, so the assembler text stays one comment per line.
std::vector<std::string> commentLines(const std::string& comment) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : comment) {
        if (c == '\r') continue;
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur += c;
    }
    lines.push_back(cur);
    while (lines.size() > 1 && trim(lines.back()).empty()) lines.pop_back();
    return lines;
}

// The first scratchpad byte of the 6 a counting loop uses: what the block says, else chosen by how deep the loop is (26, 20, 14, 8, 2). -1: not a usable number.
int loopCounterBase(const Block& b, int depth) {
    const std::string t = trim(b.get("counter"));
    if (t.empty()) return depth <= 4 ? 26 - 6 * depth : -1;
    char* end = nullptr;
    const long long v = std::strtoll(t.c_str(), &end, 0);
    return end && *end == 0 && v >= 0 && v <= 26 ? (int)v : -1;
}

// ---- variables and expressions -------------------------------------------------------------------------------------------------------
// A variable is 2 bytes of the scratchpad. The generator picks them: from the lowest free byte up, never on a byte another block names (MAIN_SCRATCHPAD[a..b] in any
// text, WRITE_SP's window) or on the bytes of a counting loop. The working values of an expression use free bytes too, and give them back at the end of the statement.
bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}
bool plainNumber(const std::string& t, long long& v) {
    const std::string s = trim(t);
    char* end = nullptr;
    v = std::strtoll(s.c_str(), &end, 0);
    return !s.empty() && end && *end == 0;
}

struct Gen {
    explicit Gen(const Registry& r) : reg(r) {}
    const Registry& reg;
    Generated out;
    int line = 0;
    int loopDepth = 0;
    // ---- variables
    bool taken[32] = {};
    std::map<std::string, int> varAt;
    std::set<std::string> assigned;
    std::vector<int> liveTemps;
    std::set<std::string> reported;
    std::vector<std::string> lineBuf;
    void problem(const std::string& id, const std::string& msg, bool error = true, const char* kind = "structure") {
        if (!reported.insert(id + "|" + msg).second) return;
        BlockProblem p;
        p.error = error; p.blockId = id; p.message = msg; p.kind = kind;
        out.problems.push_back(p);
    }
    void reserve(long long a, long long z) { for (long long i = a; i <= z && i < 32; i++) if (i >= 0) taken[i] = true; }
    // bytes named by hand: "MAIN_SCRATCHPAD[4..7]", "SCRATCH[0]" in any text of any block
    void scanText(const std::string& t) {
        static const std::regex re("(?:SCRATCHPAD|SCRATCH)\\s*\\[\\s*([0-9]+)\\s*(?:\\.\\.\\s*([0-9]+))?\\s*\\]", std::regex::icase);
        for (auto it = std::sregex_iterator(t.begin(), t.end(), re); it != std::sregex_iterator(); ++it) {
            const long long a = std::stoll((*it)[1].str());
            const long long z = (*it)[2].matched ? std::stoll((*it)[2].str()) : a;
            reserve(a, z);
        }
    }
    void scan(const std::vector<Block>& v, int depth, std::vector<std::pair<std::string, std::string>>& order, std::vector<const Block*>& reads) {
        for (const Block& b : v) {
            for (const auto& f : b.fields) scanText(f.second);
            if (b.type == "csr_write_sp") {
                long long ptr = 0, len = 0;
                if (plainNumber(b.get("ptr"), ptr) && plainNumber(b.get("len"), len)) reserve(ptr, ptr + len - 1);
            }
            if (b.type == "loop" && b.get("mode") != "forever") { const int c = loopCounterBase(b, depth); if (c >= 0) reserve(c, c + 5); }
            if (b.type == "set" || b.type == "change") { order.push_back({trim(b.get("name")), b.id}); assigned.insert(trim(b.get("name"))); }
            std::function<void(const Block&)> values = [&](const Block& x) {
                if (x.type == "v_var") { order.push_back({trim(x.get("name")), x.id}); reads.push_back(&x); }
                for (const auto& vl : x.values) for (const Block& c : vl.second) values(c);
            };
            for (const auto& vl : b.values) for (const Block& c : vl.second) values(c);
            for (const auto& st : b.stacks) scan(st.second, b.type == "loop" ? depth + 1 : depth, order, reads);
        }
    }
    void prepare(const Document& doc) {
        std::vector<std::pair<std::string, std::string>> order;
        std::vector<const Block*> reads;
        for (size_t si : programOrder(doc)) scan(doc.sections[si].blocks, 0, order, reads);
        for (const auto& nb : order) {
            const std::string& name = nb.first;
            if (varAt.count(name)) continue;
            if (!isIdentifier(name)) { problem(nb.second, "'" + name + "' is not a name for a variable (letters, digits and _, not starting with a digit)"); varAt[name] = 0; continue; }
            int at = -1;
            for (int i = 0; i + 1 < 32 && at < 0; i++) if (!taken[i] && !taken[i + 1]) at = i;
            if (at < 0) { problem(nb.second, "no room in the scratchpad for the variable '" + name + "' (32 bytes: 2 for each variable, minus the bytes the blocks name and the counting loops use)"); varAt[name] = 0; continue; }
            taken[at] = taken[at + 1] = true;
            varAt[name] = at;
        }
        for (const Block* r : reads)
            if (!assigned.count(trim(r->get("name")))) problem(r->id, "nothing sets the variable '" + trim(r->get("name")) + "'", false, "reference");
    }
    int allocTemp(const std::string& id) {
        for (int i = 0; i + 1 < 32; i++) if (!taken[i] && !taken[i + 1]) { taken[i] = taken[i + 1] = true; liveTemps.push_back(i); return i; }
        problem(id, "no room in the scratchpad for the working values of this expression (use fewer variables or a simpler expression)");
        return 0;
    }
    void freeTemp(int at) {
        auto it = std::find(liveTemps.begin(), liveTemps.end(), at);
        if (it == liveTemps.end()) return;
        taken[at] = taken[at + 1] = false;
        liveTemps.erase(it);
    }
    // ---- expressions: a value is a variable (or a working value) in the scratchpad, or a literal; an operation loads its operands into the ALU, runs it, stores ALU_OUT
    struct Loc { bool lit = false, temp = false; int at = 0; std::string text; };
    void put(const std::string& s) { lineBuf.push_back(s); }
    void load(const Block& host, const Loc& l, const char* alu) {
        if (!l.lit) { put("    MOV " + sp(l.at, l.at + 1) + " -> " + alu); return; }
        const int t = allocTemp(host.id);                                              // a constant goes through the scratchpad (MOV CONST can only write there)
        put("    MOV CONST(" + l.text + ", 2) -> " + sp(t, t + 1));
        put("    MOV " + sp(t, t + 1) + " -> " + alu);
        freeTemp(t);
    }
    std::string signText(const Block& b) { return b.get("mode") == "signed" ? "signed" : "unsigned"; }
    Loc evalValue(const Block* v, const Block& host, int into = -1) {
        Loc zero;
        zero.lit = true; zero.text = "0";
        if (!v) return zero;
        if (v->type == "v_var") {
            Loc l;
            const auto it = varAt.find(trim(v->get("name")));
            l.at = it == varAt.end() ? 0 : it->second;
            return l;
        }
        if (v->type != "v_bin") {                                                       // a number, a name, an assembler expression: a literal
            Loc l;
            l.lit = true;
            l.text = valueText(*v, reg);
            if (trim(l.text).empty()) l.text = "0";
            return l;
        }
        const std::string op = v->get("op");
        Loc a = evalValue(v->value("a"), host), b = evalValue(v->value("b"), host);
        std::string alu;
        const bool shift = op == "<<" || op == ">>";
        if (shift) {
            long long n = 0;
            if (!b.lit || !plainNumber(b.text, n) || n < 0 || n > 31) { problem(v->id, "a shift needs a plain number (0..31) on its right"); n = 0; }
            alu = std::string(op == "<<" ? "SHL " : "SHR ") + std::to_string(n) + " " + signText(*v);
        } else if (op == "+") alu = "ADD " + signText(*v);
        else if (op == "-") alu = "SUB " + signText(*v);
        else if (op == "*") alu = "MUL " + signText(*v);
        else if (op == "&") alu = "AND";
        else if (op == "|") alu = "OR";
        else if (op == "^") alu = "XOR";
        else { problem(v->id, "'" + op + "' is not an operation"); alu = "ADD unsigned"; }
        load(host, a, "ALU_OPA");
        if (!shift) load(host, b, "ALU_OPB");
        put("    ALU " + alu);
        if (a.temp) freeTemp(a.at);
        if (b.temp) freeTemp(b.at);
        Loc r;
        r.at = into >= 0 ? into : allocTemp(host.id);
        r.temp = into < 0;
        put("    MOV ALU_OUT[0..1] -> " + sp(r.at, r.at + 1));
        return r;
    }
    // the ALU compare of a "test" (the blocks before it only load the operands): afterwards the condition is ALU_CMP_TRUE
    void evalTest(const Block& host, const Block* t) {
        if (!t || t->type != "v_cmp") return;
        const std::string op = t->get("op");
        Loc a = evalValue(t->value("a"), host), b = evalValue(t->value("b"), host);
        std::string alu;
        bool swap = false;
        if (op == "==") alu = "CMP.EQ"; else if (op == "!=") alu = "CMP.NE"; else if (op == "<") alu = "CMP.LT"; else if (op == ">=") alu = "CMP.GE";
        else if (op == ">") { alu = "CMP.LT"; swap = true; } else if (op == "<=") { alu = "CMP.GE"; swap = true; }
        else { problem(t->id, "'" + op + "' is not a comparison"); alu = "CMP.EQ"; }
        load(host, swap ? b : a, "ALU_OPA");
        load(host, swap ? a : b, "ALU_OPB");
        put("    ALU " + alu + " " + signText(*t));
        if (a.temp) freeTemp(a.at);
        if (b.temp) freeTemp(b.at);
    }
    void flush(const Block& b, const std::string& fallback) {
        std::vector<std::string> ls;
        ls.swap(lineBuf);
        for (int t : std::vector<int>(liveTemps)) freeTemp(t);
        if (ls.empty()) ls.push_back("    ; " + fallback);
        emitWithComment(ls[0], b.comment, b.id, 4);
        for (size_t i = 1; i < ls.size(); i++) emit(ls[i], b.id);
    }
    void assignBlock(const Block& b) {
        const std::string name = trim(b.get("name"));
        if (b.disabled) { emit("    ; " + b.type + " " + name + " (turned off)", b.id); return; }
        if (!b.value("value")) { emit("    ; (incomplete) " + b.type + " " + name, b.id); return; }
        const int dst = varAt.count(name) ? varAt[name] : 0;
        lineBuf.clear();
        if (b.type == "change") {
            Loc cur; cur.at = dst;
            Loc by = evalValue(b.value("value"), b);
            load(b, cur, "ALU_OPA");
            load(b, by, "ALU_OPB");
            put("    ALU ADD unsigned");
            if (by.temp) freeTemp(by.at);
            put("    MOV ALU_OUT[0..1] -> " + sp(dst, dst + 1));
            flush(b, "change " + name);
            return;
        }
        Loc v = evalValue(b.value("value"), b, dst);
        if (v.lit) put("    MOV CONST(" + v.text + ", 2) -> " + sp(dst, dst + 1));
        else if (v.at != dst) { put("    MOV " + sp(v.at, v.at + 1) + " -> " + sp(dst, dst + 1)); if (v.temp) freeTemp(v.at); }
        flush(b, "set " + name + " (already there)");
    }
    std::string sp(int a, int z) { return "MAIN_SCRATCHPAD[" + std::to_string(a) + ".." + std::to_string(z) + "]"; }
    // if / while / wait: labels + BR.COND (jumps when the condition holds) + JMP. A body is written as the blocks inside it.
    void list(const std::vector<Block>* v) { if (v) for (const Block& x : *v) block(x); }
    void structured(const Block& b) {
        const Block* test = b.value("test");
        const std::string cond = test ? std::string("ALU_CMP_TRUE") : trim(b.get("cond")), top = b.type + "_" + b.id;
        if (b.disabled) { emit("; " + b.type + " " + cond + " (turned off)", b.id); return; }
        const std::string br = "    BR.COND " + (cond.empty() ? std::string("ALWAYS") : cond) + ", ";
        std::vector<std::string> pre;                                                          // a comparison plugged into "test": loads and the ALU compare, right before the branch
        if (test) { lineBuf.clear(); evalTest(b, test); pre.swap(lineBuf); for (int t : std::vector<int>(liveTemps)) freeTemp(t); }
        if (b.type == "if") {
            if (pre.empty()) emitWithComment(br + top + "_then", b.comment, b.id, 4);
            else { emitWithComment(pre[0], b.comment, b.id, 4); for (size_t i = 1; i < pre.size(); i++) emit(pre[i], b.id); emit(br + top + "_then", b.id); }
            list(b.stack("ELSE"));
            emit("    JMP " + top + "_end", b.id);
            emit(top + "_then:", b.id);
            list(b.stack("THEN"));
            emit(top + "_end:", b.id);
        } else if (b.type == "while") {
            emitWithComment(top + ":", b.comment, b.id, 0);
            list(b.stack("COND"));
            for (const std::string& l : pre) emit(l, b.id);
            emit(br + top + "_body", b.id);
            emit("    JMP " + top + "_end", b.id);
            emit(top + "_body:", b.id);
            list(b.stack("BODY"));
            emit("    JMP " + top, b.id);
            emit(top + "_end:", b.id);
        } else {                                                                              // wait
            emitWithComment(top + ":", b.comment, b.id, 0);
            list(b.stack("COND"));
            for (const std::string& l : pre) emit(l, b.id);
            emit(br + top + "_end", b.id);
            emit("    JMP " + top, b.id);
            emit(top + "_end:", b.id);
        }
    }
    void loopBlock(const Block& b) {
        const bool forever = b.get("mode") == "forever";
        const std::string top = "loop_" + b.id, end = top + "_end";
        if (b.disabled) { emit("; loop " + std::string(forever ? "forever" : "for " + trim(b.get("count")) + " times") + " (turned off)", b.id); return; }
        const int c = forever ? 0 : std::max(0, loopCounterBase(b, loopDepth));            // (a bad value is a problem of validate(); here the default keeps the text sane)
        std::string n = trim(b.get("count"));
        if (n.empty()) n = "1";
        if (forever) {
            emitWithComment(top + ":", b.comment, b.id, 0);
        } else {
            emitWithComment("    MOV CONST(0, 2) -> " + sp(c, c + 1), b.comment, b.id, 4);
            emit("    MOV CONST(1, 2) -> " + sp(c + 2, c + 3), b.id);
            emit("    MOV CONST(" + n + ", 2) -> " + sp(c + 4, c + 5), b.id);
            emit(top + ":", b.id);
        }
        loopDepth++;
        if (const std::vector<Block>* body = b.stack("BODY")) for (const Block& x : *body) block(x);
        loopDepth--;
        if (!forever) {
            emit("    MOV " + sp(c, c + 1) + " -> ALU_OPA", b.id);
            emit("    MOV " + sp(c + 2, c + 3) + " -> ALU_OPB", b.id);
            emit("    ALU ADD unsigned", b.id);
            emit("    MOV ALU_OUT[0..1] -> " + sp(c, c + 1), b.id);
            emit("    MOV " + sp(c, c + 1) + " -> ALU_OPA", b.id);
            emit("    MOV " + sp(c + 4, c + 5) + " -> ALU_OPB", b.id);
            emit("    ALU CMP.EQ unsigned", b.id);
            emit("    BR.COND ALU_CMP_TRUE, " + end, b.id);
        }
        emit("    JMP " + top, b.id);
        if (!forever) emit(end + ":", b.id);
    }
    // a line that ends in the comment, and the lines that continue it
    void emitWithComment(const std::string& text, const std::string& comment, const std::string& id, int indent) {
        const std::vector<std::string> ls = commentLines(comment);
        emit(ls[0].empty() && ls.size() == 1 ? text : text + "    ; " + ls[0], id);
        for (size_t i = 1; i < ls.size(); i++) emit(std::string((size_t)indent, ' ') + "; " + ls[i], id);
    }
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
        } else if (b.type == "loop") {
            loopBlock(b);
            return;
        } else if (b.type == "if" || b.type == "while" || b.type == "wait") {
            structured(b);
            return;
        } else if (b.type == "set" || b.type == "change") {
            assignBlock(b);
            return;
        } else if (b.type == "proc") {
            emitWithComment(".proc " + b.get("name"), b.comment, b.id, 0);
            if (const std::vector<Block>* body = b.stack("BODY")) for (const Block& c : *body) block(c);
            emit(".endp", b.id);
            return;
        } else if (!d) {
            stmt = "; unknown block '" + b.type + "'";
            indent = 0;
        } else {
            stmt = render(d->tmpl, b, reg);
            if (d->cls == "instruction") {                                        // an instruction with a hole (an operand taken out, or never filled in) is not written as code
                bool missing = false;
                for (const FieldDef& f : d->fields)
                    if (f.kind != FieldKind::Flag && !f.optional && partVisible(f.when, b) && partText(b, *d, f.name, reg).empty()) missing = true;
                if (missing) { emit("; (incomplete) " + stmt, b.id); return; }
            }
            if (b.type == "label" || b.type == "directive" || b.type == "vector" || b.type == "wcet" || b.type == "assume") indent = 0;
        }
        if (b.disabled) { emit("; " + stmt, b.id); return; }
        if (!b.comment.empty()) { emitWithComment(std::string((size_t)indent, ' ') + stmt, b.comment, b.id, indent); return; }
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
    Gen g(reg);
    g.prepare(doc);
    if (!opt.header.empty()) {
        std::istringstream hs(opt.header);
        for (std::string l; std::getline(hs, l);) g.emit("; " + l, "");
    }
    for (size_t si : programOrder(doc)) {
        const Section& s = doc.sections[si];
        if (!s.domain.empty()) {
            if (g.line > 0) g.emit("", "");                                  // one blank line between sections
            g.emitWithComment("." + s.domain, s.comment, "", 0);
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

// The structure of one list of instructions that spans several blocks: a cascade (BEGIN ... END 0 ... END 1 ..., one common exit, the selector not above the last
// block) and the branches that read the ALU flag (the block before should be the ALU operation).
void checkSequenceHints(const std::vector<Block>& v, const Registry& reg, std::vector<BlockProblem>& out) {
    auto text = [&](const Block& b, const char* name) { const BlockDef* d = reg.find(b.type); return trim(d ? partText(b, *d, name, reg) : b.get(name)); };
    auto live = [](const Block& b) { return b.type != "comment" && b.type != "blank" && !b.disabled; };
    for (size_t i = 0; i < v.size(); i++) {
        const Block& b = v[i];
        if (b.disabled) continue;
        if ((b.type == "br_cond" || b.type == "if") && !b.value("test")) {
            const std::string cond = upper(text(b, "cond"));
            if (cond == "ALU_CMP_TRUE" || cond == "ALU_OUT_OV16") {
                size_t k = i;
                while (k > 0 && !live(v[k - 1])) k--;
                const bool first = k == 0;
                const Block* prev = first ? nullptr : &v[k - 1];
                if (first) out.push_back({false, b.id, "this branch reads the ALU flag, but no ALU operation came before it in this stack (W506)", "hint"});
                else if (prev->type != "alu" && prev->type != "label") out.push_back({false, b.id, "this branch reads the ALU flag, but the block before it is not an ALU operation (W506)", "hint"});
            }
        }
        if (b.type != "cascade_begin") continue;
        // the cascade: the ENDs that follow, numbered 0, 1, 2 ... until the next BEGIN
        long long expect = 0, sel = -1;
        const bool haveSel = parseNumberLiteral(text(b, "sel"), sel);
        std::string exitLabel;
        bool exitsDiffer = false, sawEnd = false;
        for (size_t j = i + 1; j < v.size(); j++) {
            if (v[j].disabled) continue;
            if (v[j].type == "cascade_begin") break;
            if (v[j].type != "cascade_end") continue;
            long long idx = -1;
            const bool haveIdx = parseNumberLiteral(text(v[j], "index"), idx);
            if (haveIdx && idx != expect) {
                out.push_back({true, v[j].id, "the blocks of a cascade end with CASCADE_END 0, 1, 2 ... in order; this is " + std::to_string(idx) + ", expected " + std::to_string(expect) + " (E304)", "structure"});
                expect = idx;
            }
            expect++;
            sawEnd = true;
            const std::string ex = text(v[j], "exit");
            if (exitLabel.empty()) exitLabel = ex;
            else if (ex != exitLabel) exitsDiffer = true;
        }
        if (!sawEnd) out.push_back({true, b.id, "CASCADE_BEGIN without a CASCADE_END after it (E304)", "structure"});
        else {
            if (exitsDiffer) out.push_back({false, b.id, "the CASCADE_END blocks have different exit labels; use one common exit (W405)", "structure"});
            if (haveSel && sel >= expect) out.push_back({true, b.id, "the selector " + std::to_string(sel) + " is above the last block index " + std::to_string(expect - 1) + " (E304)", "structure"});
        }
    }
}

void checkList(const std::vector<Block>& v, const std::string& domain, bool inProc, const Registry& reg, std::vector<BlockProblem>& out, std::set<std::string>& ids) {
    checkSequenceHints(v, reg, out);
    for (const Block& b : v) checkBlock(b, domain, inProc, false, reg, out, ids);
}

void checkBlock(const Block& b, const std::string& domain, bool inProc, bool asValue, const Registry& reg, std::vector<BlockProblem>& out, std::set<std::string>& ids) {
    auto err = [&](const std::string& m, bool error = true) { out.push_back({error, b.id, m}); };
    if (b.id.empty()) err("the block has no id");
    else if (!ids.insert(b.id).second) err("the id " + b.id + " is used by another block");
    const BlockDef* d = reg.find(b.type);
    if (!d) { err("unknown block type '" + b.type + "'"); return; }
    if (asValue && d->valueType.empty()) err("'" + d->title + "' is a statement; it cannot be plugged into a value socket");
    if (!asValue && !d->valueType.empty() && domain != "floating") err("'" + d->title + "' is a value; it must be plugged into a socket, not stand in a stack");
    const bool floating = domain == "floating";
    if (!floating && !asValue) {
        if (domain.empty() && (d->category == "Data" || d->category == "Math" || d->category == "CSR" || d->category == "Control" || d->category == "Peripheral"))
            err("'" + d->title + "' needs a .main or .peri section before it");
        else if (domain == "main" && !(d->domains & kMain)) err("'" + d->title + "' is a Peri block; it cannot be in the Main section");
        else if (domain == "peri" && !(d->domains & kPeri)) err("'" + d->title + "' is a Main block; it cannot be in the Peri section");
    }
    detail::componentProblems(b, *d, reg, out);
    for (const FieldDef& f : d->fields) {
        const std::string v2 = partText(b, *d, f.name, reg);
        if (f.kind == FieldKind::Flag) continue;
        if (!partVisible(f.when, b)) { if (!v2.empty() && !f.optional && b.type != "loop") err("'" + f.label + "' is not used here", false); continue; }
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
        if (!c) { if (!in.optional && !b.field(in.name)) err("'" + (in.label.empty() ? in.name : in.label) + "' needs a block plugged in"); continue; }
        const BlockDef* cd = reg.find(c->type);
        if (cd && !cd->valueType.empty() && !in.accepts.empty() && std::find(in.accepts.begin(), in.accepts.end(), cd->valueType) == in.accepts.end()) {
            std::string acc;
            for (const std::string& a2 : in.accepts) acc += (acc.empty() ? "" : ", ") + a2;
            out.push_back({true, c->id, "'" + cd->title + "' (" + cd->valueType + ") does not fit into '" + in.label + "', which takes " + acc});
        }
        detail::plugProblems(b, *d, in, *c, reg, out);
        checkBlock(*c, domain, inProc, true, reg, out, ids);
    }
    for (const auto& kv : b.values) {
        bool known = false;
        for (const InputDef& in : d->inputs) if (in.name == kv.first) known = true;
        if (!known && !kv.second.empty()) err("'" + b.type + "' has no value socket '" + kv.first + "'");
    }
}
}  // namespace

namespace {
// The counting loops: the 6 scratchpad bytes must be a real place, and a loop inside a loop must not use the bytes of the loop around it.
void checkLoops(const std::vector<Block>& v, int depth, std::vector<int>& bases, std::vector<BlockProblem>& out) {
    for (const Block& b : v) {
        if (b.disabled) continue;
        const bool loop = b.type == "loop";
        const bool counting = loop && b.get("mode") != "forever";
        if (counting) {
            const int c = loopCounterBase(b, depth);
            if (c < 0) {
                out.push_back({true, b.id, trim(b.get("counter")).empty() ? "loops nested this deep (more than 5) need a counter place of their own" : "the counter must be a plain number 0..26 (it uses 6 scratchpad bytes from there)", "structure"});
            } else {
                for (int o : bases)
                    if (std::abs(o - c) < 6) { out.push_back({true, b.id, "this loop's counter (scratchpad bytes " + std::to_string(c) + ".." + std::to_string(c + 5) + ") overlaps the counter of the loop around it", "structure"}); break; }
                bases.push_back(c);
            }
        }
        // while / wait: the condition is worked out by the blocks in COND: when it reads the ALU flag, the last of them should be the ALU operation
        if ((b.type == "while" || b.type == "wait") && !b.disabled && !b.value("test")) {
            const std::string cond = upper(trim(b.get("cond")));
            if (cond == "ALU_CMP_TRUE" || cond == "ALU_OUT_OV16") {
                const std::vector<Block>* cs = b.stack("COND");
                const Block* last = nullptr;
                if (cs) for (const Block& x : *cs) if (!x.disabled && x.type != "comment" && x.type != "blank") last = &x;
                if (!last) out.push_back({false, b.id, "the condition reads the ALU flag, but nothing in COND works it out (W506)", "hint"});
                else if (last->type != "alu") out.push_back({false, last->id, "the condition reads the ALU flag, but the last block in COND is not an ALU operation (W506)", "hint"});
            }
        }
        for (const auto& st : b.stacks) checkLoops(st.second, loop ? depth + 1 : depth, bases, out);
        if (counting && !bases.empty()) bases.pop_back();
    }
}
}  // namespace

std::vector<BlockProblem> validate(const Document& doc, const Registry& reg, const SymbolTable* extra) {
    std::vector<BlockProblem> out;
    std::set<std::string> ids;
    for (const Section& s : doc.sections) {
        if (s.domain != "" && s.domain != "main" && s.domain != "peri" && s.domain != "floating") out.push_back({true, "", "unknown section '" + s.domain + "' (main, peri or floating)"});
        checkList(s.blocks, s.domain, false, reg, out, ids);
        std::vector<int> bases;
        checkLoops(s.blocks, 0, bases, out);
    }
    {                                                                            // what the generator cannot do (no room for a variable, a bad shift, a variable nothing sets)
        const Generated g = generate(doc, reg);
        out.insert(out.end(), g.problems.begin(), g.problems.end());
    }
    SymbolTable table;
    table.addDocument(doc);
    if (extra) table.addTable(*extra);
    detail::referenceProblems(doc, reg, table, out);
    return out;
}

// ---- the file ------------------------------------------------------------------------------------------------------------------------
namespace {

// The text stored for a field: the text of the component plugged into the socket of the same name (or the window the field derives from), so an
// older tool that knows nothing of components still reads the right text.
std::string mirrorText(const Block& b, const BlockDef* d, const std::string& name, const std::string& stored, const Registry& reg) {
    if (!d) return stored;
    if (b.value(name)) return partText(b, *d, name, reg);
    for (const InputDef& in : d->inputs) {
        if (in.derive.empty() || !b.value(in.name)) continue;
        if (in.derive.compare(0, 4, "arg:") == 0) { if (in.derive.substr(4) == name) return partText(b, *d, name, reg); continue; }
        const std::string spec = in.derive.substr(6);
        const size_t comma = spec.find(',');
        if (name == spec.substr(0, comma) || name == spec.substr(comma + 1)) return partText(b, *d, name, reg);
    }
    return stored;
}

json::Value toJson(const Block& b, const Registry& reg) {
    json::Value v = json::Value::makeObject();
    v.set("id", json::Value::makeString(b.id));
    v.set("type", json::Value::makeString(b.type));
    const BlockDef* def = reg.find(b.type);
    if (!b.fields.empty()) {
        json::Value f = json::Value::makeObject();
        for (const auto& kv : b.fields) f.set(kv.first, json::Value::makeString(mirrorText(b, def, kv.first, kv.second, reg)));
        v.set("fields", f);
    }
    if (!b.comment.empty()) v.set("comment", json::Value::makeString(b.comment));
    if (b.disabled) v.set("disabled", json::Value::makeBool(true));
    if (b.collapsed) v.set("collapsed", json::Value::makeBool(true));
    bool anyValue = false;
    for (const auto& vl : b.values) if (!vl.second.empty()) anyValue = true;
    if (anyValue) {
        // "parts": the components. (Older tools ignore this key and use the field texts; "values" is what stage M1 wrote and is still read.)
        json::Value vs = json::Value::makeObject();
        for (const auto& vl : b.values) if (!vl.second.empty()) vs.set(vl.first, toJson(vl.second[0], reg));
        v.set("parts", vs);
    }
    bool anyStack = false;
    for (const auto& st : b.stacks) if (!st.second.empty() || b.type == "proc" || b.type == "loop" || b.type == "if" || b.type == "while" || b.type == "wait") anyStack = true;
    if (anyStack) {
        json::Value s = json::Value::makeObject();
        for (const auto& st : b.stacks) {
            json::Value arr = json::Value::makeArray();
            for (const Block& c : st.second) arr.push(toJson(c, reg));
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
    for (const char* key : {"values", "parts"}) {
        const json::Value* vs = v.get(key);
        if (!vs) continue;
        if (vs->kind != json::Value::Object) { err = where + ": \"" + key + "\" must be an object"; return false; }
        for (const auto& kv : vs->obj) {
            Block c;
            if (!fromJson(kv.second, c, where + "." + key + "." + kv.first, err)) return false;
            b.setValue(kv.first, c);
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

std::string serializeDocument(const Document& doc, const Registry& reg) {
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
        for (const Block& b : s.blocks) arr.push(toJson(b, reg));
        sv.set("blocks", arr);
        secs.push(sv);
    }
    root.set("sections", secs);
    std::string out;
    json::write(root, out);
    return out + "\n";
}

bool parseDocument(const std::string& text, Document& out, std::string& error, const Registry& reg, const SymbolTable* extra) {
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
    reconcileDocument(out, reg, extra);                                                     // components agree with their text mirrors; text without components is split
    return true;
}

namespace detail {
json::Value blockToJson(const Block& b) { return asmcore::blocks::toJson(b, Registry::defaults()); }
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
