// The symbol table of a block program and the checks that use it: names that are not defined, of the wrong kind, out of reach; recursion.
#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "asmcore/blocks.h"
#include "blocks_internal.h"
#include "util.h"

namespace asmcore {
namespace blocks {

namespace {

bool identStart(char c) { return std::isalpha((unsigned char)c) || c == '_' || c == '.' || c == '$'; }
bool identChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$'; }
bool isIdentText(const std::string& t) {
    if (t.empty() || !identStart(t[0])) return false;
    for (char c : t) if (!identChar(c)) return false;
    return true;
}

// the first identifier of `s`, and what follows it
std::string firstWord(const std::string& s, std::string* rest = nullptr) {
    const std::string t = trim(s);
    size_t i = 0;
    while (i < t.size() && identChar(t[i])) i++;
    if (rest) *rest = trim(t.substr(i));
    return t.substr(0, i);
}

void addDirective(SymbolTable& tab, const std::string& dirIn, const std::string& args, const std::string& where) {
    const std::string dir = upper(trim(dirIn));
    std::string rest;
    if (dir == ".EQU") {
        const std::string name = firstWord(args, &rest);
        if (!isIdentText(name)) return;
        if (!rest.empty() && (rest[0] == '=' || rest[0] == ',')) rest = trim(rest.substr(1));
        tab.add({name, "int", rest, where, "", ""});
    } else if (dir == ".CSR") {
        const std::string name = firstWord(args, &rest);
        if (!isIdentText(name)) return;
        if (!rest.empty() && rest[0] == '=') rest = trim(rest.substr(1));
        tab.add({name, "csr", rest, where, "", ""});
    } else if (dir == ".SERVICE") {
        const std::string name = firstWord(args, &rest);
        if (!isIdentText(name)) return;
        if (!rest.empty() && rest[0] == '=') rest = trim(rest.substr(1));
        tab.add({name, "service", rest, where, "", ""});
    } else if (dir == ".CONST") {
        const std::string name = firstWord(args, &rest);
        if (isIdentText(name)) tab.add({name, "const", rest, where, "", ""});
    } else if (dir == ".DEVMAP") {
        const std::vector<std::string> parts = splitTop(args);
        if (parts.size() >= 4 && isIdentText(parts[3])) tab.add({parts[3], "dev", parts[0], where, "", ""});       // ".devmap i, I2C, addr7, name": the name stands for the index
    } else if (dir == ".INCLUDE") {
        tab.setOpenWorld(true);
    }
}

void walkBlocks(SymbolTable& tab, const std::vector<Block>& v, const std::string& domain, const std::string& proc) {
    for (const Block& b : v) {
        if (b.type == "label") {
            tab.add({b.get("name"), "label.flow", "", b.id, domain, proc});
        } else if (b.type == "proc") {
            tab.add({b.get("name"), "label.proc", "", b.id, domain, ""});
            if (const std::vector<Block>* body = b.stack("BODY")) walkBlocks(tab, *body, domain, b.get("name"));
            continue;
        } else if (b.type == "directive") {
            addDirective(tab, b.get("name"), b.get("args"), b.id);
        } else if (b.type == "raw") {
            std::string rest;
            const std::string x = firstWord(b.get("text"), &rest), y = firstWord(rest, &rest);       // "NAME EQU value" (8051 style)
            if (isIdentText(x) && upper(y) == "EQU") tab.add({x, "int", rest, b.id, "", ""});
        }
        for (const auto& st : b.stacks) walkBlocks(tab, st.second, domain, proc);
    }
}

bool numberKind(const std::string& kind) { return kind == "int" || kind == "csr" || kind == "service" || kind == "dev" || kind == "const"; }

}  // namespace

void SymbolTable::addDocument(const Document& doc) {
    if (!doc.hardware.empty()) openWorld_ = true;                         // the definitions may come from the hardware description
    for (const Section& s : doc.sections) walkBlocks(*this, s.blocks, s.domain, "");
}

void SymbolTable::addAsm(const std::string& text) {
    addDocument(importAsm(text));
}

void SymbolTable::addTable(const SymbolTable& other) {
    for (const Symbol& s : other.syms_) syms_.push_back(s);
    openWorld_ = openWorld_ || other.openWorld_;
}

const Symbol* SymbolTable::find(const std::string& name, const std::string& kind) const {
    for (const Symbol& s : syms_) {
        if (s.name != name) continue;
        if (kind.empty() || s.kind == kind || (kind == "int" && numberKind(s.kind))) return &s;
    }
    return nullptr;
}

std::vector<const Symbol*> SymbolTable::ofKind(const std::string& kind) const {
    std::vector<const Symbol*> v;
    for (const Symbol& s : syms_) if (kind.empty() || s.kind == kind || (kind == "int" && numberKind(s.kind))) v.push_back(&s);
    return v;
}

SymbolTable::Look SymbolTable::look(const std::string& name, const std::string& kind, const std::string& domain, const std::string& proc, const Symbol** found) const {
    bool other = false, hidden = false;
    for (const Symbol& s : syms_) {
        if (s.name != name || s.kind != kind) continue;
        if (!s.domain.empty() && !domain.empty() && domain != "floating" && s.domain != domain) { other = true; continue; }
        if (kind == "label.flow" && !s.proc.empty() && s.proc != proc) { hidden = true; if (found) *found = &s; continue; }
        if (found) *found = &s;
        return Look::Found;
    }
    return hidden ? Look::Hidden : other ? Look::OtherDomain : Look::Missing;
}

// ---- the checks that use the table ------------------------------------------------------------------------------------------------------
namespace detail {

namespace {

struct RefCheck {
    const Registry& reg;
    const SymbolTable& tab;
    std::vector<BlockProblem>& out;

    void problem(const Block& b, const std::string& msg, bool error) { out.push_back({error, b.id, msg, "reference"}); }

    std::string undefinedHint() const { return tab.openWorld() ? " (it may be defined in an included file or the hardware description)" : ""; }

    void checkName(const Block& host, const std::string& name, const std::string& kind, const std::string& domain, const std::string& proc) {
        if (name.empty()) return;
        if (kind == "label.flow" || kind == "label.proc") {
            const SymbolTable::Look l = tab.look(name, kind, domain, proc);
            const bool proc_ = kind == "label.proc";
            if (l == SymbolTable::Look::Found) return;
            if (l == SymbolTable::Look::Hidden) problem(host, "'" + name + "' is a label inside a procedure; a branch cannot enter a procedure (use CALL) (E305)", true);
            else if (l == SymbolTable::Look::OtherDomain) problem(host, std::string("'") + name + "' belongs to the other domain; Main and Peri cannot " + (proc_ ? "call" : "branch to") + " each other", true);
            else problem(host, std::string(proc_ ? "the procedure '" : "the label '") + name + "' is not defined" + undefinedHint(), false);
            return;
        }
        const Symbol* s = tab.find(name, kind);
        if (s) return;
        if (const Symbol* any = tab.find(name)) {
            problem(host, "'" + name + "' is " + (any->kind == "label.flow" || any->kind == "label.proc" ? "a code position" : "a " + any->kind) + ", not " + (kind == "int" ? "a number" : "a " + kind), true);
            return;
        }
        problem(host, "'" + name + "' is not defined" + undefinedHint(), false);
    }

    void component(const Block& v, const std::string& domain, const std::string& proc) {
        const BlockDef* d = reg.find(v.type);
        if (!d) return;
        if (d->semantic == "name" || d->semantic == "ref") {
            const std::string kind = d->semantic == "name" ? "int" : d->valueType;
            checkName(v, v.get("name"), kind, domain, proc);
        }
        for (const auto& vl : v.values) for (const Block& c : vl.second) component(c, domain, proc);
    }

    // text typed into a socket that takes a name (a label in a jump, a CSR name in a CSR operation): the same checks as for a component, but lenient about
    // the kind of a number (a .equ may stand where a CSR is wanted: the assembler reads both as numbers)
    void sockets(const Block& b, const BlockDef& d, const std::string& domain, const std::string& proc) {
        for (const InputDef& in : d.inputs) {
            if (b.value(in.name)) continue;
            const std::string text = trim(b.get(in.name));
            if (text.empty() || !isIdentText(text)) continue;
            std::string codeKind;
            bool numeric = false;
            for (const std::string& a : in.accepts) {
                if (a == "label.flow" || a == "label.proc") codeKind = a;
                if (a == "csr" || a == "service" || a == "dev" || a == "int") numeric = true;
            }
            if (!codeKind.empty()) checkName(b, text, codeKind, domain, proc);
            else if (numeric) checkName(b, text, "int", domain, proc);
        }
    }

    void walk(const std::vector<Block>& v, const std::string& domain, const std::string& proc) {
        for (const Block& b : v) {
            const BlockDef* d = reg.find(b.type);
            if (d) {
                sockets(b, *d, domain, proc);
                for (const auto& vl : b.values) for (const Block& c : vl.second) component(c, domain, proc);
            }
            const std::string inner = b.type == "proc" ? b.get("name") : proc;
            for (const auto& st : b.stacks) walk(st.second, domain, inner);
        }
    }
};

// recursion: the call graph must have no cycles (E305)
struct CallGraph {
    struct Edge { std::string to; std::string callId; };
    std::map<std::string, std::vector<Edge>> calls;           // by procedure (per domain: the names are keyed "domain:name")
    std::map<std::string, std::string> names;
};

void collectCalls(const std::vector<Block>& v, const std::string& domain, const std::string& proc, const Registry& reg, std::map<std::string, std::vector<std::pair<std::string, std::string>>>& calls) {
    for (const Block& b : v) {
        if (b.type == "call" && !proc.empty()) {
            std::string target = b.value("target") ? b.value("target")->get("name") : trim(b.get("target"));
            if (!target.empty()) calls[domain + ":" + proc].push_back({domain + ":" + target, b.id});
        }
        const std::string inner = b.type == "proc" ? b.get("name") : proc;
        for (const auto& st : b.stacks) collectCalls(st.second, domain, inner, reg, calls);
    }
}

}  // namespace

void referenceProblems(const Document& doc, const Registry& reg, const SymbolTable& tab, std::vector<BlockProblem>& out) {
    RefCheck rc{reg, tab, out};
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> calls;
    for (const Section& s : doc.sections) {
        if (s.domain == "floating") continue;
        rc.walk(s.blocks, s.domain, "");
        collectCalls(s.blocks, s.domain, "", reg, calls);
    }
    // depth-first search for a call that closes a cycle
    std::map<std::string, int> state;                          // 0 new, 1 on the stack, 2 done
    std::set<std::string> reported;
    std::function<void(const std::string&, std::vector<std::string>&)> dfs = [&](const std::string& p, std::vector<std::string>& stack) {
        state[p] = 1;
        stack.push_back(p);
        for (const auto& e : calls[p]) {
            if (state[e.first] == 1) {
                if (reported.insert(e.second).second) {
                    const std::string a = p.substr(p.find(':') + 1), b = e.first.substr(e.first.find(':') + 1);
                    std::string msg = a == b ? "'" + a + "' calls itself; recursion is not allowed (the call graph must have no cycles) (E305)" : "'" + a + "' calls '" + b + "', which leads back to '" + a + "'; recursion is not allowed (E305)";
                    out.push_back({true, e.second, msg, "reference"});
                }
            } else if (state[e.first] == 0) dfs(e.first, stack);
        }
        stack.pop_back();
        state[p] = 2;
    };
    for (auto& kv : calls) if (state[kv.first] == 0) { std::vector<std::string> st; dfs(kv.first, st); }
}

}  // namespace detail

}  // namespace blocks
}  // namespace asmcore
