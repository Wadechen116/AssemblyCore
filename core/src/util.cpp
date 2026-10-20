#include <cstdio>
#include "util.h"

namespace asmcore {

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char ch : s) {
        unsigned char c = (unsigned char)ch;
        switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04X", c); o += b; }
                else o += ch;
        }
    }
    return o;
}

std::string stripComment(const std::string& line) {
    bool inStr = false;
    char q = 0;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (inStr) {
            if (c == q) inStr = false;
        } else if (c == '"' || c == '\'') {
            inStr = true;
            q = c;
        } else if (c == ';') {
            return line.substr(0, i);
        }
    }
    return line;
}

std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    bool inStr = false;
    char q = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (inStr) {
            if (c == q) inStr = false;
            continue;
        }
        if (c == '"' || c == '\'') { inStr = true; q = c; }
        else if (c == '[' || c == '(') depth++;
        else if (c == ']' || c == ')') depth--;
        else if (c == sep && depth == 0) {
            out.push_back(trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    std::string last = trim(s.substr(start));
    if (!last.empty() || !out.empty()) out.push_back(last);
    return out;
}

size_t findTop(const std::string& s, const std::string& needle) {
    int depth = 0;
    bool inStr = false;
    char q = 0;
    for (size_t i = 0; i + needle.size() <= s.size(); i++) {
        char c = s[i];
        if (inStr) {
            if (c == q) inStr = false;
            continue;
        }
        if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
        if (c == '[' || c == '(') depth++;
        else if (c == ']' || c == ')') depth--;
        else if (depth == 0 && s.compare(i, needle.size(), needle) == 0) return i;
    }
    return std::string::npos;
}

namespace {

struct Parser {
    const std::string& s;
    size_t p = 0;
    const SymbolLookup& lookup;
    std::string err;
    Parser(const std::string& t, const SymbolLookup& l) : s(t), lookup(l) {}

    void ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) p++; }
    bool fail(const std::string& m) { if (err.empty()) err = m; return false; }

    bool number(int64_t& v) {
        size_t st = p;
        while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '_')) p++;
        std::string t = s.substr(st, p - st);
        std::string clean;
        for (char c : t) if (c != '_') clean += c;
        if (clean.empty()) return fail("number expected");
        try {
            size_t used = 0;
            std::string l = lower(clean);
            if (l.size() > 2 && l[0] == '0' && l[1] == 'x') { v = (int64_t)std::stoull(l.substr(2), &used, 16); return used == l.size() - 2 || fail("bad hex number '" + t + "'"); }
            if (l.size() > 2 && l[0] == '0' && l[1] == 'b') { v = (int64_t)std::stoull(l.substr(2), &used, 2); return used == l.size() - 2 || fail("bad binary number '" + t + "'"); }
            if (l.back() == 'h') { v = (int64_t)std::stoull(l.substr(0, l.size() - 1), &used, 16); return used == l.size() - 1 || fail("bad hex number '" + t + "'"); }
            bool allBin = l.size() > 1 && l.back() == 'b';
            for (size_t i = 0; allBin && i + 1 < l.size(); i++) if (l[i] != '0' && l[i] != '1') allBin = false;
            if (allBin) { v = (int64_t)std::stoull(l.substr(0, l.size() - 1), &used, 2); return true; }
            v = (int64_t)std::stoull(l, &used, 10);
            return used == l.size() || fail("bad number '" + t + "'");
        } catch (...) {
            return fail("bad number '" + t + "'");
        }
    }

    bool primary(int64_t& v) {
        ws();
        if (p >= s.size()) return fail("expression ends early");
        char c = s[p];
        if (c == '(') {
            p++;
            if (!orExpr(v)) return false;
            ws();
            if (p >= s.size() || s[p] != ')') return fail("')' expected");
            p++;
            return true;
        }
        if (c == '\'') {
            if (p + 2 < s.size() && s[p + 2] == '\'') { v = (unsigned char)s[p + 1]; p += 3; return true; }
            return fail("bad character constant");
        }
        if (std::isdigit((unsigned char)c)) return number(v);
        if (isIdentStart(c)) {
            size_t st = p;
            while (p < s.size() && isIdentChar(s[p])) p++;
            std::string name = s.substr(st, p - st);
            if (!lookup || !lookup(name, v)) return fail("undefined symbol '" + name + "'");
            return true;
        }
        return fail(std::string("unexpected '") + c + "'");
    }
    bool unary(int64_t& v) {
        ws();
        if (p < s.size() && (s[p] == '-' || s[p] == '+' || s[p] == '~')) {
            char c = s[p++];
            if (!unary(v)) return false;
            v = c == '-' ? -v : c == '~' ? ~v : v;
            return true;
        }
        return primary(v);
    }
    bool mulExpr(int64_t& v) {
        if (!unary(v)) return false;
        for (;;) {
            ws();
            if (p < s.size() && (s[p] == '*' || s[p] == '/' || s[p] == '%')) {
                char c = s[p++];
                int64_t r;
                if (!unary(r)) return false;
                if (c != '*' && r == 0) return fail("division by zero");
                v = c == '*' ? v * r : c == '/' ? v / r : v % r;
            } else return true;
        }
    }
    bool addExpr(int64_t& v) {
        if (!mulExpr(v)) return false;
        for (;;) {
            ws();
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
                char c = s[p++];
                int64_t r;
                if (!mulExpr(r)) return false;
                v = c == '+' ? v + r : v - r;
            } else return true;
        }
    }
    bool shiftExpr(int64_t& v) {
        if (!addExpr(v)) return false;
        for (;;) {
            ws();
            if (p + 1 < s.size() && ((s[p] == '<' && s[p + 1] == '<') || (s[p] == '>' && s[p + 1] == '>'))) {
                bool left = s[p] == '<';
                p += 2;
                int64_t r;
                if (!addExpr(r)) return false;
                if (r < 0 || r > 63) return fail("shift count out of range");
                v = left ? (int64_t)((uint64_t)v << r) : (v >> r);
            } else return true;
        }
    }
    bool andExpr(int64_t& v) {
        if (!shiftExpr(v)) return false;
        for (;;) {
            ws();
            if (p < s.size() && s[p] == '&') { p++; int64_t r; if (!shiftExpr(r)) return false; v &= r; } else return true;
        }
    }
    bool xorExpr(int64_t& v) {
        if (!andExpr(v)) return false;
        for (;;) {
            ws();
            if (p < s.size() && s[p] == '^') { p++; int64_t r; if (!andExpr(r)) return false; v ^= r; } else return true;
        }
    }
    bool orExpr(int64_t& v) {
        if (!xorExpr(v)) return false;
        for (;;) {
            ws();
            if (p < s.size() && s[p] == '|') { p++; int64_t r; if (!xorExpr(r)) return false; v |= r; } else return true;
        }
    }
};

}  // namespace

bool evalExpr(const std::string& text, const SymbolLookup& lookup, int64_t& out, std::string& err) {
    Parser ps(text, lookup);
    if (trim(text).empty()) { err = "empty expression"; return false; }
    if (!ps.orExpr(out)) { err = ps.err; return false; }
    ps.ws();
    if (ps.p != text.size()) { err = "unexpected '" + text.substr(ps.p, 1) + "' in expression"; return false; }
    return true;
}

}  // namespace asmcore
