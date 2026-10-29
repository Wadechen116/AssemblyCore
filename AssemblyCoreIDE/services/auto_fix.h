#pragma once
// Auto fix: the program is built, the assembler's diagnostics are read, and what has ONE most likely repair is repaired - then it is built again, until the build is
// clean or nothing more can be mended. Only repairs that are sure enough are made (a misspelt word with exactly one close candidate, a number a little out of its
// range, a missing closing jump ...); what is left stays a diagnostic with the assembler's own advice. Every change is recorded: where, before, after, why.
//
//   code        what is mended
//   E101        an unknown instruction / directive: the one vocabulary word that is closest ("MVO" -> "MOV")
//   E103        an undefined symbol: the one defined symbol / region that is closest ("LIMT" -> "LIMIT")
//   E205        an unknown branch condition: the one condition that is closest
//   E301        an undefined branch target: the one label of that domain that is closest
//   E201        a number out of its range: moved to the nearest value in range; a scratchpad range that runs past the end: slid back so that it fits; a CSR address
//               wider than 12 bits: cut to 12 bits
//   E202        a constant moved straight into ALU_OPA / ALU_OPB: through a free pair of scratchpad bytes (spec v31 5.1)
//   E204        EXEC_CSR WRITE_BUF: renamed WRITE_SP
//   E303        code that can run off its end: closed with a jump to the first label of the domain
//   E203        an unknown ALU operation: the closest one
//   E104        a label defined twice: the second one gets a new name
//   E105        an instruction before any .main / .peri: the directive is added in front of it
//   W501        ALU_OPA / ALU_OPB never loaded in the domain: loaded in front of the operation, from the last 2-byte value written to the scratchpad in the same
//               stretch of code, else with the neutral value of the operation (0 for ADD / SUB / OR / XOR, 0xFFFF for AND, 1 for MUL) - "check"
//   W502        scratchpad bytes read that nothing in the domain ever writes: zeroed in front of the read - "check"
//   W504        an ALU result overwritten before it was moved out (straight-line code): saved into a free pair of scratchpad bytes first - "check"
//   W410        signed / unsigned on a LOGIC operation: dropped
//   W517        a publish length that differs from the .service contract: the contract's length
// No Qt.
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "build.h"

namespace acide {

struct FixStep {
    std::string code;                    // the diagnostic that is mended
    std::string what;                    // "MVO -> MOV (unknown instruction)"
    std::string file;
    int line = 0;                        // 1-based, in the text as it was when this step was made
    std::string before, after;           // the line (after: the lines, when one was added)
    // For programs made of blocks: the words that were exchanged on the line (old -> new), and for a closing jump the domain and its target. Steps of a block
    // program never add lines, except the closing jump.
    std::vector<std::pair<std::string, std::string>> words;
    bool closingJump = false;
    std::string domain, label;           // closingJump: "main" / "peri" and where it jumps to
    bool needsCheck = false;             // a repair that is a good guess, not a certainty (a value that had to be chosen, a range that was moved): look at it
};

struct AutoFixResult {
    std::vector<FixStep> steps;
    std::vector<asmcore::Diagnostic> remaining;      // errors and warnings the final build still has
    std::map<std::string, std::string> texts;        // file -> text after the repairs, only for the files that changed
    SnapshotPtr build;                               // the build of the repaired texts (the original build if nothing was mended)
    int rounds = 0;
    bool clean() const { return build && build->result.ok; }
};

struct AutoFixOptions {
    int maxRounds = 6;
    // Files generated from blocks: their lines are the blocks'; adding a line there is not an edit of the blocks, so only repairs that change words are made in them
    // (and the closing jump, which the editor adds as a block). Repairs that move lines (the closing jump) are made last, so that the line numbers of all the
    // other steps are those of the file as it was.
    std::set<std::string> wordsOnlyFiles;
    bool wordsOnly = false;                          // all files
};

// What the IDE did with a result (the main window makes it): which files were changed, and the steps that could not be put into the blocks of a block program
// (their words are not a field of any block: they are left for the user).
struct AutoFixOutcome {
    AutoFixResult result;
    bool applied = false;                            // false: only looked (a preview)
    std::vector<std::string> changedFiles;
    std::vector<FixStep> notApplied;
    std::string error;                               // nothing could be done (nothing built yet, ...)
};

AutoFixResult autoFix(BuildInput input, const asmcore::lang::Language& language, const AutoFixOptions& opt = AutoFixOptions());

// Exposed for tests: the closest candidate to `word` (case-insensitive, at most max(1, length / 3) edits, a swap of neighbours counts one); "" if none or if two are
// equally close.
std::string closestWord(const std::string& word, const std::vector<std::string>& candidates);

}  // namespace acide
