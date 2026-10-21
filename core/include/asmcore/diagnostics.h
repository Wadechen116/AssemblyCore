#pragma once
// What a diagnostic code means, how to fix it, and the name spec v31's compiler contract (handoff section 12) gives the same problem.
// One table (src/diag_names.cpp); the CLI (`asmc --explain`), the validation report and the IDE read it, and a test checks that the
// table and docs/ASM_SYNTAX.md list exactly the same codes.
#include <string>
#include <vector>

namespace asmcore {

struct DiagInfo {
    const char* code;       // "E202", "W514"
    const char* specName;   // the v31 name ("E_MOV_REGION_OR_RANGE"), "" if v31 names no counterpart
    const char* title;      // one line: what is wrong
    const char* fix;        // what to do about it
};

// `code` may be written as Wnnn or Ennn: with --dataflow error the data-flow warnings (W5xx) are reported as E5xx and share their entry.
const DiagInfo* diagInfo(const std::string& code);
const std::vector<DiagInfo>& allDiagnostics();
// "E202: ... \n  v31 name: ...\n  fix: ..." for `asmc --explain`.
std::string explainDiagnostic(const std::string& code);

}  // namespace asmcore
