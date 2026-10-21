#pragma once
// The Hardware Designer's bridge to the workspace. The description (.achw) and the two files generated from it live in the workspace as
// ordinary text buffers: the build and the debugger already use unsaved buffers, so an edit in the designer shows up in the next build
// without saving, and Save All / the unsaved-changes question cover them like any other file.
#include <string>
#include <vector>

#include "asmcore/hardware.h"
#include "workspace.h"

namespace acide {

struct HardwareFiles {
    std::string achw, include, env;               // absolute paths
};

HardwareFiles hardwareFiles(const std::string& achwPath, const asmcore::Hardware& hw);

// Reads the description from the workspace (an open buffer wins over the disk). False with `errors` if it cannot be read or parsed.
bool loadHardware(Workspace& ws, const std::string& achwPath, asmcore::Hardware& hw, std::vector<std::string>& errors);

// Puts the description and the files generated from it into the workspace as buffers (nothing is written to disk).
void applyHardware(Workspace& ws, const std::string& achwPath, const asmcore::Hardware& hw);

// Makes the project use the description: `hardware = ...`, and -- when the generated files are the default ones next to the description --
// lists the generated include as the first source and the generated environment as the environment (unless the project already has one).
// Files named by include_file / env_file are assumed to be in the project already (that is the "adopt" case). Saves the project.
bool attachHardware(Workspace& ws, const std::string& achwPath, const asmcore::Hardware& hw, std::string& error);

// A new description with a sensible start (name from the file) written to disk and attached; or one imported from existing files.
bool createHardware(Workspace& ws, const std::string& achwPath, std::string& error);
// A profile file with the default values (and a header saying what it is), set as the project's profile and written to disk.
bool createProfile(Workspace& ws, const std::string& absPath, std::string& error);
// The profile of the project as the workspace sees it (an open or unsaved buffer first). False if the project has none or it cannot be read.
bool loadProjectProfile(Workspace& ws, std::string& text, std::string& path);

bool adoptHardware(Workspace& ws, const std::string& achwPath, const std::string& incPath, const std::string& envPath, std::vector<std::string>& messages, std::string& error);

}  // namespace acide
