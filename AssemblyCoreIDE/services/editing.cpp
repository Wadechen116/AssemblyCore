#include "editing.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

using namespace asmcore;
using asmcore::lang::Dom;
using asmcore::lang::Tok;

namespace acide {

namespace {

std::string up(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
bool startsWithNoCase(const std::string& s, const std::string& prefix) { return up(s).compare(0, prefix.size(), up(prefix)) == 0; }
bool isWordChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '@' || c == '$' || c == '?'; }

struct Collector {
    std::vector<CompletionItem> items;
    std::set<std::string> seen;
    std::string prefix;
    void add(const std::string& label, const std::string& insert, const std::string& detail, const std::string& kind) {
        if (!startsWithNoCase(label, prefix) || !seen.insert(up(label)).second) return;
        items.push_back(CompletionItem{label, insert.empty() ? label : insert, detail, kind});
    }
    void addEntries(const std::vector<const lang::Entry*>& v, const std::string& group = "*") {
        for (const lang::Entry* e : v) {
            if (group != "*" && e->group != group) continue;
            add(e->name, e->insert.empty() ? e->name : e->insert, e->signature.empty() ? e->doc : e->signature, lang::tokName(e->kind));
        }
    }
};

bool domainMatches(const std::string& scope, Dom dom) {
    if (scope.empty() || dom == Dom::None) return true;
    std::string d = dom == Dom::Peri ? "peri" : "main";
    return scope.compare(0, d.size(), d) == 0;
}

void addSymbols(Collector& c, const Index* ix, Dom dom, std::initializer_list<const char*> kinds) {
    if (!ix) return;
    for (const IndexSymbol& s : ix->symbols()) {
        bool want = false;
        for (const char* k : kinds) if (s.kind == k) want = true;
        if (want && domainMatches(s.scope, dom)) c.add(s.name, "", s.kind, s.kind);
    }
}

}  // namespace

std::vector<CompletionItem> complete(const lang::Language& L, const Index* ix, const std::string& line, int col, Dom dom) {
    lang::Language::Cursor cur = L.cursorAt(line, col, dom);
    Collector c;
    c.prefix = cur.wordPrefix;
    if (cur.atStatementStart) {
        c.addEntries(L.all(Tok::Mnemonic, dom));
        c.addEntries(L.all(Tok::Directive, dom));
    } else {
        const std::string& m = cur.mnemonic;
        const int op = cur.operand;
        const bool peri = dom == Dom::Peri;
        if (m == "MOV") {
            c.addEntries(L.all(Tok::Region, dom));
            c.add("CONST(", "CONST(", "a constant placed in MAIN_CONST_POOL", "region");
        } else if (m == "ALU") {
            if (cur.word == 0) c.addEntries(L.all(Tok::SubOp, dom), "alu");
            else c.addEntries(L.all(Tok::SubOp, dom), "alumod");
        } else if (m == "EXEC_CSR") {
            if (op == 0 && cur.word == 0) c.addEntries(L.all(Tok::SubOp, dom), "csr");
            else if (op == 0) { addSymbols(c, ix, dom, {"csr", "equ"}); }
            else { c.addEntries(L.all(Tok::ArgName, dom)); addSymbols(c, ix, dom, {"equ"}); c.addEntries(L.all(Tok::Region, dom)); }
        } else if (m == "BR.COND") {
            if (op == 0) c.addEntries(L.all(Tok::Condition, dom));
            else if (op == 1) addSymbols(c, ix, dom, {"label"});
            else c.addEntries(L.all(Tok::SubOp, dom), "branchattr");          // , safe
        } else if (m == "BR.ERET") {
            // no operands
        } else if (m == "JMP" || m.compare(0, 3, "BR.") == 0 || m == "CASCADE_BEGIN" || m == "CASCADE_END") {
            const bool cascade = m.compare(0, 8, "CASCADE_") == 0;
            const int labelAt = cascade ? 1 : 0;
            if (op == labelAt) addSymbols(c, ix, dom, {"label"});
            else if (op > labelAt && !cascade) c.addEntries(L.all(Tok::SubOp, dom), "branchattr");       // , safe  (never after a cascade instruction)
            else addSymbols(c, ix, dom, {"equ"});
        } else if (m == "CALL") {
            addSymbols(c, ix, dom, {"proc"});
        } else if (m == "EXEC_PUB") {
            if (cur.afterEquals) addSymbols(c, ix, dom, {"service", "equ"});
            else c.addEntries(L.all(Tok::ArgName, dom));
        } else if (m == "EXEC_XFER") {
            if (cur.afterEquals) addSymbols(c, ix, dom, {"devmap", "equ"});
            else if (op == 0 && cur.word == 0) c.addEntries(L.all(Tok::SubOp, dom), "xfer");
            else c.addEntries(L.all(Tok::ArgName, dom));
        } else if (m == ".ASSUME") {
            c.addEntries(L.all(Tok::Region, dom));
            c.add("defined", "", "", "subop");
        } else if (m == ".PROC" || m == ".INCLUDE" || m == ".EQU" || m == ".CSR" || m == ".SERVICE" || m == ".CONST" || m == "RET") {
            // the operand is a new name or a file: nothing to offer
        } else if (m == ".WCET") {
            if (op >= 1) addSymbols(c, ix, dom, {"label"});
        } else {
            addSymbols(c, ix, dom, {"equ", "csr", "service", "const", "label", "devmap", "proc"});
        }
        (void)peri;
    }
    std::stable_sort(c.items.begin(), c.items.end(), [](const CompletionItem& a, const CompletionItem& b) { return up(a.label) < up(b.label); });
    return c.items;
}

std::string hoverText(const lang::Language& L, const Index* ix, const Result* result, const std::string& file, int lineNo, const std::string& line, int col, Dom dom) {
    if (col < 0 || (size_t)col > line.size()) return "";
    size_t b = (size_t)col, e = (size_t)col;
    while (b > 0 && isWordChar(line[b - 1])) b--;
    while (e < line.size() && isWordChar(line[e])) e++;
    if (e == b) return "";
    std::string word = line.substr(b, e - b);
    if (ix) {
        if (const IndexSymbol* s = ix->at(file, lineNo, col)) {
            std::ostringstream o;
            o << s->kind << " " << s->name;
            if (result) for (const SymbolInfo& si : result->symbols) if (si.name == s->name && si.kind == s->kind) { o << " = " << si.value << " (0x" << std::hex << si.value << ")"; break; }
            o << "\n" << std::dec << (s->refs.size()) << " use(s); defined at line " << s->def.line;
            return o.str();
        }
    }
    std::string key = up(word);
    for (Tok k : {Tok::Mnemonic, Tok::Directive, Tok::Region, Tok::Condition, Tok::SubOp, Tok::ArgName}) {
        const lang::Entry* en = L.find(k, key, Dom::None);
        if (!en) continue;
        std::string t = en->signature.empty() ? en->name : en->signature;
        if (!en->doc.empty()) t += "\n" + en->doc;
        if (!en->spec.empty()) t += "\n(spec " + en->spec + ")";
        return t;
    }
    (void)dom;
    return "";
}

Dom domainAtLine(const lang::Language& L, const std::string& text, int lineNo) {
    Dom d = Dom::None;
    std::istringstream in(text);
    std::string line;
    for (int n = 1; n < lineNo && std::getline(in, line); n++) L.tokenizeLine(line, d, nullptr);
    return d;
}

std::map<std::string, std::string> renameEdits(const Index& ix, const IndexSymbol& sym, const std::string& newName, const std::map<std::string, std::string>& texts, std::string& problem) {
    std::map<std::string, std::string> out;
    problem = ix.renameProblem(sym, newName);
    if (!problem.empty()) return out;
    std::vector<SrcLoc> occ = ix.occurrences(sym);
    // apply from the last occurrence to the first so earlier columns stay valid
    std::map<std::string, std::vector<SrcLoc>> byFile;
    for (const SrcLoc& l : occ) byFile[l.file].push_back(l);
    for (auto& kv : byFile) {
        auto t = texts.find(kv.first);
        if (t == texts.end()) { problem = "the text of " + kv.first + " is not available"; out.clear(); return out; }
        std::string text = t->second;
        std::vector<size_t> lineStart{0};
        for (size_t i = 0; i < text.size(); i++) if (text[i] == '\n') lineStart.push_back(i + 1);
        std::vector<SrcLoc> locs = kv.second;
        std::sort(locs.begin(), locs.end(), [](const SrcLoc& a, const SrcLoc& b) { return a.line != b.line ? a.line > b.line : a.col > b.col; });
        for (const SrcLoc& l : locs) {
            if (l.line < 1 || (size_t)l.line > lineStart.size() || l.endCol <= l.col) { problem = "stale position in " + kv.first; out.clear(); return out; }
            size_t at = lineStart[(size_t)l.line - 1] + (size_t)l.col;
            size_t len = (size_t)(l.endCol - l.col);
            if (at + len > text.size() || text.compare(at, len, sym.name) != 0) { problem = "the text changed since the last build (" + kv.first + ":" + std::to_string(l.line) + ")"; out.clear(); return out; }
            text.replace(at, len, newName);
        }
        out[kv.first] = text;
    }
    return out;
}

}  // namespace acide
