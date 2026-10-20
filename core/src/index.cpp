// Language service, part 2: symbol index and source map. See include/asmcore/index.h.
#include "asmcore/index.h"

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

#include "util.h"

namespace asmcore {

namespace {

std::string domScope(lang::Dom d) { return d == lang::Dom::Peri ? "peri" : "main"; }

}  // namespace

Index Index::build(const std::vector<Source>& sources, const lang::Language& language, const IncludeResolver& resolver) {
    Index ix;
    ix.lang_ = language;
    lang::Dom dom = lang::Dom::None;
    std::string proc;                                               // current procedure ("" outside)
    struct Use { std::string name; SrcLoc loc; std::string domScope, proc; };
    std::vector<Use> uses;
    std::map<std::string, int> byKey;                               // scope + "\n" + name -> symbol index
    auto define = [&](const std::string& name, const std::string& kind, const std::string& scope, const SrcLoc& at) {
        std::string key = scope + "\n" + name;
        if (byKey.count(key)) return;                                // a duplicate definition is the assembler's E104, not the index's
        IndexSymbol s;
        s.name = name; s.kind = kind; s.scope = scope; s.def = at; s.id = (int)ix.syms_.size();
        byKey[key] = s.id;
        ix.syms_.push_back(s);
    };
    std::set<std::string> active;                                  // files being read (an include cycle ends here)
    std::function<void(const Source&, int)> readSource = [&](const Source& src, int depth) {
        if (depth > 16 || !active.insert(src.name).second) return;
        std::istringstream in(src.text);
        std::string raw;
        int no = 0;
        while (std::getline(in, raw)) {
            no++;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            std::vector<lang::Token> toks = language.tokenizeLine(raw, dom, nullptr);
            const lang::Dom lineDom = dom;                           // the domain AFTER a .main / .peri on this line
            auto loc = [&](const lang::Token& t) { SrcLoc l; l.file = src.name; l.line = no; l.col = t.col; l.endCol = t.col + t.len; return l; };
            auto text = [&](const lang::Token& t) { return raw.substr((size_t)t.col, (size_t)t.len); };
            std::string firstWord;                                   // upper-case directive / mnemonic
            int symbolsSeen = 0;
            bool defInThisLine = false;
            for (size_t k = 0; k < toks.size(); k++) {
                const lang::Token& t = toks[k];
                if (t.kind == lang::Tok::Label) {
                    std::string scope = proc.empty() ? domScope(lineDom) : domScope(lineDom) + ":" + proc;
                    define(text(t), "label", scope, loc(t));
                    continue;
                }
                if (t.kind == lang::Tok::Directive || t.kind == lang::Tok::Mnemonic) {
                    if (firstWord.empty() || t.kind == lang::Tok::Directive) { if (firstWord.empty()) firstWord = upper(text(t)); }
                    continue;
                }
                if (t.kind != lang::Tok::Symbol && t.kind != lang::Tok::Unknown) continue;
                std::string name = text(t);
                // is this word the one a directive defines?
                std::string kind;
                if (firstWord.empty() && symbolsSeen == 0 && k + 1 < toks.size()) {
                    // "NAME EQU expr": the name precedes the directive
                    for (size_t m = k + 1; m < toks.size(); m++) {
                        if (toks[m].kind == lang::Tok::Whitespace) continue;
                        if (toks[m].kind == lang::Tok::Directive && upper(text(toks[m])) == "EQU") kind = "equ";
                        break;
                    }
                }
                if (kind.empty() && !defInThisLine) {
                    if (firstWord == ".EQU") kind = "equ";
                    else if (firstWord == ".CSR") kind = "csr";
                    else if (firstWord == ".SERVICE") kind = "service";
                    else if (firstWord == ".CONST") kind = "const";
                    else if (firstWord == ".PROC") kind = "proc";
                    else if (firstWord == ".WCET") kind = "wcet";
                }
                if (firstWord == ".DEVMAP" && !defInThisLine) {
                    // .devmap i, I2C, addr [, name]: the name is the last identifier after the third comma
                    int commas = 0;
                    for (size_t m = 0; m < k; m++) if (toks[m].kind == lang::Tok::Punct && raw[(size_t)toks[m].col] == ',') commas++;
                    if (commas >= 3) kind = "devmap";
                }
                symbolsSeen++;
                if (!kind.empty() && !defInThisLine) {
                    defInThisLine = true;
                    if (kind == "proc") { define(name, "proc", domScope(lineDom), loc(t)); proc = name; }
                    else define(name, kind, "", loc(t));
                    continue;
                }
                uses.push_back(Use{name, loc(t), domScope(lineDom), proc});
            }
            if (firstWord == ".ENDP") proc.clear();
            if (firstWord == ".INCLUDE" && resolver) {
                for (const lang::Token& t : toks) {
                    if (t.kind != lang::Tok::String || t.len < 2) continue;
                    Source inc;
                    if (resolver(src.name, raw.substr((size_t)t.col + 1, (size_t)t.len - 2), inc)) readSource(inc, depth + 1);
                    break;
                }
            }
        }
        active.erase(src.name);
    };
    for (const Source& src : sources) readSource(src, 0);
    // resolve uses: procedure-local label, then domain label / proc, then global
    for (const Use& u : uses) {
        std::vector<std::string> cands;
        if (!u.proc.empty()) cands.push_back(u.domScope + ":" + u.proc);
        cands.push_back(u.domScope);
        cands.push_back("");
        int found = -1;
        for (const std::string& sc : cands) {
            auto it = byKey.find(sc + "\n" + u.name);
            if (it != byKey.end()) { found = it->second; break; }
        }
        if (found >= 0) ix.syms_[(size_t)found].refs.push_back(u.loc);
        else ix.undefined_.push_back({u.name, u.loc});
    }
    return ix;
}

const IndexSymbol* Index::at(const std::string& file, int line, int col) const {
    auto covers = [&](const SrcLoc& l) { return l.file == file && l.line == line && col >= l.col && col <= l.endCol; };
    for (const IndexSymbol& s : syms_) {
        if (covers(s.def)) return &s;
        for (const SrcLoc& r : s.refs) if (covers(r)) return &s;
    }
    return nullptr;
}

const IndexSymbol* Index::find(const std::string& name, const std::string& scope) const {
    for (const IndexSymbol& s : syms_) if (s.name == name && s.scope == scope) return &s;
    return nullptr;
}

std::vector<SrcLoc> Index::occurrences(const IndexSymbol& s) const {
    std::vector<SrcLoc> v;
    v.push_back(s.def);
    for (const SrcLoc& r : s.refs) v.push_back(r);
    std::stable_sort(v.begin(), v.end(), [](const SrcLoc& a, const SrcLoc& b) {
        return a.file != b.file ? a.file < b.file : a.line != b.line ? a.line < b.line : a.col < b.col;
    });
    return v;
}

std::string Index::renameProblem(const IndexSymbol& s, const std::string& newName) const {
    if (!isIdent(newName)) return "'" + newName + "' is not a valid name";
    if (newName[0] == '.') return "a name cannot start with '.'";
    std::string up = upper(newName);
    for (lang::Tok k : {lang::Tok::Mnemonic, lang::Tok::Directive, lang::Tok::Region, lang::Tok::Condition, lang::Tok::SubOp})
        if (lang_.find(k, up)) return "'" + newName + "' is a keyword of the language";
    for (const IndexSymbol& o : syms_) {
        if (o.id == s.id || o.name != newName) continue;
        // same scope, or one hides the other (a global against a local, a label against another of its domain)
        if (o.scope == s.scope || o.scope.empty() || s.scope.empty() || s.scope.rfind(o.scope, 0) == 0 || o.scope.rfind(s.scope, 0) == 0)
            return "'" + newName + "' is already defined";
    }
    return "";
}

// ---- SourceMap -------------------------------------------------------------------------------------------------------------------
SourceMap::SourceMap(const Result& r) : r_(&r) {
    for (size_t i = 0; i < r.instrs.size(); i++) {
        const InstrInfo& in = r.instrs[i];
        byPc_[{in.domain == Domain::Main ? 0 : 1, in.pc}] = i;
        byLine_[{in.loc.file, in.loc.line}].push_back(i);          // synthetic ones too: they are what a softret CALL line turns into
    }
    // An inline CALL has no instruction of its own: the call line stands for the first instruction of the expansion.
    if (r.callModel == "inline")
        for (const CallSiteInfo& cs : r.callSites) {
            auto it = byPc_.find({cs.domain == Domain::Main ? 0 : 1, cs.transferPc});
            if (it == byPc_.end()) continue;
            auto& v = byLine_[{cs.loc.file, cs.loc.line}];
            if (std::find(v.begin(), v.end(), it->second) == v.end()) v.push_back(it->second);
        }
    for (auto& kv : byLine_)
        std::sort(kv.second.begin(), kv.second.end(), [&](size_t a, size_t b) {
            const InstrInfo &x = r.instrs[a], &y = r.instrs[b];
            return x.domain != y.domain ? x.domain < y.domain : x.pc < y.pc;
        });
}

std::vector<const InstrInfo*> SourceMap::instrsForLine(const std::string& file, int line) const {
    std::vector<const InstrInfo*> v;
    auto it = byLine_.find({file, line});
    if (it != byLine_.end()) for (size_t i : it->second) v.push_back(&r_->instrs[i]);
    return v;
}

const InstrInfo* SourceMap::instrAt(Domain d, uint32_t pc) const {
    auto it = byPc_.find({d == Domain::Main ? 0 : 1, pc});
    return it == byPc_.end() ? nullptr : &r_->instrs[it->second];
}

int SourceMap::nearestCodeLine(const std::string& file, int line) const {
    int best = 0;
    for (auto& kv : byLine_)
        if (kv.first.first == file && kv.first.second >= line && (best == 0 || kv.first.second < best)) best = kv.first.second;
    return best;
}

std::vector<int> SourceMap::codeLines(const std::string& file) const {
    std::vector<int> v;
    for (auto& kv : byLine_) if (kv.first.first == file) v.push_back(kv.first.second);
    return v;
}

}  // namespace asmcore
