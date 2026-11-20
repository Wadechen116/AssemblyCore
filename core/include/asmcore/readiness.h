#pragma once
// Readiness levels (spec v31 handoff 9.3): how far the outputs may be trusted, decided by what has been confirmed against the hardware.
//
//   L0 structural        something of the encoding / placement / boot interface is still a planning value. The tools parse, check, lay out and test;
//                        nobody may claim that the image boots on the hardware.
//   L1 binary-approved   every encoding, placement and boot-interface group of the profile is confirmed (`confirmed = ...`): the images, the sideband
//                        layout and the SRAM / EEPROM load plan are the real ones. Timing is still planning-only: no WCET signoff.
//   L2 timing-approved   on top of L1, the cycle model is confirmed and bounded: the profile's latency and event groups, the hardware parameters that
//                        decide how an instruction is fetched (status measured / confirmed), and a WCET that bounds everything -- including the wait
//                        for the other sequencer, which these tools do not bound yet (so L2 cannot be reached today, and the list says so).
//
// The level never changes what is computed: it says what may be claimed.
#include <string>

#include "asmcore/asmcore.h"
#include "asmcore/sim_params.h"

namespace asmcore {

// `params`: the hardware parameters the build counts its cycles with, or nullptr when it was built without them (the fetch time is then not in the
// WCET, which is one more thing between the build and L2).
Readiness readiness(const Profile& profile, const SimParams* params = nullptr);

const char* readinessName(int level);          // "L0 structural", "L1 binary-approved", "L2 timing-approved"
const char* readinessMay(int level);           // what may be done with the outputs at this level
const char* readinessMayNot(int level);        // what must not be claimed at this level
// Several lines for a terminal: the level, what it allows, and everything that is missing (the next level's first), each with what to do.
std::string readinessText(const Readiness& r);
// {"level":0,"name":"L0 structural","missing":[{"blocks":1,"kind":"profile","id":"alu","title":...,"fix":...}]}
std::string readinessJson(const Readiness& r);

}  // namespace asmcore
