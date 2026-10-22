// Text -> blocks. A line that cannot be understood becomes a Raw block that keeps the text, so importing never fails and never loses anything.
#include <algorithm>
#include <functional>
#include <sstream>

#include "asmcore/blocks.h"
#include "blocks_internal.h"
#include "util.h"

namespace asmcore {
namespace blocks {

namespace {

struct Importer {
    Document doc;
    const Registry& reg;
    ImportStats* stats;
    Section* cur = nullptr;
    bool inProc = false;
    Block proc;

    Importer(const Registry& r, ImportStats* s) : reg(r), stats(s) {}

    std::vector<Block>& target() { return inProc ? *proc.stack("BODY", true) : section().blocks; }
    Section& section() {
        if (!cur) { doc.sections.push_back(Section()); cur = &doc.sections.back(); }
        return *cur;
    }
    void closeProc() {
        if (!inProc) return;
        section().blocks.push_back(proc);
        inProc = false;
    }
    void startSection(const std::string& domain, const std::string& comment) {
        closeProc();
        doc.sections.push_back(Section());
        cur = &doc.sections.back();
        cur->domain = domain;
        cur->comment = comment;
    }

    Block raw(const std::string& text) {
        Block b = doc.make("raw");
        b.set("text", text);
        if (stats) { stats->raw++; stats->rawLines.push_back(text); }
        return b;
    }

    // ---- one statement -> a block (or a Raw block)
    static std::vector<std::string> args(const std::string& s) {
        std::vector<std::string> v;
        if (trim(s).empty()) return v;
        for (const std::string& a : splitTop(s)) v.push_back(trim(a));
        return v;
    }
    // key=value pairs in any order; every key in `keys` exactly once, nothing else
    static bool keywords(const std::vector<std::string>& a, size_t from, const std::vector<std::string>& keys, std::map<std::string, std::string>& out) {
        out.clear();
        for (size_t i = from; i < a.size(); i++) {
            size_t eq = a[i].find('=');
            if (eq == std::string::npos) return false;
            std::string k = lower(trim(a[i].substr(0, eq))), v = trim(a[i].substr(eq + 1));
            if (v.empty() || out.count(k)) return false;
            out[k] = v;
        }
        if (out.size() != keys.size()) return false;
        for (const std::string& k : keys) if (!out.count(k)) return false;
        return true;
    }

    bool statement(const std::string& text, Block& out) {
        size_t sp = 0;
        while (sp < text.size() && !std::isspace((unsigned char)text[sp])) sp++;
        const std::string op = text.substr(0, sp), rest = trim(text.substr(sp)), u = upper(op);
        std::vector<std::string> a = args(rest);
        auto done = [&](Block b) { out = std::move(b); return true; };
        if (u == "MOV") {
            size_t arrow = findTop(rest, "->");
            if (arrow == std::string::npos) return false;
            std::string src = trim(rest.substr(0, arrow)), dst = trim(rest.substr(arrow + 2));
            if (src.empty() || dst.empty() || findTop(dst, "->") != std::string::npos) return false;
            Block b = doc.make("mov");
            b.set("src", src);
            b.set("dst", dst);
            return done(b);
        }
        if (u == "ALU") {
            std::istringstream ts(rest);
            std::vector<std::string> w;
            for (std::string x; ts >> x;) w.push_back(x);
            if (w.empty() || w.size() > 3) return false;
            Block b = doc.make("alu");
            b.set("op", w[0]);
            std::string amount, mode;
            for (size_t i = 1; i < w.size(); i++) {
                const std::string lw = lower(w[i]);
                if (lw == "signed" || lw == "unsigned") { if (!mode.empty()) return false; mode = lw; }
                else if (std::isdigit((unsigned char)w[i][0])) { if (!amount.empty() || !mode.empty()) return false; amount = w[i]; }
                else return false;
            }
            b.set("amount", amount);
            b.set("mode", mode);
            return done(b);
        }
        if (u == "EXEC_CSR") {
            size_t sp2 = 0;
            while (sp2 < rest.size() && !std::isspace((unsigned char)rest[sp2])) sp2++;
            const std::string sub = upper(rest.substr(0, sp2)), tail = trim(rest.substr(sp2));
            if (sub == "WRITE8" || sub == "SET8" || sub == "CLR8") {
                std::vector<std::string> x = args(tail);
                if (x.size() != 2 || x[0].empty() || x[1].empty()) return false;
                Block b = doc.make(sub == "WRITE8" ? "csr_write8" : sub == "SET8" ? "csr_set8" : "csr_clr8");
                b.set("csr", x[0]);
                b.set("imm", x[1]);
                return done(b);
            }
            if (sub == "READ8") {
                size_t arrow = findTop(tail, "->");
                if (arrow == std::string::npos) return false;
                std::string csr = trim(tail.substr(0, arrow)), dst = trim(tail.substr(arrow + 2));
                if (csr.empty() || dst.empty()) return false;
                Block b = doc.make("csr_read8");
                b.set("csr", csr);
                b.set("dst", dst);
                return done(b);
            }
            if (sub == "WRITE_SP" || sub == "READ_BUF") {
                std::vector<std::string> x = args(tail);
                std::map<std::string, std::string> kw;
                if (x.size() != 3 || x[0].empty() || !keywords(x, 1, {"len", "ptr"}, kw)) return false;
                Block b = doc.make(sub == "WRITE_SP" ? "csr_write_sp" : "csr_read_buf");
                b.set("csr", x[0]);
                b.set("len", kw["len"]);
                b.set("ptr", kw["ptr"]);
                return done(b);
            }
            return false;
        }
        if (u == "BR.COND") {
            if (a.size() != 2 && a.size() != 3) return false;
            if (a[0].find('=') != std::string::npos || a[1].empty() || a[0].empty()) return false;       // the named form cond_sel=..., target=... stays text
            bool safe = false;
            if (a.size() == 3) { if (lower(a[2]) != "safe") return false; safe = true; }
            Block b = doc.make("br_cond");
            b.set("cond", a[0]);
            b.set("target", a[1]);
            b.set("safe", safe ? "1" : "");
            return done(b);
        }
        if (u == "JMP" || (u.compare(0, 3, "BR.") == 0 && u != "BR.ERET" && u != "BR.COND")) {
            if (a.size() != 1 && a.size() != 2) return false;
            if (a[0].empty() || a[0].find('=') != std::string::npos) return false;
            bool safe = false;
            if (a.size() == 2) { if (lower(a[1]) != "safe") return false; safe = true; }
            if (u == "JMP") {
                Block b = doc.make("jmp");
                b.set("target", a[0]);
                b.set("safe", safe ? "1" : "");
                return done(b);
            }
            Block b = doc.make("br_peri");
            b.set("cond", u.substr(3));
            b.set("target", a[0]);
            b.set("safe", safe ? "1" : "");
            return done(b);
        }
        if (u == "BR.ERET") { if (!rest.empty()) return false; return done(doc.make("br_eret")); }
        if (u == "CASCADE_BEGIN" || u == "CASCADE_END") {
            if (a.size() != 2 || a[0].empty() || a[1].empty()) return false;
            if (u == "CASCADE_BEGIN") { Block b = doc.make("cascade_begin"); b.set("sel", a[0]); b.set("block0", a[1]); return done(b); }
            Block b = doc.make("cascade_end");
            b.set("index", a[0]);
            b.set("exit", a[1]);
            return done(b);
        }
        if (u == "CALL") { if (a.size() != 1 || !isIdent(a[0])) return false; Block b = doc.make("call"); b.set("target", a[0]); return done(b); }
        if (u == "RET") { if (!rest.empty()) return false; return done(doc.make("ret")); }
        if (u == "EXEC_XFER") {
            size_t sp2 = 0;
            while (sp2 < rest.size() && !std::isspace((unsigned char)rest[sp2])) sp2++;
            const std::string rw = upper(rest.substr(0, sp2)), tail = trim(rest.substr(sp2));
            if ((rw != "READ" && rw != "WRITE") || tail.empty()) return false;
            Block b = doc.make("xfer");
            b.set("rw", rw);
            b.set("args", tail);
            return done(b);
        }
        if (u == "EXEC_PUB") {
            std::map<std::string, std::string> kw;
            if (!keywords(a, 0, {"service", "src", "len"}, kw)) return false;
            Block b = doc.make("pub");
            b.set("service", kw["service"]);
            b.set("src", kw["src"]);
            b.set("len", kw["len"]);
            return done(b);
        }
        return false;
    }

    // ---- one directive
    bool directive(const std::string& text, const std::string& comment, std::vector<Block>& produced, bool& consumed) {
        size_t sp = 0;
        while (sp < text.size() && !std::isspace((unsigned char)text[sp])) sp++;
        const std::string op = text.substr(0, sp), rest = trim(text.substr(sp)), l = lower(op);
        consumed = false;
        if (l == ".main" || l == ".peri") {
            if (!rest.empty()) return false;
            startSection(l.substr(1), comment);
            consumed = true;
            return true;
        }
        if (l == ".proc") {
            if (!isIdent(rest) || inProc) return false;
            closeProc();
            proc = doc.make("proc");
            proc.set("name", rest);
            proc.comment = comment;
            inProc = true;
            consumed = true;
            return true;
        }
        if (l == ".endp") {
            if (!rest.empty() || !inProc) return false;
            closeProc();
            consumed = true;
            return true;
        }
        std::vector<std::string> a = args(rest);
        if (l == ".vector") {
            if (a.size() != 2 || a[0].empty() || a[1].empty()) return false;
            Block b = doc.make("vector");
            b.set("id", a[0]);
            b.set("label", a[1]);
            produced.push_back(b);
            return true;
        }
        if (l == ".wcet") {
            if (a.size() != 3 || a[0].empty() || a[1].empty() || a[2].empty()) return false;
            Block b = doc.make("wcet");
            b.set("name", a[0]);
            b.set("from", a[1]);
            b.set("to", a[2]);
            produced.push_back(b);
            return true;
        }
        if (l == ".assume") {
            if (rest.empty()) return false;
            Block b = doc.make("assume");
            b.set("text", rest);
            produced.push_back(b);
            return true;
        }
        Block b = doc.make("directive");
        b.set("name", op);
        b.set("args", rest);
        produced.push_back(b);
        return true;
    }

    void line(const std::string& rawLine) {
        if (stats) stats->lines++;
        std::string code = stripComment(rawLine);
        std::string comment;
        if (code.size() < rawLine.size()) comment = trim(rawLine.substr(code.size() + 1));
        std::string t = trim(code);
        if (t.empty()) {
            Block b = comment.empty() ? doc.make("blank") : doc.make("comment");
            if (!comment.empty()) b.set("text", comment);
            target().push_back(b);
            return;
        }
        std::vector<Block> produced;
        // labels
        for (;;) {
            if (t.empty() || !isIdentStart(t[0])) break;
            size_t i = 0;
            while (i < t.size() && isIdentChar(t[i])) i++;
            size_t j = i;
            while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) j++;
            if (j < t.size() && t[j] == ':' && !(j + 1 < t.size() && t[j + 1] == ':')) {
                Block b = doc.make("label");
                b.set("name", t.substr(0, i));
                produced.push_back(b);
                t = trim(t.substr(j + 1));
            } else break;
        }
        bool consumed = false;
        if (!t.empty()) {
            // "NAME EQU value" (8051 style) stays text
            std::istringstream ts(t);
            std::string x, y;
            ts >> x >> y;
            if (!x.empty() && upper(y) == "EQU" && isIdent(x)) {
                produced.push_back(raw(t));
            } else if (t[0] == '.') {
                if (!directive(t, comment, produced, consumed)) produced.push_back(raw(t));
            } else {
                Block b;
                if (statement(t, b)) produced.push_back(b);
                else produced.push_back(raw(t));
            }
        }
        if (consumed) {
            // .main / .peri / .proc / .endp: labels in front of it (rare) stay where they were
            for (Block& b : produced) target().push_back(b);
            return;
        }
        if (!comment.empty() && !produced.empty()) produced.back().comment = comment;
        for (Block& b : produced) target().push_back(b);
    }
};

}  // namespace

Document importAsm(const std::string& text, const std::string& name, ImportStats* stats, const Registry& reg) {
    Importer im(reg, stats);
    im.doc.name = name;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        im.line(raw);
    }
    im.closeProc();
    // generate() puts one blank line between sections itself: the blank line that ends a section is not a block (so text -> blocks -> text is stable),
    // and the end of the file has no trailing blank lines
    for (size_t i = 0; i < im.doc.sections.size(); i++) {
        Section& s = im.doc.sections[i];
        const bool last = i + 1 == im.doc.sections.size();
        if (last) { while (!s.blocks.empty() && s.blocks.back().type == "blank") s.blocks.pop_back(); }
        else if (!s.blocks.empty() && s.blocks.back().type == "blank") s.blocks.pop_back();
    }
    // Where the stacks lie on the canvas at first: definitions and Main in the left column, Peri in the right one, top to bottom in file order. y
    // strictly increases in file order, so the order the program is generated in (canvas order) is the order of the file. The height is an
    // estimate; the editor's "tidy up" measures the real blocks.
    {
        double colY[2] = {0, 0}, lastY = -1;
        for (Section& s : im.doc.sections) {
            const int col = s.domain == "peri" ? 1 : 0;
            size_t n = 0;
            std::function<void(const std::vector<Block>&)> count = [&](const std::vector<Block>& v) { for (const Block& b : v) { n++; for (auto& st : b.stacks) count(st.second); } };
            count(s.blocks);
            const double h = 48 + 30.0 * (double)n;
            const double y = std::max(colY[col], lastY + 1);
            s.x = col ? 560 : 0;
            s.y = y;
            colY[col] = y + h + 40;
            lastY = y;
        }
    }
    explodeDocument(im.doc, reg);                                      // operands -> components, where they give back the same text
    if (stats) stats->blocks = (int)im.doc.blockCount();
    return im.doc;
}

namespace detail {
std::vector<Block> importFlat(Document& target, const std::string& text, ImportStats* stats) {
    Document d = importAsm(text, "", stats);
    std::vector<Block> out;
    for (Section& s : d.sections) for (Block& b : s.blocks) { reid(b, target); out.push_back(std::move(b)); }
    return out;
}
}  // namespace detail

}  // namespace blocks
}  // namespace asmcore
