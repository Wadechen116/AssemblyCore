#include "auto_fix.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <set>
#include <sstream>

using namespace asmcore;

namespace acide {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// Optimal string alignment distance: insert, delete, change, swap of two neighbours.
int distance(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> d(n + 1, std::vector<int>(m + 1));
    for (size_t i = 0; i <= n; i++) d[i][0] = (int)i;
    for (size_t j = 0; j <= m; j++) d[0][j] = (int)j;
    for (size_t i = 1; i <= n; i++)
        for (size_t j = 1; j <= m; j++) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
        }
    return d[n][m];
}

bool isWordChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.'; }

// The first place `word` stands as a whole word in `line` (npos if none); case-sensitive first, then not.
size_t findWord(const std::string& line, const std::string& word, size_t from = 0) {
    if (word.empty()) return std::string::npos;
    for (int pass = 0; pass < 2; pass++) {
        const std::string hay = pass == 0 ? line : upper(line), needle = pass == 0 ? word : upper(word);
        for (size_t p = hay.find(needle, from); p != std::string::npos; p = hay.find(needle, p + 1)) {
            const bool left = p == 0 || !isWordChar(hay[p - 1]);
            const bool right = p + needle.size() >= hay.size() || !isWordChar(hay[p + needle.size()]);
            if (left && right) return p;
        }
    }
    return std::string::npos;
}

std::string stripComment(const std::string& line) {
    const size_t c = line.find(';');
    return c == std::string::npos ? line : line.substr(0, c);
}

bool parseNumber(const std::string& t, long long& out) {
    if (t.empty()) return false;
    char* end = nullptr;
    const long long v = std::strtoll(t.c_str(), &end, t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X') ? 16 : 10);
    if (end == t.c_str() || *end) return false;
    out = v;
    return true;
}

std::string formatLike(const std::string& old, long long v) {                // a number written the way `old` was
    if (old.size() > 2 && old[0] == '0' && (old[1] == 'x' || old[1] == 'X')) {
        char b[32];
        std::snprintf(b, sizeof b, "0x%llX", (unsigned long long)v);
        return b;
    }
    return std::to_string(v);
}

struct Edit {
    std::string newLine;                 // replaces the line
    std::string before;                  // lines added in front of it ("\n" separated, no trailing newline)
    std::string extra;                   // lines added after it ("\n" separated, no trailing newline)
    bool needsCheck = false;
    std::string code, what;
    std::vector<std::pair<std::string, std::string>> words;
    bool closingJump = false;
    std::string domain, label;
};

struct Ctx {
    explicit Ctx(const lang::Language& l) : lang(l) {}
    const lang::Language& lang;
    SnapshotPtr snap;
    std::set<int> reserved;              // scratchpad bytes given out in this round (two repairs of one round must not take the same pair)
    std::vector<std::string> mnemonics, directives, conditions, regions;
};

std::vector<std::string> namesOf(const lang::Language& l, lang::Tok k) {
    std::vector<std::string> v;
    for (const lang::Entry* e : l.all(k)) v.push_back(e->name);
    return v;
}

// "main" / "peri": the section the line is in.
std::string domainAtLine(const std::vector<std::string>& lines, int line) {
    std::string d;
    for (int i = 0; i < line && i < (int)lines.size(); i++) {
        const std::string t = upper(stripComment(lines[(size_t)i]));
        const size_t p = t.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        if (t.compare(p, 5, ".MAIN") == 0 && (t.size() == p + 5 || !isWordChar(t[p + 5]))) d = "main";
        if (t.compare(p, 5, ".PERI") == 0 && (t.size() == p + 5 || !isWordChar(t[p + 5]))) d = "peri";
    }
    return d;
}

std::vector<std::string> symbolNames(const Ctx& cx, const std::set<std::string>& kinds, const std::string& scope = "") {
    std::vector<std::string> v;
    if (!cx.snap) return v;
    for (const IndexSymbol& s : cx.snap->index.symbols()) {
        if (!kinds.count(s.kind)) continue;
        if (!scope.empty() && !s.scope.empty() && s.scope != scope && s.scope.compare(0, scope.size() + 1, scope + ":") != 0) continue;
        v.push_back(s.name);
    }
    return v;
}

const std::regex& write_re() {                                           // "-> MAIN_SCRATCHPAD[a..b]" or "[a]"
    static const std::regex re("->\\s*MAIN_SCRATCHPAD\\s*\\[\\s*(\\d+)\\s*(?:\\.\\.\\s*(\\d+))?\\s*\\]", std::regex::icase);
    return re;
}

// The bytes of MAIN_SCRATCHPAD that the program names anywhere: [a..b] and [a].
std::set<int> scratchpadInUse(const std::vector<std::string>& lines) {
    std::set<int> used;
    static const std::regex re("MAIN_SCRATCHPAD\\s*\\[\\s*(\\d+)\\s*(?:\\.\\.\\s*(\\d+))?\\s*\\]", std::regex::icase);
    for (const std::string& l : lines) {
        const std::string t = stripComment(l);
        for (std::sregex_iterator it(t.begin(), t.end(), re), end; it != end; ++it) {
            const int a = std::atoi((*it)[1].str().c_str());
            const int b = (*it)[2].matched ? std::atoi((*it)[2].str().c_str()) : a;
            for (int i = std::min(a, b); i <= std::max(a, b) && i < 64; i++) used.insert(i);
        }
    }
    return used;
}

bool replaceAt(std::string& line, size_t pos, size_t len, const std::string& with) {
    if (pos == std::string::npos || pos + len > line.size()) return false;
    line.replace(pos, len, with);
    return true;
}

// Replaces the word `from` on the line (at the diagnostic's columns when they point at it, else the first whole word).
bool replaceWord(std::string& line, const Diagnostic& d, const std::string& from, const std::string& to) {
    if (d.loc.endCol > d.loc.col && (size_t)d.loc.endCol <= line.size() && upper(line.substr((size_t)d.loc.col, (size_t)(d.loc.endCol - d.loc.col))) == upper(from))
        return replaceAt(line, (size_t)d.loc.col, (size_t)(d.loc.endCol - d.loc.col), to);
    const size_t p = findWord(stripComment(line), from);
    return replaceAt(line, p, from.size(), to);
}

// The numbers of the line (decimal or 0x...), with where they stand.
struct Num { size_t pos, len; long long value; };
std::vector<Num> numbersIn(const std::string& line) {
    std::vector<Num> v;
    const std::string t = stripComment(line);
    for (size_t i = 0; i < t.size();) {
        if (std::isdigit((unsigned char)t[i]) && (i == 0 || !isWordChar(t[i - 1]))) {
            size_t j = i;
            while (j < t.size() && (std::isalnum((unsigned char)t[j]))) j++;
            long long val;
            if (parseNumber(t.substr(i, j - i), val)) v.push_back({i, j - i, val});
            i = j;
        } else {
            i++;
        }
    }
    return v;
}

// "s: ALU ADD" -> head "s: ", rest "ALU ADD"; without a label the head is the indentation.
void splitHead(const std::string& line, std::string& head, std::string& rest) {
    static const std::regex re("^(\\s*[A-Za-z_][A-Za-z_0-9]*:\\s*)(.*)$");
    std::smatch m;
    if (std::regex_match(line, m, re)) { head = m[1].str(); rest = m[2].str(); return; }
    const size_t p = line.find_first_not_of(" \t");
    head = line.substr(0, p == std::string::npos ? line.size() : p);
    rest = line.substr(head.size());
}

// New instructions in FRONT of the line: a label on the line moves to the first of them (a jump to it must still run them all).
void insertInFront(const std::string& line, const std::vector<std::string>& ins, Edit& out, std::string& newLine) {
    std::string head, rest;
    splitHead(line, head, rest);
    const bool labelled = head.find(':') != std::string::npos;
    const std::string indent = labelled ? std::string(" ") : head;
    std::string text;
    for (size_t i = 0; i < ins.size(); i++) text += (i ? "\n" + indent : head) + ins[i];
    out.before = text;
    newLine = labelled ? indent + rest : line;
}

std::string mnemonicOf(const std::string& line) {
    std::string head, rest;
    splitHead(stripComment(line), head, rest);
    std::string m;
    for (char c : rest) { if (std::isspace((unsigned char)c)) break; m += (char)std::toupper((unsigned char)c); }
    return m;
}
bool hasLabel(const std::string& line) {
    std::string head, rest;
    splitHead(stripComment(line), head, rest);
    return head.find(':') != std::string::npos;
}
bool endsStraightLine(const std::string& line) {                                 // something that makes the code after it reachable another way, or not at all
    const std::string m = mnemonicOf(line);
    return hasLabel(line) || m.compare(0, 2, "BR") == 0 || m == "JMP" || m == "CALL" || m == "RET" || m.compare(0, 7, "CASCADE") == 0;
}
std::vector<std::string> domainPerLine(const std::vector<std::string>& lines) {
    std::vector<std::string> v;
    std::string d;
    for (const std::string& l : lines) {
        const std::string t = upper(stripComment(l));
        const size_t p = t.find_first_not_of(" \t");
        if (p != std::string::npos && t.compare(p, 5, ".MAIN") == 0) d = "main";
        if (p != std::string::npos && t.compare(p, 5, ".PERI") == 0) d = "peri";
        v.push_back(d);
    }
    return v;
}
// Is there any line of the domain that matches `re`?
bool anyInDomain(const std::vector<std::string>& lines, const std::string& dom, const std::regex& re) {
    const std::vector<std::string> doms = domainPerLine(lines);
    for (size_t i = 0; i < lines.size(); i++)
        if (doms[i] == dom && std::regex_search(stripComment(lines[i]), re)) return true;
    return false;
}
int freePair(const std::vector<std::string>& lines, Ctx& cx) {
    std::set<int> used = scratchpadInUse(lines);
    used.insert(cx.reserved.begin(), cx.reserved.end());
    for (int a = 30; a >= 0; a -= 2) if (!used.count(a) && !used.count(a + 1)) { cx.reserved.insert(a); cx.reserved.insert(a + 1); return a; }
    return -1;
}

bool makeEdit(const Diagnostic& d, const std::vector<std::string>& lines, Ctx& cx, bool wordsOnly, Edit& out) {
    if (d.loc.line < 1 || d.loc.line > (int)lines.size()) return false;
    const std::string orig = lines[(size_t)d.loc.line - 1];
    std::string line = orig;
    std::string cr;
    if (!line.empty() && line.back() == '\r') { cr = "\r"; line.pop_back(); }
    std::smatch m;
    auto done = [&](const std::string& what) {
        out.newLine = line + cr;
        out.code = d.code;
        out.what = what;
        return true;
    };
    auto exchanged = [&](const std::string& from, const std::string& to) { out.words.push_back({from, to}); };

    if (d.code == "E101" && std::regex_search(d.msg, m, std::regex("unknown (?:instruction|directive) '([^']+)'"))) {
        const std::string w = m[1].str();
        const std::string best = closestWord(w, w[0] == '.' ? cx.directives : cx.mnemonics);
        if (best.empty() || upper(best) == upper(w) || !replaceWord(line, d, w, best)) return false;
        exchanged(w, best);
        return done(w + " -> " + best + " (unknown instruction)");
    }
    if (d.code == "E205" && std::regex_search(d.msg, m, std::regex("unknown branch condition '([^']+)'"))) {
        const std::string w = m[1].str();
        std::vector<std::string> cands = cx.conditions;
        const std::string best = closestWord(w, cands);
        if (best.empty() || !replaceWord(line, d, w, best)) return false;
        exchanged(w, best);
        return done(w + " -> " + best + " (unknown branch condition)");
    }
    if (d.code == "E301" && std::regex_search(d.msg, m, std::regex("undefined label '([^']+)'"))) {
        const std::string w = m[1].str();
        const std::string dom = domainAtLine(lines, d.loc.line);
        const std::string best = closestWord(w, symbolNames(cx, {"label", "proc"}, dom));
        if (best.empty() || !replaceWord(line, d, w, best)) return false;
        exchanged(w, best);
        return done(w + " -> " + best + " (undefined branch target)");
    }
    if (d.code == "E103" && std::regex_search(d.msg, m, std::regex("undefined symbol '([^']+)'"))) {
        const std::string w = m[1].str();
        std::vector<std::string> cands = symbolNames(cx, {"equ", "csr", "service", "const", "label", "devmap"});
        cands.insert(cands.end(), cx.regions.begin(), cx.regions.end());
        const std::string best = closestWord(w, cands);
        if (best.empty() || !replaceWord(line, d, w, best)) return false;
        exchanged(w, best);
        return done(w + " -> " + best + " (undefined symbol)");
    }
    if (d.code == "E201") {
        if (std::regex_search(d.msg, m, std::regex("(-?\\d+) is out of range \\((-?\\d+)\\.\\.(-?\\d+)\\)"))) {
            const long long v = std::atoll(m[1].str().c_str()), lo = std::atoll(m[2].str().c_str()), hi = std::atoll(m[3].str().c_str());
            const long long to = std::min(hi, std::max(lo, v));
            for (const Num& n : numbersIn(line))
                if (n.value == v) {
                    const std::string old = line.substr(n.pos, n.len), now = formatLike(old, to);
                    line.replace(n.pos, n.len, now);
                    exchanged(old, now);
                    return done(old + " -> " + now + " (out of range " + m[2].str() + ".." + m[3].str() + ")");
                }
            return false;
        }
        if (std::regex_search(d.msg, m, std::regex("range (\\d+)\\.\\.(\\d+) is outside (\\d+)\\.\\.(\\d+)"))) {
            const int a = std::atoi(m[1].str().c_str()), b = std::atoi(m[2].str().c_str()), lo = std::atoi(m[3].str().c_str()), hi = std::atoi(m[4].str().c_str());
            const int len = b - a;
            int na = a, nb = b;
            if (b > hi) { nb = hi; na = hi - len; }
            if (na < lo) { na = lo; nb = lo + len; }
            if (nb > hi || na < lo || (na == a && nb == b)) return false;
            const std::regex re("(\\[\\s*)" + std::to_string(a) + "(\\s*\\.\\.\\s*)" + std::to_string(b) + "(\\s*\\])");
            std::smatch r;
            if (!std::regex_search(line, r, re)) return false;
            line = line.substr(0, (size_t)r.position(0)) + r[1].str() + std::to_string(na) + r[2].str() + std::to_string(nb) + r[3].str() + line.substr((size_t)(r.position(0) + r.length(0)));
            exchanged(std::to_string(a), std::to_string(na));
            exchanged(std::to_string(b), std::to_string(nb));
            out.needsCheck = true;
            return done(std::to_string(a) + ".." + std::to_string(b) + " -> " + std::to_string(na) + ".." + std::to_string(nb) + " (the range ran past the end)");
        }
        if (std::regex_search(d.msg, m, std::regex("CSR address (\\d+) does not fit (\\d+) bits"))) {
            const long long v = std::atoll(m[1].str().c_str());
            const long long mask = (1LL << std::atoi(m[2].str().c_str())) - 1;
            for (const Num& n : numbersIn(line))
                if (n.value == v) {
                    const std::string old = line.substr(n.pos, n.len), now = formatLike(old.size() > 2 && (old[1] == 'x' || old[1] == 'X') ? old : "0x0", v & mask);
                    line.replace(n.pos, n.len, now);
                    exchanged(old, now);
                    out.needsCheck = true;
                    return done(old + " -> " + now + " (a CSR address is " + m[2].str() + " bits)");
                }
        }
        return false;
    }
    if (d.code == "E204" && d.msg.find("WRITE_BUF") != std::string::npos) {
        const size_t p = findWord(stripComment(line), "WRITE_BUF");
        if (!replaceAt(line, p, 9, "WRITE_SP")) return false;
        exchanged("WRITE_BUF", "WRITE_SP");
        out.needsCheck = true;
        return done("WRITE_BUF -> WRITE_SP (renamed in spec v31; check that ptr points into MAIN_SCRATCHPAD)");
    }
    if (d.code == "E202" && d.msg.find("MAIN_CONST_POOL can only be moved to MAIN_SCRATCHPAD") != std::string::npos && !wordsOnly) {
        static const std::regex re("^(\\s*(?:[A-Za-z_][A-Za-z_0-9]*:\\s*)?)MOV\\s+(CONST\\s*\\(([^)]*)\\))\\s*->\\s*(ALU_OP[AB])\\s*(;.*)?$", std::regex::icase);
        std::smatch r;
        if (!std::regex_match(line, r, re)) return false;
        const std::string args = r[3].str();
        int width = 2;
        const size_t comma = args.rfind(',');
        if (comma != std::string::npos) {
            long long w;
            if (!parseNumber(args.substr(comma + 1).erase(0, args.substr(comma + 1).find_first_not_of(" \t")), w)) return false;
            width = (int)w;
        }
        if (width != 2) return false;                                  // an ALU operand is 2 bytes: a 1-byte constant needs a decision, not a guess
        std::set<int> used = scratchpadInUse(lines);
        used.insert(cx.reserved.begin(), cx.reserved.end());
        int k = -1;
        for (int a = 30; a >= 0; a -= 2) if (!used.count(a) && !used.count(a + 1)) { k = a; break; }
        if (k < 0) return false;
        cx.reserved.insert(k);
        cx.reserved.insert(k + 1);
        const std::string slot = "MAIN_SCRATCHPAD[" + std::to_string(k) + ".." + std::to_string(k + 1) + "]";
        const std::string indent = r[1].str();
        const std::string ws = indent.substr(0, indent.find_first_not_of(" \t") == std::string::npos ? indent.size() : indent.find_first_not_of(" \t"));
        const std::string head = r[1].str(), konst = r[2].str(), alu = r[4].str();            // (r refers to `line`, which is about to change)
        const std::string comment = r[5].matched ? " " + r[5].str() : "";
        line = head + "MOV " + konst + " -> " + slot + comment;
        out.extra = ws + (head.find(':') != std::string::npos ? std::string(" ") : std::string()) + "MOV " + slot + " -> " + alu;
        out.words.clear();
        return done("MOV CONST -> " + alu + " now goes through " + slot + " (constants reach the ALU through the scratchpad, spec v31 5.1)");
    }
    if (d.code == "E303" && std::regex_search(d.msg, m, std::regex("(Main|Peri) code can run off its end"))) {
        const std::string dom = m[1].str() == "Main" ? "main" : "peri";
        std::string label;
        int firstLine = 1 << 30;
        if (cx.snap)
            for (const IndexSymbol& s : cx.snap->index.symbols())
                if (s.kind == "label" && s.scope == dom && s.def.line < firstLine) { firstLine = s.def.line; label = s.name; }
        std::string target = label;
        if (target.empty()) target = "halt_" + dom;
        const std::string ind = " ";
        out.extra = label.empty() ? target + ": JMP " + target : ind + "JMP " + target;
        out.closingJump = true;
        out.domain = dom;
        out.label = label;
        return done("added: JMP " + target + " (the " + m[1].str() + " code ran off its end)");
    }
    if (d.code == "E203" && std::regex_search(d.msg, m, std::regex("unknown ALU operation '([^']+)'"))) {
        std::vector<std::string> cands;
        for (const lang::Entry* e : cx.lang.all(lang::Tok::SubOp)) if (e->group == "alu") cands.push_back(e->name);
        const std::string w = m[1].str();
        const std::string best = closestWord(w, cands);
        if (best.empty() || !replaceWord(line, d, w, best)) return false;
        exchanged(w, best);
        return done(w + " -> " + best + " (unknown ALU operation)");
    }
    if (d.code == "E104" && std::regex_search(d.msg, m, std::regex("label '([^']+)' is already defined")) && !wordsOnly) {
        const std::string name = m[1].str();
        std::set<std::string> taken;
        for (const IndexSymbol& s : cx.snap->index.symbols()) taken.insert(upper(s.name));
        std::string fresh;
        for (int i = 2; i < 100 && fresh.empty(); i++) if (!taken.count(upper(name + "_" + std::to_string(i)))) fresh = name + "_" + std::to_string(i);
        std::smatch r;
        if (fresh.empty() || !std::regex_search(line, r, std::regex("^(\\s*)" + name + "(\\s*:)"))) return false;
        line = r[1].str() + fresh + r[2].str() + line.substr((size_t)r.length(0));
        out.needsCheck = true;
        return done("label " + name + " -> " + fresh + " (it was defined twice; jumps to " + name + " still go to the first)");
    }
    if (d.code == "E105" && d.msg.find("instruction before any") != std::string::npos && !wordsOnly) {
        const std::string mn = mnemonicOf(line);
        const std::string dir = mn == "EXEC_XFER" || mn == "EXEC_PUB" ? ".peri" : ".main";
        out.before = dir;
        out.needsCheck = true;
        return done("added " + dir + " in front of the first instruction");
    }
    if (d.code == "W501" && !wordsOnly && std::regex_search(d.msg, m, std::regex("ALU_OP([AB]) is not loaded on every path"))) {
        const std::string reg = "ALU_OP" + m[1].str();
        const std::string dom = domainAtLine(lines, d.loc.line);
        if (dom.empty() || anyInDomain(lines, dom, std::regex("->\\s*" + reg + "\\b", std::regex::icase))) return false;       // loaded somewhere: which value belongs here is for you to say
        std::smatch op;
        const std::string code = stripComment(line);
        if (!std::regex_search(code, op, std::regex("\\bALU\\s+([A-Za-z_.]+)", std::regex::icase))) return false;
        const std::string alu = upper(op[1].str());
        // the last 2-byte value that was written into the scratchpad in this stretch of code, and not already taken for the other operand
        std::string source;
        const std::string other = reg == "ALU_OPA" ? "ALU_OPB" : "ALU_OPA";
        static const std::regex write("->\\s*MAIN_SCRATCHPAD\\s*\\[\\s*(\\d+)\\s*\\.\\.\\s*(\\d+)\\s*\\]", std::regex::icase);
        for (int i = d.loc.line - 2; i >= 0 && source.empty(); i--) {
            const std::string t = stripComment(lines[(size_t)i]);
            std::smatch w;
            if (std::regex_search(t, w, write) && std::atoi(w[2].str().c_str()) == std::atoi(w[1].str().c_str()) + 1) {
                const std::string pair = "MAIN_SCRATCHPAD[" + w[1].str() + ".." + w[2].str() + "]";
                bool usedByOther = false;
                for (int j = i + 1; j < d.loc.line - 1; j++) {
                    const std::string u = upper(stripComment(lines[(size_t)j]));
                    if (u.find(upper(pair)) != std::string::npos && u.find(other) != std::string::npos) usedByOther = true;
                }
                if (!usedByOther) source = pair;
            }
            if (endsStraightLine(lines[(size_t)i])) break;
        }
        std::vector<std::string> ins;
        std::string why;
        if (!source.empty()) {
            ins.push_back("MOV " + source + " -> " + reg);
            why = reg + " had no value: loaded from " + source + ", the last 2-byte value written before it";
        } else {
            const std::string neutral = alu == "AND" ? "0xFFFF" : alu == "MUL" ? "1" : "0";
            const int k = freePair(lines, cx);
            if (k < 0) return false;
            const std::string slot = "MAIN_SCRATCHPAD[" + std::to_string(k) + ".." + std::to_string(k + 1) + "]";
            ins.push_back("MOV CONST(" + neutral + ", 2) -> " + slot);
            ins.push_back("MOV " + slot + " -> " + reg);
            why = reg + " had no value: loaded " + neutral + ", the neutral value of " + alu + " - put the value you meant there";
        }
        std::string nl;
        insertInFront(line, ins, out, nl);
        line = nl;
        out.needsCheck = true;
        return done(why);
    }
    if (d.code == "W502" && !wordsOnly && std::regex_search(d.msg, m, std::regex("MAIN_SCRATCHPAD\\[(\\d+)\\.\\.(\\d+)\\] is read here"))) {
        const int a = std::atoi(m[1].str().c_str()), b = std::atoi(m[2].str().c_str());
        const std::string dom = domainAtLine(lines, d.loc.line);
        if (dom.empty() || b < a || b - a + 1 > 8) return false;
        const std::vector<std::string> doms = domainPerLine(lines);
        for (size_t i = 0; i < lines.size(); i++) {                          // nothing in the domain writes any of these bytes: a read of something never set
            if (doms[i] != dom) continue;
            const std::string t = stripComment(lines[i]);
            std::smatch w;
            for (std::sregex_iterator it(t.begin(), t.end(), write_re()), end; it != end; ++it) {
                const int wa = std::atoi((*it)[1].str().c_str());
                const int wb = (*it)[2].matched ? std::atoi((*it)[2].str().c_str()) : wa;
                if (wa <= b && wb >= a) return false;
            }
        }
        std::string nl;
        insertInFront(line, {"MOV CONST(0, " + std::to_string(b - a + 1) + ") -> MAIN_SCRATCHPAD[" + std::to_string(a) + ".." + std::to_string(b) + "]"}, out, nl);
        line = nl;
        out.needsCheck = true;
        return done("MAIN_SCRATCHPAD[" + std::to_string(a) + ".." + std::to_string(b) + "] was read but never written: set to 0 first - put the value you meant there");
    }
    if (d.code == "W504" && !wordsOnly && std::regex_search(d.msg, m, std::regex("result of the ALU operation at line (\\d+) is overwritten here"))) {
        const int from = std::atoi(m[1].str().c_str());
        if (from < 1 || from >= d.loc.line || mnemonicOf(lines[(size_t)from - 1]) != "ALU") return false;
        for (int i = from; i < d.loc.line - 1; i++) if (endsStraightLine(lines[(size_t)i])) return false;       // only where both are on one path
        const int k = freePair(lines, cx);
        if (k < 0) return false;
        const std::string slot = "MAIN_SCRATCHPAD[" + std::to_string(k) + ".." + std::to_string(k + 1) + "]";
        std::string nl;
        insertInFront(line, {"MOV ALU_OUT[0..1] -> " + slot}, out, nl);
        line = nl;
        out.needsCheck = true;
        return done("the result of line " + std::to_string(from) + " is saved in " + slot + " before the next ALU operation overwrites it - use it from there");
    }
    if (d.code == "W410") {
        for (const char* w : {"unsigned", "signed"}) {
            const size_t p = findWord(stripComment(line), w);
            if (p == std::string::npos) continue;
            size_t b = p, e = p + std::strlen(w);
            while (b > 0 && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
            line.erase(b, e - b);
            exchanged(w, "");
            return done(std::string("dropped ") + w + " (a LOGIC operation has no signed mode)");
        }
        return false;
    }
    if (d.code == "W517" && std::regex_search(d.msg, m, std::regex("declared with len=(\\d+) .*EXEC_PUB publishes (\\d+) byte"))) {
        const std::string want = m[1].str(), have = m[2].str();
        std::smatch r;
        const std::regex re("(\\blen\\s*=\\s*)" + have + "\\b", std::regex::icase);
        if (!std::regex_search(line, r, re)) return false;
        line = line.substr(0, (size_t)r.position(0)) + r[1].str() + want + line.substr((size_t)(r.position(0) + r.length(0)));
        exchanged(have, want);
        return done("len=" + have + " -> len=" + want + " (the .service contract)");
    }
    return false;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t e = text.find('\n', i);
        if (e == std::string::npos) { if (i < text.size()) v.push_back(text.substr(i)); break; }
        v.push_back(text.substr(i, e - i));
        i = e + 1;
    }
    return v;
}

std::string joinLines(const std::vector<std::string>& v, bool finalNewline) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) { s += v[i]; if (i + 1 < v.size() || finalNewline) s += "\n"; }
    return s;
}

}  // namespace

std::string closestWord(const std::string& word, const std::vector<std::string>& candidates) {
    const std::string w = upper(word);
    const int limit = std::max(1, (int)w.size() / 3);
    int best = 1 << 20;
    std::string pick;
    bool tie = false;
    std::set<std::string> seen;
    for (const std::string& c : candidates) {
        if (!seen.insert(upper(c)).second) continue;
        const int dist = distance(w, upper(c));
        if (dist < best) { best = dist; pick = c; tie = false; }
        else if (dist == best) tie = true;
    }
    if (pick.empty() || tie || best > limit || best == 0) return std::string();
    return pick;
}

AutoFixResult autoFix(BuildInput in, const lang::Language& language, const AutoFixOptions& opt) {
    AutoFixResult res;
    std::map<std::string, std::string> original;
    for (const Source& s : in.sources) original[s.name] = s.text;
    for (const auto& kv : in.overlay) if (!original.count(kv.first)) original[kv.first] = kv.second;
    std::map<std::string, std::string> text = original;
    auto sync = [&] {
        for (Source& s : in.sources) s.text = text[s.name];
        for (auto& kv : in.overlay) if (text.count(kv.first)) kv.second = text[kv.first];
    };
    Ctx cx(language);
    cx.mnemonics = namesOf(language, lang::Tok::Mnemonic);
    cx.directives = namesOf(language, lang::Tok::Directive);
    cx.conditions = namesOf(language, lang::Tok::Condition);
    cx.regions = namesOf(language, lang::Tok::Region);
    std::set<std::string> tried;                                       // (file:line:code:text) - a repair that came back is not made twice
    size_t previousProblems = (size_t)-1;
    int noProgress = 0;
    for (int round = 0; round < opt.maxRounds; round++) {
        sync();
        res.build = buildNow(in, language);
        cx.snap = res.build;
        cx.reserved.clear();
        res.rounds = round;
        size_t problems = 0;
        for (const auto& kv : res.build->diagsByFile) for (const Diagnostic& d : kv.second) if (d.sev != Diagnostic::Note) problems++;
        if (problems == 0) break;
        if (problems >= previousProblems && ++noProgress >= 2) break;            // it is not getting better
        previousProblems = problems;
        // one repair per line and round, made from the bottom of each file upwards so that the line numbers of the others stay right
        struct Pending { std::string file; int line; Edit edit; };
        std::vector<Pending> todo;
        std::set<std::pair<std::string, int>> taken;
        for (const auto& kv : res.build->diagsByFile) {
            const auto it = text.find(kv.first);
            if (it == text.end()) continue;
            const std::vector<std::string> lines = splitLines(it->second);
            for (const Diagnostic& d : kv.second) {
                if (d.sev == Diagnostic::Note || taken.count({kv.first, d.loc.line})) continue;
                Edit e;
                if (!makeEdit(d, lines, cx, opt.wordsOnly || opt.wordsOnlyFiles.count(kv.first) > 0, e)) continue;
                const std::string key = kv.first + ":" + std::to_string(d.loc.line) + ":" + e.code + ":" + e.what;
                if (!tried.insert(key).second) continue;
                taken.insert({kv.first, d.loc.line});
                todo.push_back({kv.first, d.loc.line, e});
            }
        }
        if (todo.empty()) break;
        {   // a jump that closes the code moves the lines below it: first everything else, then these
            const bool other = std::any_of(todo.begin(), todo.end(), [](const Pending& p) { return !p.edit.closingJump; });
            if (other) todo.erase(std::remove_if(todo.begin(), todo.end(), [](const Pending& p) { return p.edit.closingJump; }), todo.end());
        }
        std::sort(todo.begin(), todo.end(), [](const Pending& a, const Pending& b) { return a.file != b.file ? a.file < b.file : a.line > b.line; });
        std::vector<FixStep> roundSteps;
        std::string curFile;
        std::vector<std::string> lines;
        bool finalNl = true;
        auto flush = [&] { if (!curFile.empty()) text[curFile] = joinLines(lines, finalNl); };
        for (const Pending& p : todo) {
            if (p.file != curFile) {
                flush();
                curFile = p.file;
                lines = splitLines(text[curFile]);
                finalNl = text[curFile].empty() || text[curFile].back() == '\n';
            }
            FixStep s;
            s.code = p.edit.code; s.what = p.edit.what; s.file = p.file; s.line = p.line;
            s.before = lines[(size_t)p.line - 1];
            s.words = p.edit.words;
            s.closingJump = p.edit.closingJump; s.domain = p.edit.domain; s.label = p.edit.label;
            s.needsCheck = p.edit.needsCheck;
            if (p.edit.closingJump) {
                if ((opt.wordsOnly || opt.wordsOnlyFiles.count(p.file)) && p.edit.label.empty()) continue;      // a block program without a label to jump to: not mended here
                s.after = p.edit.extra;
                lines.insert(lines.begin() + p.line, p.edit.extra);
            } else {
                lines[(size_t)p.line - 1] = p.edit.newLine;
                s.after = p.edit.newLine;
                if (!p.edit.extra.empty()) {
                    lines.insert(lines.begin() + p.line, p.edit.extra);
                    s.after += "\n" + p.edit.extra;
                }
                if (!p.edit.before.empty()) {
                    lines.insert(lines.begin() + (p.line - 1), p.edit.before);
                    s.after = p.edit.before + "\n" + s.after;
                }
            }
            for (char& c : s.before) if (c == '\r') c = ' ';
            roundSteps.push_back(s);
        }
        flush();
        if (roundSteps.empty()) break;
        for (FixStep& s : roundSteps) res.steps.push_back(std::move(s));
        res.rounds = round + 1;
    }
    sync();
    res.build = buildNow(in, language);
    for (const auto& kv : res.build->diagsByFile) for (const Diagnostic& d : kv.second) if (d.sev != Diagnostic::Note) res.remaining.push_back(d);
    for (const auto& kv : text) if (kv.second != original[kv.first]) res.texts[kv.first] = kv.second;
    return res;
}

}  // namespace acide
