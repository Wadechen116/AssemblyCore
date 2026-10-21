// Front end: Ctx helpers, source parsing (labels, directives, procedure structure) and the assemble() driver.
#include <algorithm>
#include <sstream>

#include "internal.h"

namespace asmcore {

// The span a diagnostic points at: the symbol the message quotes ('name') if it occurs on the line, else the statement without its
// labels and comment.
SrcLoc Ctx::withColumns(SrcLoc l, const std::string& msg) const {
    if (l.endCol > 0 || l.line <= 0) return l;
    auto f = srcLines.find(l.file);
    if (f == srcLines.end() || (size_t)l.line > f->second.size()) return l;
    const std::string& raw = f->second[(size_t)l.line - 1];
    std::string code = stripComment(raw);
    size_t b = code.find_first_not_of(" \t");
    if (b == std::string::npos) return l;
    size_t e = code.find_last_not_of(" \t") + 1;
    const size_t b0 = b;
    // skip leading "label:" parts
    for (;;) {
        size_t i = b;
        if (i >= e || !isIdentStart(code[i])) break;
        while (i < e && isIdentChar(code[i])) i++;
        size_t j = i;
        while (j < e && (code[j] == ' ' || code[j] == '\t')) j++;
        if (j < e && code[j] == ':') {
            size_t nb = code.find_first_not_of(" \t", j + 1);
            if (nb == std::string::npos || nb >= e) break;
            b = nb;
        } else break;
    }
    l.col = (int)b;
    l.endCol = (int)e;
    size_t q = msg.find('\'');
    if (q != std::string::npos) {
        size_t q2 = msg.find('\'', q + 1);
        if (q2 != std::string::npos && q2 > q + 1) {
            std::string tok = msg.substr(q + 1, q2 - q - 1);
            size_t at = code.find(tok, b0);
            if (at != std::string::npos && at + tok.size() <= e) { l.col = (int)at; l.endCol = (int)(at + tok.size()); }
        }
    }
    return l;
}

void Ctx::error(const SrcLoc& l, const std::string& code, const std::string& m) {
    Diagnostic d;
    d.sev = Diagnostic::Error;
    d.loc = withColumns(l, m);
    d.code = code;
    d.msg = m;
    res.diags.push_back(d);
}
void Ctx::warn(const SrcLoc& l, const std::string& code, const std::string& m) {
    for (const std::string& s : opt.suppress) if (s == code) return;
    Diagnostic d;
    d.sev = Diagnostic::Warning;
    d.loc = withColumns(l, m);
    d.code = code;
    d.msg = m;
    res.diags.push_back(d);
}

bool Ctx::eval(const std::string& text, int64_t& v, const SrcLoc& l, const char* code) {
    std::string err;
    auto lookup = [&](const std::string& n, int64_t& out) {
        auto it = syms.find(n);
        if (it == syms.end()) return false;
        out = it->second.value;
        return true;
    };
    if (!evalExpr(text, lookup, v, err)) {
        error(l, code, err + " in '" + trim(text) + "'");
        return false;
    }
    return true;
}

bool Ctx::defineSym(const std::string& name, const std::string& kind, int64_t v, int size, const SrcLoc& l) {
    if (!isIdent(name)) { error(l, "E105", "'" + name + "' is not a valid symbol name"); return false; }
    auto it = syms.find(name);
    if (it != syms.end()) {
        error(l, "E104", "symbol '" + name + "' is already defined (" + it->second.kind + ", " + it->second.loc.file + ":" + std::to_string(it->second.loc.line) + ")");
        return false;
    }
    Sym s;
    s.kind = kind;
    s.value = v;
    s.size = size;
    s.loc = l;
    syms[name] = s;
    return true;
}

int Ctx::constOffsetFor(int64_t value, int width) {
    auto key = std::make_pair(value, width);
    auto it = autoConst.find(key);
    if (it != autoConst.end()) return it->second;
    if ((int)constPool.size() + width > opt.profile.constPoolMax) return -1;
    int off = (int)constPool.size();
    for (int i = 0; i < width; i++) constPool.push_back((uint8_t)((value >> (8 * i)) & 0xFF));
    autoConst[key] = off;
    return off;
}

namespace {

struct Front {
    Ctx& c;
    int domain = -1;           // 0 Main, 1 Peri, -1 none yet
    ProcDef* proc = nullptr;
    bool ended = false;
    std::set<std::string> flowLabels[2];
    std::set<std::string> procNames[2];

    explicit Front(Ctx& cc) : c(cc) {}

    void addLabel(Stmt& st, const std::string& name) {
        if (proc) {
            if (proc->localLabels.count(name)) c.error(st.loc, "E104", "label '" + name + "' is already defined in procedure '" + proc->name + "'");
            proc->localLabels.insert(name);
        } else {
            if (flowLabels[domain].count(name) || procNames[domain].count(name)) c.error(st.loc, "E104", "label '" + name + "' is already defined");
            flowLabels[domain].insert(name);
        }
        st.labels.push_back(name);
    }

    void directive(const Stmt& st, const std::string& d, const std::string& rest, int depth, const std::string& fileName);
    void line(const std::string& file, int no, const std::string& raw, int depth);
    void file(const Source& src, int depth);
};

void Front::file(const Source& src, int depth) {
    if (depth > 16) {
        c.error(SrcLoc{src.name, 0}, "E106", "includes nested too deeply (a file includes itself?)");
        return;
    }
    std::istringstream in(src.text);
    std::string raw;
    int n = 0;
    std::vector<std::string>& keep = c.srcLines[src.name];
    keep.clear();
    while (!ended && std::getline(in, raw)) {
        n++;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        keep.push_back(raw);
        line(src.name, n, raw, depth);
    }
}

void Front::line(const std::string& file, int no, const std::string& raw, int depth) {
    std::string text = trim(stripComment(raw));
    if (text.empty()) return;
    Stmt st;
    st.loc = SrcLoc{file, no};

    // labels:  name:  (possibly several)
    for (;;) {
        size_t i = 0;
        if (text.empty() || !isIdentStart(text[0])) break;
        while (i < text.size() && isIdentChar(text[i])) i++;
        size_t j = i;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) j++;
        if (j < text.size() && text[j] == ':' && !(j + 1 < text.size() && text[j + 1] == ':')) {
            std::string name = text.substr(0, i);
            if (domain < 0) c.error(st.loc, "E105", "a label before any '.main' / '.peri' directive");
            else addLabel(st, name);
            text = trim(text.substr(j + 1));
        } else break;
    }

    // "NAME EQU expr"  (8051 style)
    {
        std::istringstream ts(text);
        std::string a, b;
        ts >> a >> b;
        if (!a.empty() && upper(b) == "EQU" && isIdent(a)) {
            std::string expr = trim(text.substr(text.find(b, a.size()) + b.size()));
            int64_t v = 0;
            if (c.eval(expr, v, st.loc)) c.defineSym(a, "equ", v, 0, st.loc);
            return;
        }
    }

    if (text.empty()) {
        if (!st.labels.empty()) {
            (proc ? proc->body : c.prog[domain].flow).push_back(st);
        }
        return;
    }
    size_t sp = 0;
    while (sp < text.size() && !std::isspace((unsigned char)text[sp])) sp++;
    st.op = text.substr(0, sp);
    st.rest = trim(text.substr(sp));

    if (st.op[0] == '.') {
        directive(st, lower(st.op), st.rest, depth, file);
        if (!st.labels.empty() && domain >= 0) {
            Stmt lab = st;
            lab.op.clear();
            lab.rest.clear();
            (proc ? proc->body : c.prog[domain].flow).push_back(lab);
        }
        return;
    }
    if (domain < 0) {
        c.error(st.loc, "E105", "instruction before any '.main' / '.peri' directive");
        return;
    }
    if (upper(st.op) == "CALL" && proc) {
        std::string callee = trim(st.rest);
        proc->callees.push_back(callee);
    }
    (proc ? proc->body : c.prog[domain].flow).push_back(st);
}

void Front::directive(const Stmt& st, const std::string& d, const std::string& rest, int depth, const std::string& fileName) {
    const SrcLoc& loc = st.loc;
    auto needDomain = [&]() {
        if (domain < 0) { c.error(loc, "E105", "'" + d + "' needs a preceding '.main' or '.peri'"); return false; }
        return true;
    };
    if (d == ".main" || d == ".peri") {
        if (proc) { c.error(loc, "E105", "'" + d + "' inside procedure '" + proc->name + "' (missing .endp?)"); return; }
        domain = d == ".main" ? 0 : 1;
        c.prog[domain].present = true;
    } else if (d == ".end") {
        ended = true;
    } else if (d == ".org") {
        c.error(loc, "E105", "'.org' is not supported: the program counter starts at 0 and the image layout is fixed (spec 3.1)");
    } else if (d == ".include") {
        std::string name = trim(rest);
        if (name.size() >= 2 && (name[0] == '"' || name[0] == '<')) name = name.substr(1, name.size() - 2);
        Source inc;
        if (!(c.resolver && c.resolver(fileName, name, inc))) { c.error(loc, "E106", "cannot include '" + name + "'"); return; }
        file(inc, depth + 1);
    } else if (d == ".equ") {
        std::vector<std::string> a;
        size_t eq = rest.find('=');
        if (eq != std::string::npos && rest.find(',') == std::string::npos) a = {trim(rest.substr(0, eq)), trim(rest.substr(eq + 1))};
        else a = splitTop(rest);
        int64_t v;
        if (a.size() != 2) { c.error(loc, "E102", ".equ NAME = expression   (or: .equ NAME, expression)"); return; }
        if (c.eval(a[1], v, loc)) c.defineSym(a[0], "equ", v, 0, loc);
    } else if (d == ".csr" || d == ".service") {
        // .csr NAME = 0x123      .service NAME = 1
        size_t eq = rest.find('=');
        if (eq == std::string::npos) { c.error(loc, "E102", d + " NAME = value"); return; }
        std::string name = trim(rest.substr(0, eq));
        int64_t v;
        std::vector<std::string> opts;
        std::string valueText = rest.substr(eq + 1);
        if (d == ".service") {                                 // .service NAME = id [, len=N] [, big|little]
            opts = splitTop(valueText);
            valueText = opts.empty() ? "" : opts[0];
        }
        if (!c.eval(valueText, v, loc)) return;
        if (d == ".csr") {
            if (v < 0 || v > 0xFFF) { c.error(loc, "E201", "CSR address " + std::to_string(v) + " does not fit 12 bits"); return; }
            c.defineSym(name, "csr", v, 0, loc);
        } else {
            if (v < 1 || v > 15) { c.error(loc, "E201", "service_id must be 1..15 (0 is reserved, spec 10.3)"); return; }
            if (!c.defineSym(name, "service", v, 0, loc)) return;
            Ctx::ServiceDecl sd;
            sd.name = name;
            sd.loc = loc;
            for (size_t k = 1; k < opts.size(); k++) {
                std::string o = lower(trim(opts[k]));
                if (o == "big" || o == "big_endian" || o == "msb_first") sd.wire = 'B';
                else if (o == "little" || o == "little_endian" || o == "lsb_first") sd.wire = 'L';
                else if (o.compare(0, 4, "len=") == 0) {
                    int64_t n;
                    if (!c.eval(o.substr(4), n, loc)) return;
                    if (n < 1 || n > 32) { c.error(loc, "E201", "service payload length must be 1..32 bytes (the ResultSlot holds 32)"); return; }
                    sd.len = (int)n;
                } else { c.error(loc, "E102", ".service NAME = id [, len=N] [, big|little]   ('" + trim(opts[k]) + "' is not an option)"); return; }
            }
            c.services[(int)v] = sd;
        }
    } else if (d == ".const") {
        // .const NAME db 1, 2, 3      .const NAME dw 100, 200
        std::istringstream ts(rest);
        std::string name, w;
        ts >> name >> w;
        std::string vals = trim(rest.substr(rest.find(w, name.size()) + w.size()));
        std::string lw = lower(w);
        int width = lw == "db" ? 1 : lw == "dw" ? 2 : 0;
        if (name.empty() || !width) { c.error(loc, "E102", ".const NAME db|dw value, ..."); return; }
        std::vector<uint8_t> bytes;
        for (const std::string& e : splitTop(vals)) {
            int64_t v;
            if (!c.eval(e, v, loc)) return;
            int64_t lo = width == 1 ? -128 : -32768, hi = width == 1 ? 255 : 65535;
            if (v < lo || v > hi) { c.error(loc, "E201", "value " + std::to_string(v) + " does not fit " + (width == 1 ? "a byte" : "16 bits")); return; }
            for (int i = 0; i < width; i++) bytes.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
        }
        if (bytes.empty()) { c.error(loc, "E102", ".const needs at least one value"); return; }
        if ((int)(c.constPool.size() + bytes.size()) > c.opt.profile.constPoolMax) { c.error(loc, "E308", "MAIN_CONST_POOL is full"); return; }
        int off = (int)c.constPool.size();
        if (c.defineSym(name, "const", off, (int)bytes.size(), loc)) c.constPool.insert(c.constPool.end(), bytes.begin(), bytes.end());
    } else if (d == ".devmap") {
        // .devmap index, I2C, addr7 [, name]
        auto a = splitTop(rest);
        if (a.size() < 3 || a.size() > 4) { c.error(loc, "E102", ".devmap index, I2C, addr7 [, name]"); return; }
        int64_t idx, addr;
        if (!c.eval(a[0], idx, loc) || !c.eval(a[2], addr, loc)) return;
        if (upper(a[1]) != "I2C") { c.error(loc, "E206", "bus_type '" + a[1] + "' is not supported (baseline: I2C only, spec 9.1)"); return; }
        if (idx < 0 || idx >= c.opt.profile.deviceMapDepth || idx > 7) { c.error(loc, "E201", "DeviceMap index must be 0.." + std::to_string(std::min(7, c.opt.profile.deviceMapDepth - 1))); return; }
        if (addr < 0 || addr > 0x7F) { c.error(loc, "E201", "i2c_addr_7b must be 0..0x7F"); return; }
        if (c.dev[idx].defined) { c.error(loc, "E104", "DeviceMap[" + std::to_string(idx) + "] is already defined"); return; }
        c.dev[idx].defined = true;
        c.dev[idx].bus = isa::BUS_I2C;
        c.dev[idx].addr = (int)addr;
        c.dev[idx].loc = loc;
        if (a.size() == 4) c.defineSym(a[3], "devmap", idx, 0, loc);
    } else if (d == ".csrbit") {
        // .csrbit CSRNAME[bit] = timer_start | timer_clear | poll_arm | poll_clear
        size_t lb = rest.find('['), rb = rest.find(']'), eq = rest.find('=');
        if (lb == std::string::npos || rb == std::string::npos || eq == std::string::npos || rb < lb || eq < rb) { c.error(loc, "E102", ".csrbit CSRNAME[bit] = timer_start | timer_clear | poll_arm | poll_clear"); return; }
        int64_t addr = 0, bit = 0;
        std::string role = lower(trim(rest.substr(eq + 1)));
        if (!c.eval(rest.substr(0, lb), addr, loc) || !c.eval(rest.substr(lb + 1, rb - lb - 1), bit, loc)) return;
        if (role != "timer_start" && role != "timer_clear" && role != "poll_arm" && role != "poll_clear") { c.error(loc, "E102", "bit role '" + role + "': use timer_start, timer_clear, poll_arm or poll_clear"); return; }
        if (addr < 0 || addr > 0xFFF || bit < 0 || bit > 7) { c.error(loc, "E201", ".csrbit: CSR address 0..0xFFF and bit 0..7"); return; }
        c.csrBits.push_back({(uint32_t)addr, (int)bit, role, loc});
    } else if (d == ".vector") {
        // .vector EVENT_ID, label     (the handler entry of this domain; the handler must end in BR.ERET)
        if (!needDomain()) return;
        auto a = splitTop(rest);
        int64_t id;
        if (a.size() != 2 || !isIdent(trim(a[1]))) { c.error(loc, "E102", ".vector EVENT_ID, label   (the label of a handler in this domain, e.g. '.vector 0, on_frame')"); return; }
        if (!c.eval(a[0], id, loc)) return;
        const int n = c.opt.profile.eventCount;
        if (id < 0 || id >= n) { c.error(loc, "E315", "event id " + std::to_string(id) + " is outside 0.." + std::to_string(n - 1) + " (profile event_count = " + std::to_string(n) + ")"); return; }
        for (const Ctx::VecDecl& v : c.vectors[domain])
            if (v.id == id) { c.error(loc, "E104", "event " + std::to_string(id) + " already has a vector in this domain (line " + std::to_string(v.loc.line) + ")"); return; }
        c.vectors[domain].push_back({(int)id, trim(a[1]), loc});
    } else if (d == ".proc") {
        if (!needDomain()) return;
        if (proc) { c.error(loc, "E105", "procedures do not nest ('" + proc->name + "' is still open)"); return; }
        std::string name = trim(rest);
        if (!isIdent(name)) { c.error(loc, "E102", ".proc NAME"); return; }
        if (procNames[domain].count(name) || flowLabels[domain].count(name)) { c.error(loc, "E104", "procedure '" + name + "' is already defined"); return; }
        procNames[domain].insert(name);
        ProcDef pd;
        pd.name = name;
        pd.loc = loc;
        c.prog[domain].procs.push_back(pd);
        proc = &c.prog[domain].procs.back();
    } else if (d == ".endp") {
        if (!proc) { c.error(loc, "E105", "'.endp' without '.proc'"); return; }
        proc = nullptr;
    } else if (d == ".assume") {
        // .assume MAIN_SCRATCHPAD[16..17] defined      (also MAIN_CSR_DATA_BUF[..] in Main, PERI_RX_BUF[..] in Peri)
        if (!needDomain()) return;
        size_t lb = rest.find('[');
        size_t rb = rest.find(']');
        std::string tail = rb == std::string::npos ? "" : lower(trim(rest.substr(rb + 1)));
        if (lb == std::string::npos || rb == std::string::npos || tail != "defined") { c.error(loc, "E102", ".assume REGION[a..b] defined"); return; }
        std::string reg = upper(trim(rest.substr(0, lb)));
        std::string in = rest.substr(lb + 1, rb - lb - 1);
        size_t dd = in.find("..");
        int64_t a = 0, b = 0;
        if (!c.eval(dd == std::string::npos ? in : in.substr(0, dd), a, loc) || !c.eval(dd == std::string::npos ? in : in.substr(dd + 2), b, loc)) return;
        const char* canon = nullptr;
        if (domain == 0 && (reg == "MAIN_SCRATCHPAD" || reg == "SCRATCHPAD" || reg == "SCRATCH")) canon = "scratch";
        else if (domain == 0 && (reg == "MAIN_CSR_DATA_BUF" || reg == "CSR_DATA_BUF" || reg == "CSRBUF")) canon = "csrbuf";
        else if (domain == 1 && (reg == "PERI_RX_BUF" || reg == "RX_BUF" || reg == "RX")) canon = "rx";
        if (!canon) { c.error(loc, "E102", ".assume: region '" + reg + "' is not data of this domain (MAIN_SCRATCHPAD, MAIN_CSR_DATA_BUF, PERI_RX_BUF)"); return; }
        if (a < 0 || b < a || b > 31) { c.error(loc, "E201", ".assume: bad byte range"); return; }
        c.assumes.push_back(Ctx::Assume{domain, canon, (int)a, (int)(b - a + 1), loc});
    } else if (d == ".wcet") {
        auto a = splitTop(rest);
        if (a.size() != 3) { c.error(loc, "E102", ".wcet NAME, start_label, end_label"); return; }
        c.wcetDecls.push_back(Ctx::WcetDecl{a[0], a[1], a[2], loc});
    } else {
        c.error(loc, "E101", "unknown directive '" + d + "'");
    }
}

}  // namespace

void parseSources(Ctx& c, const std::vector<Source>& sources) {
    Front f(c);   // one front end for all files, so duplicate labels across files are caught
    for (const Source& s : sources) {
        f.domain = -1;
        f.ended = false;
        f.file(s, 0);
        if (f.proc) c.error(SrcLoc{s.name, 0}, "E105", "procedure '" + f.proc->name + "' has no '.endp'");
    }
}

}  // namespace asmcore
