#pragma once
// Writing the outputs of a build next to each other: PREFIX.main.bin / .peri.bin (+ .hex), .lst, .map, .dbg.json. Same names as asmc.
#include <string>
#include <vector>

#include "asmcore/asmcore.h"
#include "fs.h"

namespace acide {

struct ArtifactOptions {
    bool hex = true, listing = true, map = true, debug = true;
    // spec v31: PREFIX.<domain>.layout.json is ALWAYS written (the .bin holds no ImageLayout; it is meaningless without it).
    // `v31` adds the rest of the artifact set: SRAM words, boot plan, memory proof, section / descriptor maps, services, events,
    // validation report and (when `sources` is given) the build manifest.
    bool v31 = true;
    std::vector<asmcore::Source> sources;
};
// Returns the paths written; `error` names the first file that could not be written ("" if all went well).
std::vector<std::string> writeArtifacts(IFileSystem& fs, const std::string& prefix, const asmcore::Result& r, const ArtifactOptions& opt, std::string& error);

}  // namespace acide
