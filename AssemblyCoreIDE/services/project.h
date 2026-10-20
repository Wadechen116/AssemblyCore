#pragma once
// The project file (.acproj): which sources make the program and how to build and run it. A plain "key = value" text file; keys that
// can repeat (source, include_dir, ...) are listed once per line. Paths are relative to the project file.
#include <string>
#include <vector>

#include "asmcore/asmcore.h"

namespace acide {

struct Project {
    std::string name;
    std::string file;                              // absolute path of the .acproj ("" for an unsaved project)
    std::vector<std::string> sources;              // in assembly order (shared definition files first)
    std::vector<std::string> includeDirs;
    std::string profile;                           // profile .ini
    std::string environment;                       // simulator environment file
    std::vector<std::string> scenarios;
    asmcore::CallModel callModel = asmcore::CallModel::Inline;
    asmcore::DataflowMode dataflow = asmcore::DataflowMode::Warn;
    std::vector<std::string> suppress;             // warning codes
    std::vector<std::string> wcet;                 // "from:to"
    bool werror = false;
    std::string outputDir = "out";
    std::string outputPrefix;                      // default: name

    std::string baseDir() const;                   // directory of the project file
    std::string absolute(const std::string& rel) const;
    // Fields the assembler needs; the profile must be loaded by the caller (it is a file).
    asmcore::Options options() const;

    // Text <-> Project. Unknown keys are kept in `extra` so a newer file survives an older IDE unchanged.
    static bool parse(const std::string& text, Project& out, std::vector<std::string>& errors);
    std::string serialize() const;
    std::vector<std::pair<std::string, std::string>> extra;
};

}  // namespace acide
