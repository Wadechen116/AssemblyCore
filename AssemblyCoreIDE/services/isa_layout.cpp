#include "isa_layout.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace acide {

namespace {

// Words of a line: separated by spaces, "quoted text" is one word (without the quotes); key="a b" is one word "key=a b".
std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> v;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == ' ' || c == '\t' || c == '\r')) {
            if (any) { v.push_back(cur); cur.clear(); any = false; }
            continue;
        }
        if (!quoted && c == '#') break;
        cur += c;
        any = true;
    }
    if (any) v.push_back(cur);
    return v;
}

bool number(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && !*end;
}

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

const std::set<std::string>& kinds() {
    static const std::set<std::string> k = {"code", "decoder", "data", "alu", "flags", "timer", "shared", "device"};
    return k;
}
const std::set<std::string>& regionNames() {
    static const std::set<std::string> k = {"scratch", "csrbuf", "rx", "slot", "meta", "opa", "opb", "out", "status", "const"};
    return k;
}

bool parseSource(const std::vector<std::string>& w, size_t from, IsaSource& s, std::string& err) {
    if (from >= w.size()) { err = "a row needs a source"; return false; }
    const std::string k = w[from];
    auto domain = [&](size_t i, int& d) {
        if (i >= w.size() || (w[i] != "main" && w[i] != "peri")) { err = k + " needs main or peri"; return false; }
        d = w[i] == "main" ? 0 : 1;
        return true;
    };
    if (k == "pc" || k == "instr" || k == "count" || k == "fault") {
        s.kind = k == "pc" ? IsaSource::Pc : k == "instr" ? IsaSource::Instr : k == "count" ? IsaSource::Count : IsaSource::Fault;
        return domain(from + 1, s.domain);
    }
    if (k == "cycle") { s.kind = IsaSource::Cycle; return true; }
    if (k == "text") {
        s.kind = IsaSource::Text;
        s.text = from + 1 < w.size() ? w[from + 1] : std::string();
        return true;
    }
    if (k == "signal" || k == "flag") {
        if (from + 1 >= w.size()) { err = k + " needs a name"; return false; }
        s.kind = k == "signal" ? IsaSource::Signal : IsaSource::Flag;
        s.name = w[from + 1];
        return true;
    }
    if (k == "csrn") {
        double n;
        if (from + 1 >= w.size() || !number(w[from + 1], n) || n < 0) { err = "csrn needs an index"; return false; }
        s.kind = IsaSource::Csrn;
        s.from = (int)n;
        return true;
    }
    if (k == "region") {
        if (from + 2 >= w.size() || !regionNames().count(w[from + 1])) { err = "region needs a name (" + std::string("scratch csrbuf rx slot meta opa opb out status const") + ") and A..B"; return false; }
        const std::string r = w[from + 2];
        const size_t dots = r.find("..");
        double a = 0, b = 0;
        if (dots == std::string::npos || !number(r.substr(0, dots), a) || !number(r.substr(dots + 2), b) || a < 0 || b < a) { err = "region needs A..B"; return false; }
        s.kind = IsaSource::Region;
        s.name = w[from + 1];
        s.from = (int)a;
        s.to = (int)b;
        return true;
    }
    err = "unknown source '" + k + "'";
    return false;
}

}  // namespace

const IsaBox* IsaLayout::box(const std::string& id) const {
    for (const IsaBox& b : boxes) if (b.id == id) return &b;
    return nullptr;
}
const IsaWire* IsaLayout::wire(const std::string& id) const {
    for (const IsaWire& w : wires) if (w.id == id) return &w;
    return nullptr;
}

std::string IsaLayout::mnemonicOf(int domain, const std::string& text) const {
    if (domain < 0 || domain > 1) return std::string();
    const std::string t = upper(text);
    std::string best;
    for (const auto& p : paths[(size_t)domain])
        if (t.compare(0, p.first.size(), p.first) == 0 && p.first.size() > best.size()) best = p.first;
    return best;
}

std::vector<std::string> IsaLayout::wiresFor(int domain, const std::string& text) const {
    const std::string m = mnemonicOf(domain, text);
    if (!m.empty())
        for (const auto& p : paths[(size_t)domain]) if (p.first == m) return p.second;
    return {};
}

IsaLayout parseIsaLayout(const std::string& text) {
    IsaLayout L;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    auto bad = [&](const std::string& why) { L.errors.push_back("line " + std::to_string(n) + ": " + why); };
    while (std::getline(in, line)) {
        n++;
        const std::vector<std::string> w = words(line);
        if (w.empty()) continue;
        const std::string& cmd = w[0];
        if (cmd == "canvas") {
            double a, b;
            if (w.size() < 3 || !number(w[1], a) || !number(w[2], b) || a < 100 || b < 100) { bad("canvas needs W H"); continue; }
            L.width = a;
            L.height = b;
        } else if (cmd == "box") {
            IsaBox b;
            double x, y, bw, bh;
            if (w.size() < 7 || !number(w[2], x) || !number(w[3], y) || !number(w[4], bw) || !number(w[5], bh) || bw <= 0 || bh <= 0) { bad("box needs ID X Y W H KIND \"Title\""); continue; }
            b.id = w[1]; b.x = x; b.y = y; b.w = bw; b.h = bh; b.kind = w[6];
            if (!kinds().count(b.kind)) { bad("unknown box kind '" + b.kind + "'"); continue; }
            if (L.box(b.id)) { bad("box '" + b.id + "' is defined twice"); continue; }
            b.title = w.size() > 7 ? w[7] : b.id;
            for (size_t i = 8; i < w.size(); i++) {
                const size_t eq = w[i].find('=');
                if (eq == std::string::npos) { bad("'" + w[i] + "' is not key=value"); continue; }
                const std::string k = w[i].substr(0, eq), v = w[i].substr(eq + 1);
                if (k == "domain") b.domain = v == "main" ? 0 : v == "peri" ? 1 : -1;
                else if (k == "target") b.target = v;
                else if (k == "help") b.help = v;
                else bad("unknown box option '" + k + "'");
            }
            L.boxes.push_back(b);
        } else if (cmd == "row") {
            if (w.size() < 4) { bad("row needs BOX \"Label\" SOURCE"); continue; }
            IsaBox* box = nullptr;
            for (IsaBox& b : L.boxes) if (b.id == w[1]) box = &b;
            if (!box) { bad("row of an unknown box '" + w[1] + "'"); continue; }
            IsaRow r;
            r.label = w[2];
            std::string err;
            if (!parseSource(w, 3, r.src, err)) { bad(err); continue; }
            box->rows.push_back(r);
        } else if (cmd == "wire") {
            if (w.size() < 4) { bad("wire needs ID \"Label\" X,Y X,Y"); continue; }
            IsaWire wr;
            wr.id = w[1];
            wr.label = w[2];
            bool ok = true;
            for (size_t i = 3; i < w.size() && ok; i++) {
                const size_t comma = w[i].find(',');
                double x, y;
                if (comma == std::string::npos || !number(w[i].substr(0, comma), x) || !number(w[i].substr(comma + 1), y)) { bad("'" + w[i] + "' is not X,Y"); ok = false; break; }
                wr.points.push_back({x, y});
            }
            if (!ok) continue;
            if (wr.points.size() < 2) { bad("a wire needs two points"); continue; }
            if (L.wire(wr.id)) { bad("wire '" + wr.id + "' is defined twice"); continue; }
            L.wires.push_back(wr);
        } else if (cmd == "path") {
            if (w.size() < 5 || (w[1] != "main" && w[1] != "peri") || w[3] != "=") { bad("path needs main|peri MNEMONIC = WIRE..."); continue; }
            std::vector<std::string> ids(w.begin() + 4, w.end());
            L.paths[w[1] == "main" ? 0 : 1].push_back({upper(w[2]), ids});
        } else {
            bad("unknown line '" + cmd + "'");
        }
    }
    for (int d = 0; d < 2; d++)                                   // a path may only name wires that exist
        for (const auto& p : L.paths[(size_t)d])
            for (const std::string& id : p.second)
                if (!L.wire(id)) L.errors.push_back("path " + std::string(d == 0 ? "main " : "peri ") + p.first + ": unknown wire '" + id + "'");
    return L;
}

std::string isaValue(const IsaSource& s, const SimView* v, const std::function<std::string(int, uint32_t)>& instrAt) {
    char buf[64];
    if (s.kind == IsaSource::Text) return s.text;
    if (!v) return "-";
    switch (s.kind) {
        case IsaSource::Pc: std::snprintf(buf, sizeof buf, "0x%04X", v->pc[s.domain]); return buf;
        case IsaSource::Instr: { const std::string t = instrAt ? instrAt(s.domain, v->pc[s.domain]) : std::string(); return t.empty() ? "-" : t; }
        case IsaSource::Count: return std::to_string((unsigned long long)v->instrs[s.domain]);
        case IsaSource::Fault: return v->hasFault[s.domain] ? v->fault[s.domain].code : std::string("none");
        case IsaSource::Cycle: return std::to_string((unsigned long long)v->cycle);
        case IsaSource::Region: {
            const auto it = v->regions.find(s.name);
            if (it == v->regions.end()) return "-";
            std::string out;
            for (int i = s.from; i <= s.to && i < (int)it->second.size(); i++) {
                std::snprintf(buf, sizeof buf, "%s%02X", out.empty() ? "" : " ", it->second[(size_t)i]);
                out += buf;
            }
            return out.empty() ? "-" : out;
        }
        case IsaSource::Signal:
        case IsaSource::Flag:
            for (const auto& kv : v->signalValues)
                if (kv.first == s.name) return s.kind == IsaSource::Flag ? std::string(kv.second ? "\xE2\x97\x8F on" : "\xE2\x97\x8B off") : std::to_string((long long)kv.second);
            return "-";
        case IsaSource::Csrn:
            if ((size_t)s.from < v->csrs.size()) { std::snprintf(buf, sizeof buf, "=0x%02X", v->csrs[(size_t)s.from].second); return v->csrs[(size_t)s.from].first + buf; }
            return "-";
        default: return "-";
    }
}

const std::string& defaultIsaLayoutText() {
    static const std::string text = R"ISA(
# ISA Top, as the design drawing shows it: Main (the Main-instruction Decoder with its code, scratchpad, ALU, status flags, and the Frame Timing + Timer that
# release it), Peri (the Peri-instruction Decoder with its code, Timer, polling, rx_buf and the result slot to Main) and what both share (CSR Arbiter, CSR,
# Data Xfer, i2ch, i2cm_top, the I2C device).  Format: see services/isa_layout.h.  Every number here is a position on the canvas.
canvas 1200 740

# ---- Main
box main_code   20  20 180  72 code    "ISA Main Code"                 domain=main target=code help="The Main program: assembly, descriptor tables and the constant pool."
row main_code "PC" pc main
row main_code "image" text "tables + pool"
box main_dec   250  20 240 132 decoder "Main-instruction Decoder"      domain=main target=code help="Executes MOV, ALU, EXEC_CSR and BR. Click: the line it is at."
row main_dec "PC" pc main
row main_dec "next" instr main
row main_dec "executed" count main
row main_dec "fault" fault main
row main_dec "does" text "MOV ALU EXEC_CSR BR"
box alu        540  20 230 152 alu     "ALU"                            target=state help="iopa / iopb / oalu and the ALU status."
row alu "OPA" region opa 0..1
row alu "OPB" region opb 0..1
row alu "OUT" region out 0..1
row alu "cmp_true" flag alu.cmp_true
row alu "zero" flag alu.zero
row alu "negative" flag alu.negative
box scratch    250 190 240 112 data    "MAIN_SCRATCHPAD"               target=memory help="The Main scratchpad (32 bytes). Click: the Memory panel."
row scratch "[0..7]" region scratch 0..7
row scratch "[8..15]" region scratch 8..15
row scratch "[16..23]" region scratch 16..23
row scratch "[24..31]" region scratch 24..31
box flags      540 210 230 152 flags   "Main Status Flag"              target=signals help="The conditions BR can test."
row flags "PERI_RESULT_VALID" flag slot.valid
row flags "VBLANK_OPEN" flag vblank_open
row flags "MAIN_TIMER_DONE" flag main_timer.done
row flags "FRAME_END_EVT" flag frame_end_evt
row flags "PGL_PENDING" flag pgl_pending
row flags "ALU_CMP_TRUE" flag alu.cmp_true
box csrbuf     250 330 240  72 data    "MAIN_CSR_DATA_BUF"             target=memory help="What EXEC_CSR read back."
row csrbuf "[0..3]" region csrbuf 0..3
row csrbuf "[4..7]" region csrbuf 4..7
box frame       20 140 180 112 timer   "Frame Timing WCET"            target=signals help="The frame timing: blanking windows and the WCET registers. With one Timer it releases Main."
row frame "VBLANK_OPEN" flag vblank_open
row frame "vblank left" signal remaining_vblank_cycles
row frame "wcet_pgl" signal wcet_pgl
row frame "wcet_ae" signal wcet_ae
box main_timer  20 290 180  92 timer   "Main Timer"                    target=signals help="Started by EXEC_CSR; its done flag is what BR polls."
row main_timer "period" signal main_timer_period
row main_timer "done" flag main_timer.done
row main_timer "restarts" signal main_timer.gen

# ---- Peri
box peri_code   20 450 180  72 code    "ISA Peri Code"                 domain=peri target=code help="The Peri program."
row peri_code "PC" pc peri
row peri_code "image" text "descriptors"
box peri_dec   250 450 240 152 decoder "Peri-instruction Decoder"      domain=peri target=code help="Executes EXEC_XFER, EXEC_PUB, EXEC_CSR and BR. Click: the line it is at."
row peri_dec "PC" pc peri
row peri_dec "next" instr peri
row peri_dec "executed" count peri
row peri_dec "done" flag peri.done
row peri_dec "error" flag peri.error
row peri_dec "fault" fault peri
box poll       540 440 230 112 flags   "Polling / Status Flag"         target=signals help="Polling of the I2C device status and the Peri Timer."
row poll "poll.ready" flag poll.ready
row poll "poll.arm" flag poll.arm
row poll "poll.timeout" flag poll.timeout
row poll "poll_mask" signal poll_mask
box rx         250 640 240  72 data    "PERI_RX_BUF"                   target=memory help="What EXEC_XFER READ received."
row rx "[0..7]" region rx 0..7
row rx "[8..15]" region rx 8..15
box slot       540 590 230 132 data    "result_slot  (to Main)"        target=memory help="EXEC_PUB copies rx_buf here; Main takes it with MOV."
row slot "valid" flag slot.valid
row slot "busy" flag slot.busy
row slot "overrun" flag slot.overrun
row slot "service_id" signal slot.service_id
row slot "data[0..5]" region slot 0..5
box peri_timer  20 570 180  92 timer   "Peri Timer"                    target=signals help="Started by EXEC_CSR; TIMER_DONE is what Peri's BR polls."
row peri_timer "period" signal timer_period
row peri_timer "done" flag timer.done
row peri_timer "restarts" signal timer.gen

# ---- shared
box arb        820 200 190  72 shared  "Main/Peri CSR Arbiter"         help="Both decoders reach the CSRs through it. (The simulator does not model contention yet: sim parameters csr.arbiter_*)"
row arb "" text "not modelled yet"
box csr       1040 200 140 152 data    "CSR"                           target=csrs help="The CSRs the program names. Click: the CSRs panel."
row csr "" csrn 0
row csr "" csrn 1
row csr "" csrn 2
row csr "" csrn 3
row csr "" csrn 4
box xfer       830 440 150  60 shared  "Data Xfer"                     help="The data path of EXEC_XFER."
box i2ch      1020 440 160  60 shared  "i2ch"                          help="CSR -> i2ch -> i2cm_top: parameter i2ch.overhead."
box i2cm       830 540 150  60 shared  "i2cm_top"                      help="The I2C master: parameters i2c.ack_delay, i2c.stretch, i2c.turnaround."
box dev       1020 540 160  92 device  "I2C device / EEPROM"           help="The device of the environment file ([device ...])."
row dev "bus" text "I2C"
row dev "cycle" cycle

# ---- wires
wire w_main_code   "descriptor"      200,56 250,56
wire w_main_alu    ""                490,86 540,86
wire w_main_scr    "MOV"             370,152 370,190
wire w_scr_alu     ""                490,246 515,246 515,130 540,130
wire w_alu_flags   "status"          655,172 655,210
wire w_flags_main  "BR"              540,300 503,300 503,120 490,120
wire w_main_csr    "EXEC_CSR"        370,20 370,8 800,8 800,236 820,236
wire w_frame_flags "commit / frame"  200,200 225,200 225,425 655,425 655,362
wire w_mtimer      ""                200,336 225,336
wire w_pcode       "descriptor"      200,486 250,486
wire w_poll_peri   "BR"              540,470 490,470
wire w_peri_rx     "READ"            370,602 370,640
wire w_rx_slot     "EXEC_PUB"        490,676 540,676
wire w_peri_bus    ""                490,570 800,570
wire w_bus_arb     "EXEC_CSR"        800,570 800,264 820,264
wire w_bus_xfer    "EXEC_XFER"       800,570 800,470 830,470
wire w_xfer_i2ch   ""                980,470 1020,470
wire w_i2ch_i2cm   ""                1100,500 1100,520 905,520 905,540
wire w_i2cm_dev    "I2C"             980,570 1020,570
wire w_arb_csr     ""                1010,236 1040,236
wire w_slot_main   "to Main"         770,640 785,640 785,290 770,290
wire w_ptimer      "timer"           200,616 225,616 225,540 250,540

# ---- what an instruction lights up
path main MOV       = w_main_code w_main_scr w_scr_alu
path main ALU       = w_main_code w_main_alu w_alu_flags
path main EXEC_CSR  = w_main_code w_main_csr w_arb_csr
path main BR        = w_main_code w_flags_main
path main JMP       = w_main_code
path main CALL      = w_main_code
path main RET       = w_main_code
path main CASCADE   = w_main_code
path peri EXEC_XFER = w_pcode w_peri_bus w_bus_xfer w_xfer_i2ch w_i2ch_i2cm w_i2cm_dev w_peri_rx
path peri EXEC_PUB  = w_pcode w_peri_rx w_rx_slot w_slot_main
path peri EXEC_CSR  = w_pcode w_peri_bus w_bus_arb w_arb_csr
path peri BR        = w_pcode w_poll_peri
path peri JMP       = w_pcode
)ISA";
    return text;
}

}  // namespace acide
