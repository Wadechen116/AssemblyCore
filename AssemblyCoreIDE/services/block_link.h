#pragma once
// The bridge between block programs (.acblk) and the workspace. The blocks are the truth; the assembler file they generate (named by the program's `output`,
// else NAME.generated.asm next to it) lives in the workspace as a buffer like any other, so the build and the debugger use it without saving, and Save All
// writes both. The project lists the programs (`blocks = ...`) and the generated file is one of its sources.
#include <map>
#include <string>

#include "asmcore/blocks.h"
#include "workspace.h"

namespace acide {

// Absolute path of the file generated from a program: its `output` (relative to the program's folder), else NAME.generated.asm next to it.
std::string generatedPathFor(const std::string& acblkPath, const asmcore::blocks::Document& doc);
// The comment at the top of a generated file.
std::string generatedHeader(const std::string& acblkPath);

// Makes the project use the program: `blocks = ...` and, if it is not a source yet, the generated file as a source (at the end). Does nothing without a project,
// or for a program outside the project's folder. Saves the project -- and the generated file if it is not on disk yet (the project never names a missing file). `changed` tells whether anything was edited.
bool attachBlocks(Workspace& ws, const std::string& acblkPath, const std::string& generatedPath, std::string& error, bool* changed = nullptr);
// Releases the program: `blocks = ...` goes, the generated file stays a source (it is a plain assembler file again).
bool detachBlocks(Workspace& ws, const std::string& acblkPath, std::string& error);

// generated file (absolute) -> program (absolute), for every program of the project (the workspace buffers first).
std::map<std::string, std::string> generatedFiles(Workspace& ws);

}  // namespace acide
