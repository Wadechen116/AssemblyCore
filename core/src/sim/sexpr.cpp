#include "sexpr.h"

#include <vector>

#include "../util.h"

namespace asmcore {
namespace sim {

struct Expr::Node {
    enum Kind { Num, Var, Un, Bin } kind = Num;
    int64_t num = 0;
    int handle = -1;
    std::shared_ptr<const Node> idx, idxEnd;   // Var with [..]: idx, and idxEnd for a range
    bool indexed = false;
    std::string op;
    std::shared_ptr<const Node> l, r;
};

namespace {

typedef std::shared_ptr<const Expr::Node> NodeP;

struct P {
    const std::string& s;
    ExprBinder& b;
    size_t p = 0;
    std::string err;
    P(const std::string& t, ExprBinder& bb) : s(t), b(bb) {}

    void ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) p++; }
    bool peek(const char* t) { ws(); size_t n = std::char_traits<char>::length(t); return s.compare(p, n, t) == 0; }
    bool eat(const char* t) { if (peek(t)) { p += std::char_traits<char>::length(t); return true; } return false; }
    NodeP fail(const std::string& m) { if (err.empty()) err = m; return nullptr; }

    NodeP bin(const std::string& op, NodeP l, NodeP r) {
        auto n = std::make_shared<Expr::Node>();
        n->kind = Expr::Node::Bin;
        n->op = op;
        n->l = l;
        n->r = r;
        return n;
    }
    NodeP primary() {
        ws();
        if (p >= s.size()) return fail("expression ends early");
        char c = s[p];
        if (c == '(') {
            p++;
            NodeP e = orE();
            if (!e) return nullptr;
            if (!eat(")")) return fail("')' expected");
            return e;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t st = p;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '_')) p++;
            int64_t v;
            std::string e2;
            if (!evalExpr(s.substr(st, p - st), nullptr, v, e2)) return fail(e2);
            auto n = std::make_shared<Expr::Node>();
            n->kind = Expr::Node::Num;
            n->num = v;
            return n;
        }
        if (isIdentStart(c)) {
            size_t st = p;
            while (p < s.size() && isIdentChar(s[p])) {
                // "a..b" inside an index must not be swallowed by the identifier
                if (s[p] == '.' && p + 1 < s.size() && s[p + 1] == '.') break;
                p++;
            }
            std::string name = s.substr(st, p - st);
            auto n = std::make_shared<Expr::Node>();
            n->kind = Expr::Node::Var;
            if (p < s.size() && s[p] == '[') {
                p++;
                n->indexed = true;
                n->idx = orE();
                if (!n->idx) return nullptr;
                if (eat("..")) { n->idxEnd = orE(); if (!n->idxEnd) return nullptr; }
                if (!eat("]")) return fail("']' expected");
            }
            n->handle = b.bind(name, n->indexed);
            if (n->handle < 0) return fail("unknown name '" + name + "'");
            return n;
        }
        return fail(std::string("unexpected '") + c + "'");
    }
    NodeP unary() {
        ws();
        for (const char* o : {"!", "~", "-"}) {
            if (p < s.size() && s[p] == o[0] && !(o[0] == '!' && p + 1 < s.size() && s[p + 1] == '=')) {
                p++;
                NodeP e = unary();
                if (!e) return nullptr;
                auto n = std::make_shared<Expr::Node>();
                n->kind = Expr::Node::Un;
                n->op = o;
                n->l = e;
                return n;
            }
        }
        return primary();
    }
    NodeP mul() {
        NodeP l = unary();
        while (l) {
            ws();
            if (p < s.size() && (s[p] == '*' || s[p] == '/' || s[p] == '%')) { std::string o(1, s[p++]); NodeP r = unary(); if (!r) return nullptr; l = bin(o, l, r); }
            else break;
        }
        return l;
    }
    NodeP add() {
        NodeP l = mul();
        while (l) {
            ws();
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) { std::string o(1, s[p++]); NodeP r = mul(); if (!r) return nullptr; l = bin(o, l, r); }
            else break;
        }
        return l;
    }
    NodeP shift() {
        NodeP l = add();
        while (l) {
            if (peek("<<")) { p += 2; NodeP r = add(); if (!r) return nullptr; l = bin("<<", l, r); }
            else if (peek(">>")) { p += 2; NodeP r = add(); if (!r) return nullptr; l = bin(">>", l, r); }
            else break;
        }
        return l;
    }
    NodeP rel() {
        NodeP l = shift();
        while (l) {
            std::string o;
            ws();
            if (peek("<=")) o = "<="; else if (peek(">=")) o = ">=";
            else if (peek("<") && !peek("<<")) o = "<"; else if (peek(">") && !peek(">>")) o = ">";
            else break;
            p += o.size();
            NodeP r = shift();
            if (!r) return nullptr;
            l = bin(o, l, r);
        }
        return l;
    }
    NodeP eq() {
        NodeP l = rel();
        while (l) {
            std::string o;
            if (peek("==")) o = "=="; else if (peek("!=")) o = "!="; else break;
            p += 2;
            NodeP r = rel();
            if (!r) return nullptr;
            l = bin(o, l, r);
        }
        return l;
    }
    NodeP band() {
        NodeP l = eq();
        while (l) { if (peek("&") && !peek("&&")) { p++; NodeP r = eq(); if (!r) return nullptr; l = bin("&", l, r); } else break; }
        return l;
    }
    NodeP bxor() {
        NodeP l = band();
        while (l) { if (peek("^")) { p++; NodeP r = band(); if (!r) return nullptr; l = bin("^", l, r); } else break; }
        return l;
    }
    NodeP bor() {
        NodeP l = bxor();
        while (l) { if (peek("|") && !peek("||")) { p++; NodeP r = bxor(); if (!r) return nullptr; l = bin("|", l, r); } else break; }
        return l;
    }
    NodeP land() {
        NodeP l = bor();
        while (l) { if (peek("&&")) { p += 2; NodeP r = bor(); if (!r) return nullptr; l = bin("&&", l, r); } else break; }
        return l;
    }
    NodeP orE() {
        NodeP l = land();
        while (l) { if (peek("||")) { p += 2; NodeP r = land(); if (!r) return nullptr; l = bin("||", l, r); } else break; }
        return l;
    }
};

int64_t run(const Expr::Node* n, const ExprEnv& env) {
    switch (n->kind) {
        case Expr::Node::Num: return n->num;
        case Expr::Node::Var: {
            if (!n->indexed) return env.get(n->handle, 0, 0);
            int64_t a = run(n->idx.get(), env);
            int len = 1;
            if (n->idxEnd) { int64_t b = run(n->idxEnd.get(), env); len = (int)(b - a + 1); if (len < 1) len = 1; }
            return env.get(n->handle, a, len);
        }
        case Expr::Node::Un: {
            int64_t v = run(n->l.get(), env);
            return n->op == "!" ? !v : n->op == "~" ? ~v : -v;
        }
        case Expr::Node::Bin: {
            const std::string& o = n->op;
            if (o == "&&") return run(n->l.get(), env) && run(n->r.get(), env);
            if (o == "||") return run(n->l.get(), env) || run(n->r.get(), env);
            int64_t a = run(n->l.get(), env), b = run(n->r.get(), env);
            if (o == "+") return a + b;
            if (o == "-") return a - b;
            if (o == "*") return a * b;
            if (o == "/") return b ? a / b : 0;
            if (o == "%") return b ? a % b : 0;
            if (o == "<<") return b >= 0 && b < 63 ? (int64_t)((uint64_t)a << b) : 0;
            if (o == ">>") return b >= 0 && b < 63 ? a >> b : 0;
            if (o == "<") return a < b;
            if (o == "<=") return a <= b;
            if (o == ">") return a > b;
            if (o == ">=") return a >= b;
            if (o == "==") return a == b;
            if (o == "!=") return a != b;
            if (o == "&") return a & b;
            if (o == "^") return a ^ b;
            return a | b;
        }
    }
    return 0;
}

}  // namespace

bool Expr::compile(const std::string& text, ExprBinder& binder, Expr& out, std::string& error) {
    P ps(text, binder);
    if (trim(text).empty()) { error = "empty expression"; return false; }
    NodeP root = ps.orE();
    if (!root) { error = ps.err.empty() ? "bad expression" : ps.err; return false; }
    ps.ws();
    if (ps.p != text.size()) { error = std::string("unexpected '") + text[ps.p] + "' in expression"; return false; }
    out.root_ = root;
    out.text_ = text;
    return true;
}

int64_t Expr::eval(const ExprEnv& env) const { return root_ ? run(root_.get(), env) : 0; }

}  // namespace sim
}  // namespace asmcore
