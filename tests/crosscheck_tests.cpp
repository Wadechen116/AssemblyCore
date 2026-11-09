// The decoder cross-check: the disassembler (disasm.cpp, DecoderSet) and the simulator's decoders (sim/main_core.cpp, sim/peri_core.cpp) decide
// separately whether the bytes of an instruction are a legal encoding (they share only the bit layouts of codec.h). This test makes them judge
// the same bytes: every bit of every byte of the EEPROM image of a program is flipped in turn, and for each instruction of each changed image
//   the disassembler says  valid / not a legal encoding (DecodedLine.valid)
//   the simulator, set to that instruction alone (setPc + step), executes it / raises a fault
// A fault of the encoding (reserved bits, a table index, a region, a range ...) must come exactly where the disassembler says "not legal".
// Faults of the state (a result slot that is empty, an ERET outside a handler, a cascade not begun ...) say nothing about the encoding.
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"
#include "asmcore/disasm.h"
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

// The simulator's faults that are about the bytes of the instruction itself (whatever the state), against those that depend on the state.
static const std::set<std::string> kEncodingFaults = {
    "fault_desc_reserved", "fault_desc_index", "fault_pc_out_of_code", "fault_branch_target", "fault_mov_range", "fault_range", "fault_mov_region",
    "fault_opcode", "fault_mov_overlap", "fault_cond_undefined", "xfer_fault_reserved_bus_type", "fault_xfer_multibyte_write", "fault_publish_service",
    "fault_peri_csr_read", "fault_peri_csr_multibyte", "fault_dev_undefined"};

struct Tally {
    long images = 0, instructions = 0, bothValid = 0, bothInvalid = 0, stateFault = 0, inconclusive = 0, mismatches = 0;
    long caughtBySpec = 0, notCaught = 0, executesInvalid = 0;               // of the mismatches: does the whole disassembly check (decode + spec + round trip) reject the image?
    std::map<std::string, long> uncaught;               // kind -> how many of it the whole check lets through
    std::map<std::string, long> kinds;                  // "disassembler <verdict> / simulator <fault>" -> count
    std::map<std::string, std::string> example;         // the first instance of each kind
};

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
        int whole = -1;                                  // the whole disassembly check of this image: -1 not run yet, 0 passes, 1 rejects
        auto wholeCheck = [&] {
            if (whole < 0) {
                EepromDisasmCheck ck;
                std::string e;
                whole = (checkEepromDisassembly(image, map, pf, nullptr, false, ck, e) && ck.ok(false)) ? 0 : 1;
            }
            return whole == 1;
        };
        for (Domain d : {Domain::Main, Domain::Peri})
            for (const DecodedLine& L : sim.decodedProgram(d)) {
                t.instructions++;
                sim.restore(start);
                if (!sim.setPc(d, L.pc)) continue;
                const RunResult rr = sim.step(d);
                const Fault* f = sim.fault(d);
                const bool enc = f && kEncodingFaults.count(f->code);
                const bool state = f && !enc;
                std::string kind;
                if (L.valid && !f) { t.bothValid++; continue; }
                if (L.valid && state) { t.stateFault++; continue; }               // a legal encoding the state does not allow: nothing to compare
                if (!L.valid && enc) { t.bothInvalid++; continue; }
                if (!L.valid && state) { t.inconclusive++; kind = "inconclusive: disassembler 'not legal', simulator stopped first at the state (" + f->code + ")"; }
                else if (L.valid && enc) {
                    t.mismatches++;
                    kind = "MISMATCH: disassembler 'legal', simulator " + f->code;
                    if (wholeCheck()) t.caughtBySpec++;
                    else { t.notCaught++; t.uncaught[kind]++; }
                }
                else { t.mismatches++; t.executesInvalid++; kind = "MISMATCH: disassembler 'not legal', simulator executes it"; }
                if (!t.kinds[kind]++)
                    t.example[kind] = where + ", " + domainName(d) + " pc " + std::to_string(L.pc) + ": decoded '" + L.text + "'" + (L.valid ? "" : " (" + L.problem + ")") +
                                      (f ? "; simulator: " + f->msg : "; simulator: step stopped (" + rr.detail + "), pc now " + std::to_string(sim.pc(d)) + ", " +
                                                     std::to_string(sim.instructions(d)) + " instruction(s) executed");
            }
    };
    const long invalidBefore = t.bothInvalid, mismatchesBefore = t.mismatches;
    judge(base, std::string(name) + " unchanged");
    check(t.bothInvalid == invalidBefore && t.mismatches == mismatchesBefore, std::string(name) + ": the unchanged image -- every instruction legal for both");
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
    std::printf("  both: a legal encoding                         %ld\n", t.bothValid);
    std::printf("  both: not a legal encoding                     %ld\n", t.bothInvalid);
    std::printf("  legal, but the state does not allow it         %ld   (not compared)\n", t.stateFault);
    std::printf("  inconclusive (the simulator stopped at the state first) %ld\n", t.inconclusive);
    std::printf("  MISMATCHES                                     %ld\n", t.mismatches);
    std::printf("     of them rejected by the whole disassembly check (spec stage)   %ld\n", t.caughtBySpec);
    std::printf("     of them NOT rejected by any check                           %ld\n", t.notCaught);
    for (const auto& kv : t.kinds)
        std::printf("\n  %ld x %s%s\n     e.g. %s\n", kv.second, kv.first.c_str(),
                    t.uncaught.count(kv.first) ? ("   [" + std::to_string(t.uncaught[kv.first]) + " not rejected by the whole check]").c_str() : "",
                    t.example[kv.first].c_str());
    // What holds today. The decoder stage of the disassembler checks the encoding bit by bit; the range and meaning checks the simulator makes
    // when it executes (a MOV range, a branch target, a DeviceMap entry ...) are made by the spec stage, which assembles the disassembly.
    // So the per-instruction verdict of the decode stage (the Decoder panel) can say "legal" where the simulator faults: that gap is
    // counted above, not failed. What must hold: nothing the disassembler rejects is executed, and nothing the simulator rejects passes the whole check.
    check(t.executesInvalid == 0, "no instruction the disassembler rejects is executed by the simulator", std::to_string(t.executesInvalid));
    check(t.notCaught == 0, "every image the simulator faults on is rejected by the whole disassembly check (decode + spec + round trip)", std::to_string(t.notCaught));
    std::printf("\n  decode stage alone vs the simulator: %ld instruction(s) the decode stage calls legal and the simulator faults on (all caught by the spec stage)\n",
                t.mismatches - t.executesInvalid);
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
