#include "blocks/block_catalog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

#include "asmcore/blocks_edit.h"
#include "asmcore/lang.h"

namespace acide {
namespace blocks {

using namespace asmcore::blocks;

namespace {

// An operand typed as text (a number, an expression, a name, a range such as MAIN_SCRATCHPAD[4..7]): a number / expression first, then a region.
bool operandFromText(Document& doc, const Registry& reg, const std::string& text, Block& out) {
    InputDef num("v", "", {kTypeInt}, true);
    if (componentFromText(doc, reg, num, text, out)) return true;
    InputDef region("v", "", {}, true);
    for (const BlockDef& d : reg.all()) if (d.semantic == "region") region.accepts.push_back(d.valueType);
    return componentFromText(doc, reg, region, text, out);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string firstWord(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    size_t j = i;
    while (j < s.size() && !std::isspace((unsigned char)s[j]) && s[j] != ',') j++;
    return s.substr(i, j - i);
}

// What a fresh instruction says (plain assembler; the importer makes the block). Only the block itself is offered: instantiate() takes the components the text
// brings out again, so that every sample of the toolbox is ONE piece with empty sockets (black holes) where the data goes; its choices (an operation, a
// condition) stay, with the default the sample text says.
const char* sampleText(const std::string& type) {
    static const std::map<std::string, const char*> m = {
        {"mov", "MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]"},
        {"alu", "ALU ADD unsigned"},
        {"csr_write8", "EXEC_CSR WRITE8 0, 0"},
        {"csr_set8", "EXEC_CSR SET8 0, 1"},
        {"csr_clr8", "EXEC_CSR CLR8 0, 1"},
        {"csr_read8", "EXEC_CSR READ8 0 -> MAIN_CSR_DATA_BUF[0]"},
        {"csr_write_sp", "EXEC_CSR WRITE_SP 0, len=1, ptr=0"},
        {"csr_read_buf", "EXEC_CSR READ_BUF 0, len=1, ptr=0"},
        {"br_cond", "BR.COND ALWAYS, target"},
        {"jmp", "JMP target"},
        {"br_eret", "BR.ERET"},
        {"br_peri", "BR.ALWAYS target"},
        {"cascade_begin", "CASCADE_BEGIN 0, block0"},                                // one piece like every other: its CASCADE_END is a block of its own
        {"cascade_end", "CASCADE_END 0, done"},
        {"call", "CALL name"},
        {"ret", "RET"},
        {"xfer", "EXEC_XFER READ dev=0, addr=0, addrlen=1, ptr=0"},
        {"pub", "EXEC_PUB service=1, src=0, len=1"},
        {"proc", ".proc procedure\n.endp"},                                         // (the name the "procedure" reference of the Options drawer says)
        {"label", "label:"},
        {"vector", ".vector 0, handler"},
        {"wcet", ".wcet name, from, to"},
        {"assume", ".assume MAIN_SCRATCHPAD[0..0] defined"},
        {"directive", ".equ NAME = 0"},
        {"comment", "; comment"},
        {"raw", ""},
    };
    auto it = m.find(type);
    return it == m.end() ? nullptr : it->second;
}

struct Pattern {
    const char* id;
    const char* title;
    const char* description;
    int domains;
    const char* text;
};
const Pattern kPatterns[] = {
    {"load_const", "Load a constant into the ALU", "Put a number in the scratchpad, then in ALU A", kMain,
     "MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]\nMOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA"},
    {"add_two", "Add two numbers", "Load both numbers, add them, keep the result", kMain,
     "MOV CONST(1, 2) -> MAIN_SCRATCHPAD[0..1]\nMOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA\nMOV CONST(2, 2) -> MAIN_SCRATCHPAD[2..3]\nMOV MAIN_SCRATCHPAD[2..3] -> ALU_OPB\nALU ADD unsigned\nMOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[4..5]"},
    {"write_csr_const", "Write a CSR from a constant", "Put a byte in the scratchpad, write it to a CSR", kMain,
     "MOV CONST(0, 1) -> MAIN_SCRATCHPAD[0..0]\nEXEC_CSR WRITE_SP 0, len=1, ptr=0"},
    {"wait_for", "Wait until the ALU says yes", "Compare, branch out when it holds, otherwise try again", kMain,
     "wait:\n ALU CMP.EQ unsigned\n BR.COND ALU_CMP_TRUE, done\n JMP wait\ndone:"},
    {"read_publish", "Read a device and publish", "Read one byte over I2C, publish it for Main", kPeri,
     "EXEC_XFER READ dev=0, addr=0, addrlen=1, ptr=0\nEXEC_PUB service=1, src=0, len=1"},
};

// the order of the drawers
const char* kInstructionDrawers[] = {"Data", "Variables", "Math", "CSR", "Control", "Peripheral"};
const char* kComponentDrawers[] = {"Values", "Expressions", "Regions", "Options", "References"};
const char* kHardwareDrawers[] = {"Hardware CSR", "Hardware Devices", "Hardware Services", "Hardware Data"};      // (and "Hardware Names": components)

}  // namespace

std::string encodeCustom(const CustomBlock& c) {
    auto esc = [](const std::string& s) {
        std::string o;
        for (char ch : s) {
            if (ch == '\\') o += "\\\\";
            else if (ch == '\t') o += "\\t";
            else if (ch == '\n') o += "\\n";
            else if (ch == '\r') continue;
            else o += ch;
        }
        return o;
    };
    return esc(c.drawer) + "\t" + esc(c.title) + "\t" + esc(c.text);
}

bool decodeCustom(const std::string& line, CustomBlock& out) {
    std::vector<std::string> parts(1);
    for (size_t i = 0; i < line.size(); i++) {
        char ch = line[i];
        if (ch == '\t') { parts.emplace_back(); continue; }
        if (ch == '\\' && i + 1 < line.size()) {
            const char n = line[++i];
            parts.back() += n == 'n' ? '\n' : n == 't' ? '\t' : n;
            continue;
        }
        parts.back() += ch;
    }
    if (parts.size() != 3 || parts[0].empty() || parts[1].empty()) return false;
    out = CustomBlock{parts[0], parts[1], parts[2]};
    return true;
}

BlockCatalog::BlockCatalog(const Registry& reg) : reg_(reg) {
    for (const Pattern& p : kPatterns) {
        CatalogEntry e;
        e.id = std::string("pattern:") + p.id;
        e.title = p.title;
        e.category = "Patterns";
        e.cls = "pattern";
        e.domains = p.domains;
        e.text = p.text;
        e.description = p.description;
        builtin_.push_back(e);
    }
    for (const BlockDef& d : reg.all()) {
        CatalogEntry e;
        e.id = "type:" + d.type;
        e.title = d.title.empty() ? d.type : d.title;
        e.type = d.type;
        e.category = d.category;
        e.cls = d.cls.empty() ? "instruction" : d.cls;
        e.domains = d.domains;
        e.description = d.help;
        if (e.cls != "component") {
            const char* t = sampleText(d.type);
            e.text = t ? t : "";
            if (!t && e.cls != "text" && d.stacks.empty() && !d.structural) continue;       // a block with no known sample text is not offered (a test notices); one with a slot or its own generator (a loop, set) is made from its definition
        }
        if (d.type == "blank" || d.type == "raw") continue;                         // (a blank line and raw text are not something to pick from a toolbox)
        builtin_.push_back(e);
    }
    rebuild();
}

void BlockCatalog::rebuild() {
    entries_ = builtin_;
    entries_.insert(entries_.end(), hardware_.begin(), hardware_.end());
    for (const CustomBlock& c : custom_) {
        CatalogEntry e;
        e.id = customId(c);
        e.title = c.title;
        e.category = c.drawer;
        e.cls = drawerClass(c.drawer);
        e.text = c.text;
        e.custom = true;
        e.description = "";
        Document tmp;
        if (e.cls != "component") {                                              // Main-only / Peri-only follows the first block
            std::vector<Block> v = blocksFromText(tmp, c.text, nullptr);
            if (!v.empty()) if (const BlockDef* d = reg_.find(v[0].type)) e.domains = d->domains;
        }
        entries_.push_back(e);
    }
}

bool BlockCatalog::setHardware(const asmcore::Hardware& hw) {
    std::vector<CatalogEntry> v;
    auto upper = [](std::string s) { for (char& ch : s) ch = (char)std::toupper((unsigned char)ch); return s; };
    auto add = [&](const std::string& category, const std::string& title, const std::string& text, const std::string& what, bool operand = false) {
        CatalogEntry e;
        e.id = "hw:" + category + "/" + title;
        e.title = title;
        e.category = category;
        e.cls = operand ? "component" : "instruction";
        e.text = text;
        e.description = what;
        e.hardware = true;
        e.operand = operand;
        if (!operand) {                                                              // Main-only / Peri-only follows the first block
            Document tmp;
            std::vector<Block> blocks = blocksFromText(tmp, text, nullptr);
            if (!blocks.empty()) if (const BlockDef* d = reg_.find(blocks[0].type)) e.domains = d->domains;
        }
        v.push_back(e);
    };
    char hex[16];
    for (const asmcore::HwCsr& c : hw.csrs) {
        if (c.name.empty()) continue;
        std::snprintf(hex, sizeof hex, "0x%03X", c.addr < 0 ? 0 : c.addr);
        const std::string what = (c.description.empty() ? std::string() : c.description + " - ") + "CSR " + hex + " (" + c.kind + ")";
        const bool writable = c.kind == "ram" || c.kind == "value" || c.kind == "action", readable = c.kind != "action";
        if (writable) {
            add("Hardware CSR", "write " + c.name, "EXEC_CSR WRITE8 " + c.name + ", 0", what);
            add("Hardware CSR", "set bits " + c.name, "EXEC_CSR SET8 " + c.name + ", 1", what);
            add("Hardware CSR", "clear bits " + c.name, "EXEC_CSR CLR8 " + c.name + ", 1", what);
        }
        if (readable) add("Hardware CSR", "read " + c.name, "EXEC_CSR READ8 " + c.name + " -> MAIN_CSR_DATA_BUF[0]", what);
    }
    for (const asmcore::HwDevice& d : hw.devices) {
        if (d.index < 0) continue;                                                   // not in the DeviceMap: EXEC_XFER cannot name it
        const std::string sym = d.symbol.empty() ? "DEV_" + upper(d.name) : d.symbol;
        std::snprintf(hex, sizeof hex, "0x%02X", d.addr);
        const std::string what = (d.description.empty() ? std::string() : d.description + " - ") + "I2C device at " + hex;
        add("Hardware Devices", "read " + sym, "EXEC_XFER READ dev=" + sym + ", addr=0, addrlen=1, ptr=0", what);
        add("Hardware Devices", "read " + sym + " (several bytes)", "EXEC_XFER READ dev=" + sym + ", len=2, ptr=0, ext=[0]", what);
        add("Hardware Devices", "write " + sym, "EXEC_XFER WRITE dev=" + sym + ", addr=0, addrlen=1, data=0", what);
    }
    for (const asmcore::HwService& s : hw.services) {
        if (s.name.empty()) continue;
        const int len = s.len >= 1 && s.len <= 32 ? s.len : 1;
        add("Hardware Services", "publish " + s.name, "EXEC_PUB service=" + s.name + ", src=0, len=" + std::to_string(len), (s.description.empty() ? std::string() : s.description + " - ") + "service " + std::to_string(s.id));
    }
    for (const asmcore::HwConst& k : hw.consts) {
        if (k.name.empty()) continue;
        const int bytes = std::max(1, std::min(32, k.width * (int)std::max<size_t>(1, k.values.size())));
        add("Hardware Data", "load " + k.name, "MOV MAIN_CONST_POOL[@" + k.name + "] -> MAIN_SCRATCHPAD[0.." + std::to_string(bytes - 1) + "]", (k.description.empty() ? std::string() : k.description + " - ") + "constant of " + std::to_string(bytes) + " byte(s)");
    }
    for (const asmcore::HwEqu& q : hw.equs) {
        if (q.name.empty()) continue;
        add("Hardware Names", q.name, q.name, (q.description.empty() ? std::string() : q.description + " - ") + "= " + q.value, true);
    }
    auto same = [](const std::vector<CatalogEntry>& a, const std::vector<CatalogEntry>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) if (a[i].id != b[i].id || a[i].text != b[i].text || a[i].description != b[i].description) return false;
        return true;
    };
    if (same(v, hardware_)) return false;
    hardware_ = std::move(v);
    rebuild();
    return true;
}

void BlockCatalog::setCustom(std::vector<CustomBlock> c) {
    custom_ = std::move(c);
    rebuild();
}

std::string BlockCatalog::drawerClass(const std::string& drawer) const {
    for (const CatalogEntry& e : builtin_) if (e.category == drawer) return e.cls;
    return "instruction";
}

BlockCatalog::Check BlockCatalog::checkCustom(const CustomBlock& c, const std::string& replacing) const {
    Check r;
    auto trimmed = [](std::string s) {
        size_t a = 0;
        while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
        size_t b = s.size();
        while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
        return s.substr(a, b - a);
    };
    const std::string title = trimmed(c.title), text = trimmed(c.text);
    if (title.empty()) { r.message = "Give the block a name"; return r; }
    if (text.empty()) { r.message = "Write the text the block is made from"; return r; }
    for (const CatalogEntry& e : entries_)
        if (e.category == c.drawer && e.title == title && e.id != replacing) { r.message = "There is already a block with this name in the drawer"; return r; }
    Document tmp;
    if (drawerClass(c.drawer) == "component") {
        Block b;
        if (!operandFromText(tmp, reg_, text, b)) { r.message = "The text is not an operand (a number, an expression, or a range such as MAIN_SCRATCHPAD[4..7])"; return r; }
        r.blocks = 1;
        r.ok = true;
        return r;
    }
    bool raw = false;
    const std::vector<Block> v = blocksFromText(tmp, text, &raw);
    if (v.empty()) { r.message = "Nothing in the text makes a block"; return r; }
    r.blocks = (int)v.size();
    std::function<void(const std::vector<Block>&)> count = [&](const std::vector<Block>& l) { for (const Block& b : l) { if (b.type == "raw") r.raw++; for (auto& st : b.stacks) count(st.second); } };
    count(v);
    r.ok = true;
    if (r.raw) r.message = std::to_string(r.raw) + " line(s) are not understood as instructions and stay as assembler text";
    return r;
}

const CatalogEntry* BlockCatalog::find(const std::string& id) const {
    for (const CatalogEntry& e : entries_) if (e.id == id) return &e;
    return nullptr;
}

std::vector<BlockCatalog::Drawer> BlockCatalog::drawers() const {
    std::vector<Drawer> out;
    auto drawer = [&](const std::string& name, const std::string& cls) {
        Drawer d;
        d.name = name;
        d.cls = cls;
        for (const CatalogEntry& e : entries_) if (e.category == name && e.cls == cls) d.entries.push_back(&e);
        if (!d.entries.empty()) out.push_back(d);
    };
    // the single pieces first; the ready-made combinations (patterns, and the blocks of the hardware description, which come assembled) last
    for (const char* n : kInstructionDrawers) drawer(n, "instruction");
    drawer("Structure", "structure");
    drawer("Definitions", "definition");
    drawer("Text", "text");
    for (const char* n : kComponentDrawers) drawer(n, "component");
    drawer("Patterns", "pattern");
    for (const char* n : kHardwareDrawers) drawer(n, "instruction");                    // what the project's hardware description brings: right after the patterns
    drawer("Hardware Names", "component");
    // any drawer a new definition brings that is not listed above (after the combinations: a new definition is rare, and its drawer is easy to find at the end)
    std::set<std::string> seen;
    for (const Drawer& d : out) seen.insert(d.cls + ":" + d.name);
    for (const CatalogEntry& e : entries_)
        if (!seen.count(e.cls + ":" + e.category)) { drawer(e.category, e.cls); seen.insert(e.cls + ":" + e.category); }
    return out;
}

std::vector<const CatalogEntry*> BlockCatalog::search(const std::string& query0, size_t limit) const {
    std::string q = lower(query0);
    while (!q.empty() && std::isspace((unsigned char)q.back())) q.pop_back();
    size_t s = 0;
    while (s < q.size() && std::isspace((unsigned char)q[s])) s++;
    q = q.substr(s);
    std::vector<std::pair<int, const CatalogEntry*>> scored;
    if (q.empty()) return {};
    for (const CatalogEntry& e : entries_) {
        const std::string title = lower(e.title), type = lower(e.type), mnemonic = e.pattern() ? std::string() : lower(firstWord(e.text)), cat = lower(e.category);
        int score = 0;
        auto words = [&](const std::string& t) {
            std::vector<std::string> w;
            std::string cur;
            for (char c : t) { if (std::isalnum((unsigned char)c) || c == '.' || c == '_') cur += c; else if (!cur.empty()) { w.push_back(cur); cur.clear(); } }
            if (!cur.empty()) w.push_back(cur);
            return w;
        };
        if (title == q || type == q || mnemonic == q) score = 100;
        else if (title.compare(0, q.size(), q) == 0 || type.compare(0, q.size(), q) == 0 || mnemonic.compare(0, q.size(), q) == 0) score = 80;
        else {
            for (const std::string& w : words(title + " " + e.description)) if (w.compare(0, q.size(), q) == 0) score = std::max(score, 60);
            if (score == 0 && (title.find(q) != std::string::npos || type.find(q) != std::string::npos || mnemonic.find(q) != std::string::npos)) score = 40;
            if (score == 0 && cat.compare(0, q.size(), q) == 0) score = 20;
            if (score == 0 && q.size() >= 2) {                                          // the letters in order ("mv" finds move)
                size_t pos = 0;
                for (char c : q) { pos = title.find(c, pos); if (pos == std::string::npos) break; pos++; }
                if (pos != std::string::npos) score = 10;
            }
        }
        if (score > 0) scored.push_back({score, &e});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second->title < b.second->title; });
    std::vector<const CatalogEntry*> out;
    for (auto& p : scored) { if (out.size() >= limit) break; out.push_back(p.second); }
    return out;
}

namespace {
// Take every component out of the sockets of a block: what Editor::unplugValue leaves behind -- a hole, its text mirror emptied, the fields derived from it
// following (a window's first / last emptied, a composite argument written again from what is left).
// A CHOICE stays plugged in (the operation of a calculation, signed / unsigned, the condition of a branch): it has a sensible default, and an instruction
// that lacks it is not an instruction at all -- "ALU" with no operation is only a comment in the generated program and never runs. What is DATA goes: the
// numbers, the regions, the names.
// (told by the socket: one that takes a component of the Options drawer which is a choice -- the sample text may not have been turned into components yet)
bool takesChoice(const InputDef& in, const Registry& reg) {
    for (const BlockDef& c : reg.all()) {
        if (c.category != "Options" || c.valueType.empty()) continue;
        bool choice = false;
        for (const FieldDef& f : c.fields) choice = choice || f.kind == FieldKind::Choice;
        if (!choice) continue;
        for (const std::string& a : in.accepts) if (a == c.valueType) return true;
    }
    return false;
}
void hollow(Block& b, const Registry& reg) {
    const BlockDef* d = reg.find(b.type);
    if (!d) return;
    for (const InputDef& in : d->inputs) {
        if (takesChoice(in, reg) && (b.value(in.name) || !b.get(in.name).empty())) continue;
        b.clearValue(in.name);
        if (b.field(in.name)) b.set(in.name, std::string());
        if (in.derive.compare(0, 6, "range:") == 0) {
            const std::string spec = in.derive.substr(6);
            const size_t comma = spec.find(',');
            for (const std::string& f : {spec.substr(0, comma), spec.substr(comma + 1)}) b.set(f, std::string());
        }
    }
    for (const InputDef& in : d->inputs)
        if (in.derive.compare(0, 4, "arg:") == 0) b.set(in.derive.substr(4), partText(b, *d, in.derive.substr(4), reg));
}

// What a fresh component that stands for a name says. Each kind has a word of its own: they are drawn as the name alone, so with one common placeholder
// ("name") a label, a procedure, a CSR, a service and a device were five blocks that looked alike in the toolbox.
const char* placeholderName(const std::string& type) {
    static const std::map<std::string, const char*> m = {
        {"name", "NAME"}, {"label_ref", "label"}, {"proc_ref", "procedure"}, {"csr_ref", "CSR_NAME"}, {"service_ref", "SERVICE_NAME"}, {"dev_ref", "DEVICE_NAME"},
    };
    auto it = m.find(type);
    return it == m.end() ? "name" : it->second;
}

// a component on its own: its own fields (a choice, a name, the value of a number), every socket a hole
Block makeComponent(Document& doc, const Registry& reg, const std::string& type) {
    Block b = doc.make(type);
    const BlockDef* d = reg.find(type);
    if (!d) return b;
    for (const FieldDef& f : d->fields) {
        if (!b.get(f.name).empty()) continue;
        if (f.optional) continue;                                                       // (a number's kind / sign / width stay "not stated")
        if (f.kind == FieldKind::Choice) { for (const std::string& o : f.options) if (!o.empty()) { b.set(f.name, o); break; } }
        else if (f.name == "name") b.set("name", placeholderName(type));
        else if (f.name == "value" && d->semantic == "number") b.set("value", "0");
    }
    hollow(b, reg);
    if (d->semantic == "number") b.set("value", "0");                                  // (a number is its value: 0, not a hole)
    return b;
}
}  // namespace

std::vector<Block> BlockCatalog::instantiate(const CatalogEntry& e, Document& doc) const {
    if ((e.custom || e.operand) && e.component()) {                                 // the text is an operand
        Block b;
        if (operandFromText(doc, reg_, e.text, b)) return {b};
        return {doc.make("num")};
    }
    if (e.component()) return {makeComponent(doc, reg_, e.type)};
    if (e.text.empty() && !e.type.empty()) {                                        // no assembler form (a loop): made from the definition, with its (empty) slots and holes
        Block b = doc.make(e.type);
        if (const BlockDef* d = reg_.find(e.type)) for (const std::string& st : d->stacks) b.stack(st, true);
        hollow(b, reg_);
        return {b};
    }
    std::vector<Block> v = blocksFromText(doc, e.text, nullptr);
    if (v.empty()) v.push_back(doc.make(e.type.empty() ? "comment" : e.type));
    // A block of the toolbox is one piece: the components its sample text brought are taken out again. The ready-made combinations keep everything: a pattern,
    // the user's own blocks, and the blocks of the hardware description (whose point is the name of the part in them).
    if (!e.pattern() && !e.custom && !e.hardware)
        for (Block& b : v) hollow(b, reg_);
    return v;
}

std::vector<Block> BlockCatalog::fromAssembler(Document& doc, const std::string& text, bool* anyRaw) const {
    std::vector<Block> v = blocksFromText(doc, text, anyRaw);
    return v;
}

bool BlockCatalog::looksLikeAssembler(const std::string& text) const {
    const std::string w = firstWord(text);
    if (w.empty()) return false;
    {                                                                                // a single word is a name to look up ("ret", "mov"), unless it is a label / directive / comment
        size_t i = 0;
        while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
        const std::string rest = text.substr(i + w.size());
        bool more = false;
        for (char c : rest) if (!std::isspace((unsigned char)c)) more = true;
        if (!more && w.back() != ':' && w[0] != '.' && w[0] != ';') return false;
    }
    if (w.back() == ':' || w[0] == '.' || w[0] == ';') return true;                 // a label, a directive, a comment
    const asmcore::lang::Language L = asmcore::lang::Language::defaults();
    const std::string up = [&] { std::string u = w; for (char& c : u) c = (char)std::toupper((unsigned char)c); return u; }();
    if (L.find(asmcore::lang::Tok::Mnemonic, up)) return true;
    return false;
}

}  // namespace blocks
}  // namespace acide
