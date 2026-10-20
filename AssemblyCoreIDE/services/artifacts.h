#pragma once
// Writing the outputs of a build next to each other: PREFIX.main.bin / .peri.bin (+ .hex), .lst, .map, .dbg.json. Same names as asmc.
#include <string>
#include <vector>

#include "asmcore/asmcore.h"
#include "fs.h"

namespace acide {

struct ArtifactOptions {
    bool hex = true, listing = true, map = true, debug = true;
};
// Returns the paths written; `error` names the first file that could not be written ("" if all went well).
std::vector<std::string> writeArtifacts(IFileSystem& fs, const std::string& prefix, const asmcore::Result& r, const ArtifactOptions& opt, std::string& error);

}  // namespace acide
