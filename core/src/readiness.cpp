// Readiness levels (readiness.h).
#include "asmcore/readiness.h"

#include <sstream>

#include "asmcore/profile_info.h"
#include "util.h"

namespace asmcore {

namespace {

// Which level an unconfirmed group of the profile keeps a build from. Encoding, placement and the boot interface decide whether the IMAGE is right
// (L1); the cycle counts decide whether the TIMING is right (L2). The ALU group stands for the shift direction too (SHL = 0 / SHR = 1 in isa.h: it
// has no key of its own, and is confirmed together with the other ALU codes).
int levelOfGroup(const std::string& g) { return g == "latency" || g == "events" ? 2 : 1; }

bool trusted(const SimParams& p, const char* key) { const std::string s = p.status(key); return s == "measured" || s == "confirmed"; }

void addParam(Readiness& r, const SimParams& p, const char* key) {
    if (trusted(p, key)) return;
    const ParamDef* d = findParam(key);
    ReadinessItem it;
    it.blocks = 2;
    it.kind = "parameter";
    it.id = key;
    it.title = d ? d->help : std::string(key);
    it.why = "the time to fetch an instruction is counted with it, and it is still a planning value (" + p.text(key) + ")";
    it.fix = std::string("measure it or check it against the RTL, then mark it: ") + key + ".status = measured (or confirmed) in the .simparams file";
    r.missing.push_back(it);
}

std::string jq(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

}  // namespace

Readiness readiness(const Profile& profile, const SimParams* params) {
    Readiness r;
    bool l1 = true;
    for (const ProfileGap& g : profileGaps(profile)) {
        ReadinessItem it;
        it.blocks = levelOfGroup(g.group);
        it.kind = "profile";
        it.id = g.group;
        it.title = g.title;
        it.why = g.why;
        if (g.group == "alu") it.why += " This group also stands for the shift direction bit (SHL = 0, SHR = 1), which has no key of its own.";
        it.fix = "check the values against the RTL / the board, then add the group to the profile: confirmed = ..., " + g.group;
        if (it.blocks == 1) l1 = false;
        r.missing.push_back(it);
    }
    // ---- the fetch time (L2): counted at all, and with numbers somebody has measured
    if (!params) {
        ReadinessItem it;
        it.blocks = 2; it.kind = "tool"; it.id = "fetch_time";
        it.title = "the time to fetch each instruction is not in the WCET";
        it.why = "this build was made without the hardware parameters, so its WCET counts the execution alone";
        it.fix = "build with the parameters: AssemblyCoreIDE does, asmc with --params FILE / --param KEY=VALUE / --fetch-time";
        r.missing.push_back(it);
    } else {
        const int source = (int)params->get("fetch.source");            // 0 eeprom, 1 sram, 2 frontend
        const bool instant = source == 0 && params->get("fetch.read_mode") == 2;
        if (source == 1 || instant) {
            ReadinessItem it;
            it.blocks = 2; it.kind = "parameter"; it.id = source == 1 ? "fetch.source" : "fetch.read_mode";
            it.title = source == 1 ? "fetch.source = sram: fetching takes no time" : "fetch.read_mode = instant: fetching takes no time";
            it.why = "a setting for looking at a program, not the hardware: the WCET then counts the execution alone";
            it.fix = "set fetch.source to how the hardware fetches (eeprom or frontend)" + std::string(instant ? " and fetch.read_mode to sequential or per_byte" : "");
            r.missing.push_back(it);
        } else {
            addParam(r, *params, "fetch.source");
            if (source == 0) for (const char* k : {"fetch.read_mode", "fetch.transaction_overhead", "i2c.ack_delay", "i2c.stretch"}) addParam(r, *params, k);
            else for (const char* k : {"frontend.sram_latency", "frontend.sram_policy", "frontend.hold_cycles", "frontend.dec_cycles", "frontend.fire_cycles"}) addParam(r, *params, k);
        }
    }
    {   // what the tools cannot bound yet, whatever is confirmed
        ReadinessItem it;
        it.blocks = 2; it.kind = "tool"; it.id = "shared_waits";
        it.title = "the WCET does not bound the waiting for the other sequencer";
        it.why = "Main and Peri share the I2C bus / the program SRAM port, the CSR arbiter, the Status Flag unit and Data Xfer; how long one waits depends on what the other is doing, and the WCET analysis looks at one sequencer alone";
        it.fix = "not available yet: until the tools bound these waits, add the longest wait by hand (the simulator shows them) and treat every WCET as planning-only";
        r.missing.push_back(it);
    }
    bool l2 = l1;
    for (const ReadinessItem& it : r.missing) if (it.blocks == 2) l2 = false;
    r.level = !l1 ? 0 : !l2 ? 1 : 2;
    // what blocks the NEXT level first
    std::vector<ReadinessItem> first, rest;
    for (const ReadinessItem& it : r.missing) (it.blocks == r.level + 1 ? first : rest).push_back(it);
    first.insert(first.end(), rest.begin(), rest.end());
    r.missing = first;
    return r;
}

const char* readinessName(int level) { return level <= 0 ? "L0 structural" : level == 1 ? "L1 binary-approved" : "L2 timing-approved"; }

const char* readinessMay(int level) {
    return level <= 0 ? "check the program (syntax, fields, ranges, data flow), look at the layout, simulate and compare alternatives"
         : level == 1 ? "burn the images: the encoding, the placement and the load plan are the hardware's"
                      : "sign off the timing: the WCET and the event latency are bounded with confirmed numbers";
}
const char* readinessMayNot(int level) {
    return level <= 0 ? "that the image boots or runs correctly on the hardware, or that any cycle count is a guarantee"
         : level == 1 ? "that a WCET or an event latency is a guarantee: the timing is planning-only"
                      : "timing behaviour of an architecture nobody has validated in RTL";
}

std::string readinessText(const Readiness& r) {
    std::ostringstream o;
    o << "readiness: " << readinessName(r.level) << "\n";
    o << "  you may:      " << readinessMay(r.level) << "\n";
    o << "  do not claim: " << readinessMayNot(r.level) << "\n";
    if (r.missing.empty()) return o.str();
    int shown = -1;
    for (const ReadinessItem& it : r.missing) {
        if (it.blocks != shown) { o << "\n  missing for " << readinessName(it.blocks) << ":\n"; shown = it.blocks; }
        o << "    [" << it.kind << "] " << it.id << " -- " << it.title << "\n";
        if (!it.why.empty()) o << "        why: " << it.why << "\n";
        o << "        do:  " << it.fix << "\n";
    }
    return o.str();
}

std::string readinessJson(const Readiness& r) {
    std::ostringstream o;
    o << "{\"level\":" << r.level << ",\"name\":" << jq(readinessName(r.level)) << ",\"may\":" << jq(readinessMay(r.level)) << ",\"do_not_claim\":" << jq(readinessMayNot(r.level)) << ",\"missing\":[";
    for (size_t i = 0; i < r.missing.size(); i++) {
        const ReadinessItem& it = r.missing[i];
        o << (i ? "," : "") << "{\"blocks\":" << it.blocks << ",\"kind\":" << jq(it.kind) << ",\"id\":" << jq(it.id) << ",\"title\":" << jq(it.title) << ",\"fix\":" << jq(it.fix) << "}";
    }
    o << "]}";
    return o.str();
}

}  // namespace asmcore
