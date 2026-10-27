#include "blocks/block_catalog.h"

#include <algorithm>
#include <cctype>
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

// What a fresh instruction says (plain assembler; the importer makes the blocks, with their components). A short placeholder where a real value is needed.
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
        {"cascade_begin", "CASCADE_BEGIN 0, block0\nblock0: JMP done\n CASCADE_END 0, done\ndone:"},                     // (a BEGIN alone would be a cascade that never ends)
        {"cascade_end", "CASCADE_END 0, done"},
        {"call", "CALL name"},
        {"ret", "RET"},
        {"xfer", "EXEC_XFER READ dev=0, addr=0, addrlen=1, ptr=0"},
        {"pub", "EXEC_PUB service=1, src=0, len=1"},
        {"proc", ".proc name\n.endp"},
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
const char* kInstructionDrawers[] = {"Data", "Math", "CSR", "Control", "Peripheral"};
const char* kComponentDrawers[] = {"Values", "Regions", "Options", "References"};

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
            if (!t && e.cls != "text" && d.stacks.empty()) continue;              // a block with no known sample text is not offered (a test notices); one with a slot (a loop) is made from its definition
        }
        if (d.type == "blank" || d.type == "raw") continue;                         // (a blank line and raw text are not something to pick from a toolbox)
        builtin_.push_back(e);
    }
    rebuild();
}

void BlockCatalog::rebuild() {
    entries_ = builtin_;
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
    drawer("Patterns", "pattern");
    for (const char* n : kInstructionDrawers) drawer(n, "instruction");
    drawer("Structure", "structure");
    drawer("Definitions", "definition");
    drawer("Text", "text");
    for (const char* n : kComponentDrawers) drawer(n, "component");
    // any drawer a new definition brings that is not listed above
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
// a component with its parts: numbers where numbers go
Block makeComponent(Document& doc, const Registry& reg, const std::string& type) {
    Block b = doc.make(type);
    const BlockDef* d = reg.find(type);
    if (!d) return b;
    for (const FieldDef& f : d->fields) {
        if (!b.get(f.name).empty()) continue;
        if (f.optional) continue;                                                       // (a number's kind / sign / width stay "not stated")
        if (f.kind == FieldKind::Choice) { for (const std::string& o : f.options) if (!o.empty()) { b.set(f.name, o); break; } }
        else if (f.name == "name") b.set("name", "name");
        else if (f.name == "value" && d->semantic == "number") b.set("value", "0");
    }
    for (const InputDef& in : d->inputs) {
        bool mirror = false;
        for (const FieldDef& f : d->fields) if (f.name == in.name) mirror = true;
        if (mirror) {
            if (b.get(in.name).empty() && (!in.optional || d->semantic == "region")) b.set(in.name, in.name == "last" ? "1" : "0");      // a range of two bytes: [0..1]
            continue;
        }
        if (in.derive.empty()) { Block n = doc.make("num"); n.set("value", "0"); b.setValue(in.name, n); }
    }
    explodeBlock(b, doc, reg);
    return b;
}
}  // namespace

std::vector<Block> BlockCatalog::instantiate(const CatalogEntry& e, Document& doc) const {
    if (e.custom && e.component()) {                                                // the text is an operand
        Block b;
        if (operandFromText(doc, reg_, e.text, b)) return {b};
        return {doc.make("num")};
    }
    if (e.component()) return {makeComponent(doc, reg_, e.type)};
    if (e.text.empty() && !e.type.empty()) {                                        // no assembler form (a loop): made from the definition, with its slots
        Block b = doc.make(e.type);
        if (const BlockDef* d = reg_.find(e.type)) for (const std::string& st : d->stacks) b.stack(st, true);
        return {b};
    }
    std::vector<Block> v = blocksFromText(doc, e.text, nullptr);
    if (v.empty()) v.push_back(doc.make(e.type.empty() ? "comment" : e.type));
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
