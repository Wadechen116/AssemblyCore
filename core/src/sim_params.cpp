// The simulator's parameter catalog and the text form of a set of values. See include/asmcore/sim_params.h.
#include "asmcore/sim_params.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace asmcore {

namespace {

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string lowered(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::vector<ParamDef> build() {
    std::vector<ParamDef> v;
    const Latency L;                                                   // the defaults of the cycle model are the initial values
    auto add = [&](const char* key, const char* group, const char* unit, int64_t def, int64_t lo, int64_t hi, const char* help, const char* measure = "", bool active = true) {
        ParamDef d;
        d.key = key; d.group = group; d.unit = unit; d.def = def; d.lo = lo; d.hi = hi; d.help = help; d.measure = measure; d.active = active;
        v.push_back(d);
        return &v.back();
    };
    auto lat = [&](const char* key, int Latency::* m, const char* help, const char* measure) {
        ParamDef* d = add(key, "Instruction latency", "ISA cycles", L.*m, 0, 100000, help, measure);
        d->latency = m;
    };
    auto boolean = [&](const char* key, const char* group, bool def, const char* help, const char* measure, bool active) {
        ParamDef* d = add(key, group, "", def ? 1 : 0, 0, 1, help, measure, active);
        d->kind = ParamDef::Bool;
    };
    auto choice = [&](const char* key, const char* group, std::vector<std::string> options, int64_t def, const char* help, const char* measure, bool active) {
        ParamDef* d = add(key, group, "", def, 0, (int64_t)options.size() - 1, help, measure, active);
        d->kind = ParamDef::Enum;
        d->options = std::move(options);
    };

    // ---- instruction latency (the cycle model of spec 14.3 / 14.4: planning numbers until the RTL gives the table)
    lat("latency.br_cond", &Latency::brCond, "BR.COND, taken or not", "N back-to-back BR.COND: total cycles / N");
    lat("latency.cascade", &Latency::cascade, "CASCADE_BEGIN / CASCADE_END", "a cascade of N parts: total cycles / (2N)");
    lat("latency.alu", &Latency::alu, "ALU add / sub / compare / logic / shift", "N back-to-back ALU operations: total cycles / N");
    lat("latency.mul", &Latency::mul, "ALU multiply", "N back-to-back MUL: total cycles / N");
    lat("latency.mov_setup", &Latency::movSetup, "MOV: fixed part (MOV costs setup + byte x length)", "MOV of 1 and of 8 bytes: the difference gives the per-byte part, the rest is the setup");
    lat("latency.mov_byte", &Latency::movByte, "MOV: cycles per byte", "see mov_setup");
    lat("latency.slot_consume_setup", &Latency::slotConsumeSetup, "MOV from PERI_RESULT_SLOT: fixed part (setup + byte x length)", "MOV from the slot of 1 and of 8 bytes");
    lat("latency.csr_access", &Latency::csrAccess, "one CSR access (a one-byte EXEC_CSR is 1 + access, a read-modify-write 1 + 2 x access)", "N back-to-back EXEC_CSR WRITE8: (total / N) - 1");
    lat("latency.csr_byte", &Latency::csrByte, "multi-byte EXEC_CSR: cycles per byte", "WRITE_SP of 1 and of 8 bytes");
    lat("latency.csr_done", &Latency::csrDone, "multi-byte EXEC_CSR: closing cycles", "see csr_byte");
    lat("latency.pub_setup", &Latency::pubSetup, "EXEC_PUB: fixed part (setup + byte x length)", "EXEC_PUB of 1 and of 8 bytes");
    lat("latency.pub_byte", &Latency::pubByte, "EXEC_PUB: cycles per byte", "see pub_setup");
    lat("latency.event_dispatch", &Latency::eventDispatch, "redirect to the handler vector once a pending event is dispatched", "event injection: dispatch to the first instruction of the handler");
    lat("latency.eret", &Latency::eret, "BR.ERET: back to the resume address", "the last handler instruction to the first resumed one");
    lat("latency.xfer_issue", &Latency::xferIssue, "EXEC_XFER issue overhead, before the bus time", "EXEC_XFER issue to the first SCL pulse (minus i2ch.overhead)");

    // ---- clocks
    add("clock.cpu_hz", "Clocks", "Hz", L.cpuHz, 1000, 1000000000, "the ISA clock (spec 14.1)", "from the FPGA design")->latency = &Latency::cpuHz;
    add("clock.i2c_hz", "Clocks", "Hz", L.i2cHz, 1000, 10000000, "the I2C bus clock; 400 kHz is 67.5 ISA cycles per SCL pulse at 27 MHz (spec 14.4)", "SCL period on a logic analyzer")->latency = &Latency::i2cHz;

    // ---- frame timing (the video timing that Main's conditions look at)
    const int64_t kMaxCycles = 2147483647;
    add("timing.frame_cycles", "Frame timing", "ISA cycles", 0, 0, kMaxCycles, "length of a frame; 0 = no frame timing (FRAME_END_EVT never fires)", "frame period from the sensor / timing generator");
    add("timing.vblank_start", "Frame timing", "ISA cycles", 0, 0, kMaxCycles, "where in the frame the vertical blanking window opens", "timing generator");
    add("timing.vblank_len", "Frame timing", "ISA cycles", 0, 0, kMaxCycles, "length of the vertical blanking window", "timing generator");
    add("timing.line_cycles", "Frame timing", "ISA cycles", 0, 0, kMaxCycles, "length of a line; 0 = no line timing", "timing generator");
    add("timing.hblank_start", "Frame timing", "ISA cycles", 0, 0, kMaxCycles, "where in the line the horizontal blanking window opens", "timing generator");
    add("timing.hblank_len", "Frame timing", "ISA cycles", 0, 0, kMaxCycles, "length of the horizontal blanking window", "timing generator");
    add("frame.wcet_pgl", "Frame timing", "ISA cycles", 0, 0, kMaxCycles, "Frame Timing WCET: worst-case time of a PGL commit (PGL_COMMIT_ELIGIBLE needs this much blanking left)", "the WCET register of the Frame Timing block");
    add("frame.wcet_ae", "Frame timing", "ISA cycles", 0, 0, kMaxCycles, "Frame Timing WCET of an AE commit", "the WCET register of the Frame Timing block");
    add("frame.wcet_line", "Frame timing", "ISA cycles", 0, 0, kMaxCycles, "Frame Timing WCET of a line commit", "the WCET register of the Frame Timing block");

    // ---- the pace of a run in the IDE (the simulator's own results do not depend on it)
    add("sim.speed", "Simulator pace", "ISA cycles per second", 1000, 0, 1000000000, "how fast the IDE lets the simulator run in real time, so that it can be watched: 20 to 100 is slow enough to follow instruction by instruction, 1000 shows the two lanes moving, 0 is full speed. It never changes what the program does or when (the Main and Peri timers do that), only how fast the clock on your screen goes. It can be changed while the simulator runs", "");

    // ---- scheduling
    choice("sim.tie_order", "Scheduling", {"main_first", "peri_first"}, 0, "who goes first when both sequencers are ready in the same cycle (timed events always come before both)", "two programs that touch the same CSR in the same cycle", true);
    choice("sim.unmapped_csr", "Scheduling", {"ram", "fault"}, 0, "an EXEC_CSR to an address the environment does not define: behaves as RAM, or faults", "", true);
    choice("sim.fault_stops", "Scheduling", {"domain", "all"}, 0, "a fault stops only its own sequencer, or both", "", true);

    // ---- the two decoders
    add("main.fetch_cycles", "Decoders", "ISA cycles", 0, 0, 1000, "extra cycles the Main decoder spends on every instruction (descriptor fetch and decode; the program SRAM runs at 2 x the ISA clock)", "N back-to-back one-cycle instructions: (total / N) - 1");
    add("peri.fetch_cycles", "Decoders", "ISA cycles", 0, 0, 1000, "the same for the Peri decoder", "N back-to-back BR instructions: (total / N) - 1");
    add("csr.access_main", "Decoders", "ISA cycles", -1, -1, 100000, "one CSR access as the Main decoder sees it; -1 = the same as latency.csr_access", "N back-to-back EXEC_CSR from Main");
    add("csr.access_peri", "Decoders", "ISA cycles", -1, -1, 100000, "one CSR access as the Peri decoder sees it; -1 = the same as latency.csr_access", "N back-to-back EXEC_CSR from Peri");
    add("reset.boot_cycles", "Decoders", "ISA cycles", 0, 0, 1000000, "from the end of reset to the first instruction of each sequencer", "first CSR write after reset on the logic analyzer");

    // ---- events
    add("event.dispatch_extra", "Events", "ISA cycles", 0, 0, 100000, "extra cycles on top of latency.event_dispatch (checkpoint sampling, vector load)", "event injection: raised to dispatched, minus latency.event_dispatch");
    add("event.eret_extra", "Events", "ISA cycles", 0, 0, 100000, "extra cycles on top of latency.eret", "last handler instruction to the first resumed one");

    // ---- timers (Main has one, Peri has one; a timer is started by EXEC_CSR and polled by BR)
    for (const char* who : {"main", "peri"}) {
        const std::string k = std::string("timer.") + who + ".";
        auto put = [&](const char* name, int64_t def, int64_t lo, int64_t hi, const char* unit, const char* help, const char* measure) {
            ParamDef d;
            d.key = k + name; d.group = "Timers"; d.unit = unit; d.def = def; d.lo = lo; d.hi = hi; d.help = std::string(who) + " timer: " + help; d.measure = measure;
            v.push_back(d);
        };
        put("clock_div", 1, 1, 65535, "ISA cycles per tick", "the timer counts at the ISA clock divided by this", "start with period N, measure the cycles until done, divide by N");
        put("start_latency", 0, 0, 1000000, "ISA cycles", "from the EXEC_CSR that starts it to the first tick", "start with period 1: total time minus the done latency");
        put("done_latency", 0, 0, 1000000, "ISA cycles", "from the last tick to the done latch being visible to BR", "start with period 1: total time minus the start latency");
    }

    // ---- the I2C path behind EXEC_XFER
    add("i2ch.overhead", "I2C path", "ISA cycles", 0, 0, 1000000, "extra cycles from EXEC_XFER to the I2C master starting (the i2ch block)", "EXEC_XFER issue to the first SCL pulse");
    add("i2c.ack_delay", "I2C path", "ISA cycles per byte", 0, 0, 1000, "extra cycles per byte phase for the acknowledge", "SCL falling edge of the 9th pulse to the next byte");
    add("i2c.stretch", "I2C path", "ISA cycles per byte", 0, 0, 1000000, "clock stretching per byte phase", "SCL held low by the device");
    add("i2c.turnaround", "I2C path", "ISA cycles", 0, 0, 1000000, "extra cycles for the direction change (repeated start) of a read with a register address", "repeated start on the bus");

    // ---- what the architecture drawing shows but the simulator does not model yet (stored, not used; a later stage turns them on)
    choice("csr.arbiter_policy", "CSR arbiter", {"none", "main_first", "peri_first", "round_robin"}, 0,
           "the Main/Peri CSR arbiter. none: no arbitration is modelled (the two decoders never wait for each other). Otherwise both decoders reach the CSRs through ONE arbiter: an EXEC_CSR holds it for the access part of the instruction, and an EXEC_CSR of the other decoder that finds it taken does not start until it is free (plus csr.arbiter_penalty). When both want it in the same cycle this policy chooses who goes first: main_first, peri_first, or round_robin (the one that did not have it last). A MODEL: confirm it against the RTL / the FPGA",
           "both decoders issue EXEC_CSR in a loop, started together: who is slowed, and by how much", true);
    add("csr.arbiter_penalty", "CSR arbiter", "ISA cycles", 0, 0, 100000, "extra cycles an EXEC_CSR waits after the arbiter became free (the hand-over); only with csr.arbiter_policy other than none", "difference in completion between the winner and the loser, minus the time the winner held the arbiter", true);
    boolean("datapath.contention", "Data path", false,
            "Data Xfer is shared by Peri's EXEC_PUB and Main's MOV from PERI_RESULT_SLOT: one that finds it taken waits until it is free (plus datapath.contention_cycles). A MODEL: confirm it against the RTL / the FPGA",
            "Main MOV from the slot while Peri runs EXEC_PUB", true);
    add("datapath.contention_cycles", "Data path", "ISA cycles", 0, 0, 100000, "extra cycles the one that waited for Data Xfer waits after it became free; only with datapath.contention", "see datapath.contention", true);
    add("slot.copy_extra", "Result slot", "ISA cycles", 0, 0, 100000, "extra cycles for EXEC_PUB to copy rx_buf into result_slot: the instruction takes this much longer and the result becomes visible this much later", "EXEC_PUB of 8 bytes against the model", true);
    add("slot.valid_delay", "Result slot", "ISA cycles", 0, 0, 100000, "delay from the copy to PERI_RESULT_VALID being visible to Main (a BR or a MOV before that does not see the result)", "Main polling PERI_RESULT_VALID after a PUB", true);
    add("flag.sample_delay_main", "Status flags", "ISA cycles", 0, 0, 100000, "a Main BR sees the flags as they were this many cycles ago (stored signals such as the timers, the video windows, the result slot; not the ALU flags)", "toggle a condition, find the first BR that sees it", true);
    add("flag.sample_delay_peri", "Status flags", "ISA cycles", 0, 0, 100000, "the same for a Peri BR", "toggle a condition, find the first BR that sees it", true);
    return v;
}

}  // namespace

const std::vector<ParamDef>& paramCatalog() {
    static const std::vector<ParamDef> c = build();
    return c;
}

const ParamDef* findParam(const std::string& key) {
    for (const ParamDef& d : paramCatalog()) if (d.key == key) return &d;
    return nullptr;
}

int64_t SimParams::defaultOf(const std::string& key) {
    const ParamDef* d = findParam(key);
    return d ? d->def : 0;
}

bool SimParams::set(const std::string& key, const std::string& text0, std::string& error) {
    const ParamDef* d = findParam(key);
    if (!d) { error = "unknown parameter '" + key + "' (asmsim --list-params lists them)"; return false; }
    const std::string text = trimmed(text0);
    int64_t v = 0;
    if (d->kind == ParamDef::Bool) {
        const std::string t = lowered(text);
        if (t == "true" || t == "on" || t == "yes" || t == "1") v = 1;
        else if (t == "false" || t == "off" || t == "no" || t == "0") v = 0;
        else { error = key + ": '" + text + "' is not true or false"; return false; }
    } else if (d->kind == ParamDef::Enum) {
        const std::string t = lowered(text);
        bool found = false;
        for (size_t i = 0; i < d->options.size(); i++) if (lowered(d->options[i]) == t) { v = (int64_t)i; found = true; }
        if (!found) {
            std::string all;
            for (const std::string& o : d->options) all += (all.empty() ? "" : " | ") + o;
            error = key + ": '" + text + "' is not one of " + all;
            return false;
        }
    } else {
        char* end = nullptr;
        v = std::strtoll(text.c_str(), &end, 0);
        if (text.empty() || !end || *end != 0) { error = key + ": '" + text + "' is not a number"; return false; }
        if (v < d->lo || v > d->hi) { error = key + ": " + std::to_string(v) + " is outside " + std::to_string(d->lo) + ".." + std::to_string(d->hi); return false; }
    }
    v_[key] = v;
    return true;
}

void SimParams::clear(const std::string& key) {
    v_.erase(key);
    status_.erase(key);
    note_.erase(key);
}

void SimParams::clearAll() {
    v_.clear();
    status_.clear();
    note_.clear();
}

int64_t SimParams::get(const std::string& key) const {
    auto it = v_.find(key);
    return it != v_.end() ? it->second : defaultOf(key);
}

std::string SimParams::text(const std::string& key) const {
    const ParamDef* d = findParam(key);
    const int64_t v = get(key);
    if (!d) return std::to_string(v);
    if (d->kind == ParamDef::Bool) return v ? "true" : "false";
    if (d->kind == ParamDef::Enum) return v >= 0 && (size_t)v < d->options.size() ? d->options[(size_t)v] : std::to_string(v);
    return std::to_string(v);
}

bool SimParams::setStatus(const std::string& key, const std::string& status, std::string& error) {
    if (!findParam(key)) { error = "unknown parameter '" + key + "'"; return false; }
    const std::string s = lowered(trimmed(status));
    if (s != "planning" && s != "measured" && s != "confirmed") { error = key + ": status must be planning, measured or confirmed"; return false; }
    if (s == "planning") status_.erase(key); else status_[key] = s;
    return true;
}

std::string SimParams::status(const std::string& key) const {
    auto it = status_.find(key);
    return it == status_.end() ? std::string("planning") : it->second;
}

void SimParams::merge(const SimParams& o) {
    for (const auto& kv : o.v_) v_[kv.first] = kv.second;
    for (const auto& kv : o.status_) status_[kv.first] = kv.second;
    for (const auto& kv : o.note_) note_[kv.first] = kv.second;
}

std::vector<std::string> SimParams::keys() const {
    std::vector<std::string> out;
    for (const ParamDef& d : paramCatalog()) if (v_.count(d.key)) out.push_back(d.key);
    return out;
}

bool SimParams::load(const std::string& text, std::vector<std::string>& errors) {
    bool ok = true;
    std::istringstream in(text);
    std::string raw;
    int no = 0;
    while (std::getline(in, raw)) {
        no++;
        const std::string line = trimmed(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) { errors.push_back("line " + std::to_string(no) + ": expected 'key = value'"); ok = false; continue; }
        const std::string key = trimmed(line.substr(0, eq)), val = trimmed(line.substr(eq + 1));
        std::string err;
        const size_t dot = key.rfind('.');
        const std::string tail = dot == std::string::npos ? std::string() : key.substr(dot + 1);
        if ((tail == "status" || tail == "note") && findParam(key.substr(0, dot))) {
            const std::string base = key.substr(0, dot);
            if (tail == "note") setNote(base, val);
            else if (!setStatus(base, val, err)) { errors.push_back("line " + std::to_string(no) + ": " + err); ok = false; }
        } else if (!set(key, val, err)) {
            errors.push_back("line " + std::to_string(no) + ": " + err);
            ok = false;
        }
    }
    return ok;
}

std::string SimParams::serialize() const {
    std::ostringstream o;
    o << "[params]\n";
    for (const std::string& k : keys()) {
        o << k << " = " << text(k) << "\n";
        if (status(k) != "planning") o << k << ".status = " << status(k) << "\n";
        if (!note(k).empty()) o << k << ".note = " << note(k) << "\n";
    }
    return o.str();
}

void SimParams::apply(Latency& l) const {
    for (const ParamDef& d : paramCatalog())
        if (d.latency && v_.count(d.key)) l.*(d.latency) = (int)v_.at(d.key);
}

}  // namespace asmcore
