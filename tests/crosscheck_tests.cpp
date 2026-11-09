// The decoder cross-check: the disassembler (disasm.cpp, DecoderSet) and the simulator's decoders (sim/main_core.cpp, sim/peri_core.cpp) decide
// separately whether the bytes of an instruction are a legal encoding (they share only the bit layouts of codec.h). This test makes them judge
// the same bytes: every bit of every byte of the EEPROM image of a program is flipped in turn, and for each instruction of each changed image
//   the disassembler says  valid / not a legal encoding (DecodedLine.valid)
//   the simulator, set to that instruction alone (setPc + step), executes it / raises a fault
// A problem in the bytes -- A (not a legal encoding) or B (a legal encoding against a rule of the spec) -- must be seen by both, in the same class.
// Faults of the state (C: an empty result slot, an ERET outside a handler ...) say nothing about the bytes: only running finds them.
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"
#include "asmcore/eeprom.h"
#include "asmcore/sim.h"

using namespace asmcore;
using namespace asmcore::sim;

#include "all_forms.inc"           // kAll: every instruction form of both domains

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& why = "") {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok && !why.empty()) std::printf("        %s\n", why.c_str());
    (ok ? g_pass : g_fail)++;
}
static std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Tally {
    long images = 0, instructions = 0, bothOk = 0, bothFlagged = 0, stateFault = 0, notReached = 0, inconclusive = 0, mismatches = 0, classDiffer = 0;
    std::map<std::string, long> kinds;                  // a disagreement -> count
    std::map<std::string, std::string> example;         // the first instance of each
};

// A branch the decoder marks B (its target is outside the code) faults only when it is taken: not reaching it is no disagreement.
static bool conditionalBranch(const std::string& text) { return text.rfind("BR", 0) == 0 || text.rfind("CASCADE", 0) == 0 || text.rfind("JMP", 0) == 0; }

static void crossCheck(const char* name, const Result& r, const Profile& pf, Tally& t) {
    std::vector<uint8_t> base;
    EepromMap map;
    std::string err;
    if (!buildEeprom(r, pf, base, map, err)) { check(false, std::string(name) + ": its EEPROM image", err); return; }
    auto judge = [&](const std::vector<uint8_t>& image, const std::string& where) {
        SimConfig c;
        c.profile = pf;
        c.eepromImage = image;
        c.eepromMap = map;
        c.params.set("fetch.source", "eeprom", err);
        c.params.set("fetch.read_mode", "instant", err);
        Simulator sim(r, c);
        if (!sim.ok()) return;
        t.images++;
        const Snapshot start = sim.snapshot();
        for (Domain d : {Domain::Main, Domain::Peri})
            for (const DecodedLine& L : sim.decodedProgram(d)) {
                t.instructions++;
                sim.restore(start);
                if (!sim.setPc(d, L.pc)) continue;
                const RunResult rr = sim.step(d);
                const Fault* f = sim.fault(d);
                const char sc = f ? faultClass(f->code) : 0;                  // the simulator: 'A' 'B' 'C' or 0
                const char dc = L.problemClass();                             // the decoder:   'A' 'B' or 0
                std::string kind;
                if (!dc && !f) { t.bothOk++; continue; }
                if (!dc && sc == 'C') { t.stateFault++; continue; }           // a legal instruction the state does not allow: only running finds it
                if (dc && (sc == 'A' || sc == 'B')) {
                    t.bothFlagged++;
                    if (dc == sc) continue;
                    t.classDiffer++;
                    kind = std::string("class differs: decoder ") + dc + ", simulator " + sc + " (" + f->code + ")";
                } else if (dc == 'B' && !f && conditionalBranch(L.text)) { t.notReached++; continue; }
                else if (dc && !f) { t.mismatches++; kind = std::string("MISMATCH: decoder ") + dc + ", the simulator executes it"; }
                else if (dc == 'B' && sc == 'C') { t.inconclusive++; continue; }  // stopped at the state before the rule B is about (CASCADE_END with no cascade)
                else if (dc && sc == 'C') { t.mismatches++; kind = std::string("MISMATCH: decoder ") + dc + ", the simulator stopped at the state first (" + f->code + ")"; }
                else { t.mismatches++; kind = std::string("MISMATCH: the decoder sees no problem, the simulator ") + sc + " " + f->code; }
                if (!t.kinds[kind]++)
                    t.example[kind] = where + ", " + domainName(d) + " pc " + std::to_string(L.pc) + ": decoded '" + L.text + "'" +
                                      (dc == 'A' ? " (" + L.problem + ")" : dc == 'B' ? " (" + L.specProblem + ")" : "") +
                                      (f ? "; simulator: " + f->msg : "; simulator: " + rr.detail);
            }
    };
    const long before = t.bothFlagged + t.mismatches;
    judge(base, std::string(name) + " unchanged");
    check(t.bothFlagged + t.mismatches == before, std::string(name) + ": the unchanged image -- no problem for either");
    for (int i = 0; i < ER_COUNT; i++)
        for (uint32_t k = 0; k < map.region[i].size; k++)
            for (int bit = 0; bit < 8; bit++) {
                std::vector<uint8_t> img = base;
                const uint32_t a = map.region[i].addr + k;
                img[a] ^= (uint8_t)(1u << bit);
                char where[96];
                std::snprintf(where, sizeof where, "%s, %s byte 0x%04X bit %d", name, eepromRegionName(i), a, bit);
                judge(img, where);
            }
}

int main() {
    std::printf("AssemblyCore decoder cross-check (disassembler vs the simulator's decoders)\n");
    Tally t;
    {
        Options o;
        std::vector<Source> src{Source{"all_forms.asm", kAll}};
        Result r = assemble(src, nullptr, o);
        check(r.ok, "(the all-forms program assembles)", r.diagnosticsText());
        if (r.ok) crossCheck("all forms", r, Profile(), t);
    }
    {
        const std::string dir = std::string(ASMCORE_SOURCE_DIR) + "/sample/";
        Options o;
        std::vector<Source> src{Source{dir + "14_main_peri_sim/main_peri.asm", slurp(dir + "14_main_peri_sim/main_peri.asm")}};
        auto res = [&](const std::string&, const std::string& n, Source& out) { out.name = dir + "common/" + n; out.text = slurp(out.name); return !out.text.empty(); };
        Result r = assemble(src, res, o);
        check(r.ok, "(example 14 assembles)", r.diagnosticsText());
        if (r.ok) crossCheck("example 14", r, Profile(), t);
    }
    std::printf("\n%ld changed images, %ld instructions judged by both\n", t.images, t.instructions);
    std::printf("  no problem for either                                   %ld\n", t.bothOk);
    std::printf("  a problem in the bytes for both (A / B)                 %ld   (of them the class differs: %ld)\n", t.bothFlagged, t.classDiffer);
    std::printf("  legal, the state does not allow it (C, only running)    %ld\n", t.stateFault);
    std::printf("  B branch target, not taken (faults only when taken)     %ld\n", t.notReached);
    std::printf("  B, but the state stopped it before that rule (inconclusive) %ld\n", t.inconclusive);
    std::printf("  MISMATCHES                                              %ld\n", t.mismatches);
    for (const auto& kv : t.kinds) std::printf("\n  %ld x %s\n     e.g. %s\n", kv.second, kv.first.c_str(), t.example[kv.first].c_str());
    std::printf("\n");
    check(t.mismatches == 0, "whatever the simulator faults on for its bytes (A / B), the decoder marks; whatever the decoder marks, the simulator faults on", std::to_string(t.mismatches));
    check(t.classDiffer == 0, "...and both put it in the same class (A or B)", std::to_string(t.classDiffer));
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
