#include "instr_params.h"

#include <algorithm>
#include <cctype>

namespace acide {

namespace {

void addOnce(std::vector<std::string>& v, const std::string& k) {
    if (std::find(v.begin(), v.end(), k) == v.end()) v.push_back(k);
}

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

bool has(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }

}  // namespace

InstrParams instructionParams(const std::string& lineIn, int domain) {
    InstrParams p;
    std::string line = lineIn.substr(0, lineIn.find(';'));                   // no comment
    // no labels in front ("loop:  MOV ..."): a word ending in ':' before the mnemonic
    for (;;) {
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) return p;
        size_t b = line.find_first_of(" \t", a);
        const std::string w = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!w.empty() && w.back() == ':') { line = b == std::string::npos ? std::string() : line.substr(b); continue; }
        line = line.substr(a);
        break;
    }
    if (line.empty() || line[0] == '.') return p;                           // a directive
    const size_t sp = line.find_first_of(" \t");
    const std::string m = upper(line.substr(0, sp));
    const std::string rest = upper(sp == std::string::npos ? std::string() : line.substr(sp));
    const std::string dom = domain == 1 ? "peri" : "main";
    auto own = [&](const char* k) { addOnce(p.own, k); };
    auto rel = [&](const std::string& k) { addOnce(p.related, k); };
    bool known = true;
    if (m == "MOV") {
        if (has(rest, "PERI_RESULT_SLOT")) { own("latency.slot_consume_setup"); own("latency.mov_byte"); rel("datapath.contention"); rel("datapath.contention_cycles"); rel("slot.valid_delay"); }
        else { own("latency.mov_setup"); own("latency.mov_byte"); }
    } else if (m == "ALU") {
        own(has(rest, "MUL") ? "latency.mul" : "latency.alu");
    } else if (m == "EXEC_CSR" || m.compare(0, 4, "CSR.") == 0) {
        const bool multi = has(rest, "WRITE_SP") || has(rest, "READ_BUF") || m == "CSR.WRITE_SP" || m == "CSR.READ_BUF";
        if (multi) { own("latency.csr_byte"); own("latency.csr_done"); }
        else { own("latency.csr_access"); own(domain == 1 ? "csr.access_peri" : "csr.access_main"); }
        rel("csr.arbiter_policy"); rel("csr.arbiter_penalty");
    } else if (m == "EXEC_PUB" || m == "PUB") {
        own("latency.pub_setup"); own("latency.pub_byte"); own("slot.copy_extra");
        rel("datapath.contention"); rel("datapath.contention_cycles"); rel("slot.valid_delay");
    } else if (m == "EXEC_XFER" || m.compare(0, 5, "XFER.") == 0) {
        own("latency.xfer_issue");
        rel("clock.i2c_hz"); rel("i2ch.overhead"); rel("i2c.ack_delay"); rel("i2c.stretch"); rel("i2c.turnaround");
        if (domain == 1) { rel("datapath.contention"); rel("datapath.contention_cycles"); }
    } else if (m == "BR.ERET") {
        own("latency.eret"); own("event.eret_extra");
    } else if (m == "CASCADE_BEGIN" || m == "CASCADE_END") {
        own("latency.cascade");
    } else if (m == "JMP" || m == "CALL" || m == "RET" || m.compare(0, 3, "BR.") == 0 || m == "BR") {
        own("latency.br_cond");
        const bool conditional = m != "JMP" && m != "CALL" && m != "RET" && !has(rest, "ALWAYS");
        if (conditional) {
            rel("flag.arbiter_policy"); rel("flag.access_cycles"); rel("flag.arbiter_penalty"); rel("flag.sample_delay_" + dom);
        }
        if (has(rest, "SAFE")) { rel("latency.event_dispatch"); rel("event.dispatch_extra"); }        // an event checkpoint: where a pending event is dispatched
    } else {
        known = false;
    }
    if (known) {
        p.mnemonics.push_back(m);
        rel("fetch.read_mode");                                               // every instruction is read from the I2C EEPROM first
        rel(dom + ".fetch_cycles");                                           // every instruction of that decoder
    }
    return p;
}

InstrParams instructionParams(const std::vector<std::string>& lines, int domain) {
    InstrParams all;
    for (const std::string& l : lines) {
        const InstrParams p = instructionParams(l, domain);
        for (const std::string& k : p.own) addOnce(all.own, k);
        for (const std::string& k : p.related) addOnce(all.related, k);
        for (const std::string& k : p.mnemonics) addOnce(all.mnemonics, k);
    }
    // a parameter that is the instruction's own for one line is not "related" for another
    all.related.erase(std::remove_if(all.related.begin(), all.related.end(), [&](const std::string& k) { return std::find(all.own.begin(), all.own.end(), k) != all.own.end(); }), all.related.end());
    return all;
}

const std::vector<std::string>& globalParamKeys() {
    static const std::vector<std::string> k = {"clock.cpu_hz", "sim.speed", "sim.tie_order", "sim.fault_stops"};
    return k;
}

}  // namespace acide
