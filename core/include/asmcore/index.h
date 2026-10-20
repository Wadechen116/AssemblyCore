#pragma once
// AssemblyCore language service, part 2: where every symbol is defined and used, and the source <-> address map.
//
// The index is built from the token stream (lang.h), NOT from a successful assembly, so it keeps working while the program is
// half typed or has errors: outline, go-to-definition, find-all-references and rename all stand on it.
//
// Scoping follows the assembler: labels and procedure names belong to a domain (.main / .peri), labels inside a .proc belong to
// that procedure, everything else (.equ .csr .service .const .devmap names) is global.
#include <map>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"
#include "asmcore/lang.h"

namespace asmcore {

struct IndexSymbol {
    std::string name;
    std::string kind;                 // label | proc | equ | csr | service | const | devmap | wcet
    std::string scope;                // "" global, "main" / "peri" (labels, procs), or "main:proc" / "peri:proc" for a procedure-local label
    SrcLoc def;                       // columns of the name
    std::vector<SrcLoc> refs;         // every use (not including the definition)
    int id = 0;
};

class Index {
public:
    // `sources` in the order the assembler reads them (an included file's text goes where its .include stands; list the shared
    // files first). Unsaved editor buffers can be passed as they are.
    // `resolver` (optional) follows `.include "file"` into the included text, like the assembler does.
    static Index build(const std::vector<Source>& sources, const lang::Language& language, const IncludeResolver& resolver = nullptr);

    const std::vector<IndexSymbol>& symbols() const { return syms_; }
    // The symbol whose definition or use covers (file, line, col); nullptr if none. Line is 1-based, col 0-based.
    const IndexSymbol* at(const std::string& file, int line, int col) const;
    const IndexSymbol* find(const std::string& name, const std::string& scope = "") const;
    // Definition plus all uses, in file/line order.
    std::vector<SrcLoc> occurrences(const IndexSymbol& s) const;
    // Why `newName` cannot replace the symbol's name ("" when it can): not an identifier, a keyword, or already taken in the scope.
    std::string renameProblem(const IndexSymbol& s, const std::string& newName) const;
    // Identifiers used but never defined (name, first use), for "undefined symbol" hints that do not need an assembly.
    std::vector<std::pair<std::string, SrcLoc>> undefined() const { return undefined_; }

private:
    std::vector<IndexSymbol> syms_;
    std::vector<std::pair<std::string, SrcLoc>> undefined_;
    lang::Language lang_;
};

// ---- source <-> address ------------------------------------------------------------------------------------------------------------
// Answers the questions a debugger asks: which addresses does this line produce (an inline CALL expands a procedure line into
// several), and which line, and call chain, does this address come from.
class SourceMap {
public:
    explicit SourceMap(const Result& r);
    // Every instruction that came from the line, in address order (a procedure line expanded at N call sites gives N entries). A CALL line
    // gives the first instruction of the expansion (inline) or the generated call sequence (softret, flagged `synthetic`).
    std::vector<const InstrInfo*> instrsForLine(const std::string& file, int line) const;
    const InstrInfo* instrAt(Domain d, uint32_t pc) const;
    // The nearest line at or after `line` that produces code (a breakpoint on a comment moves down to the next instruction); 0 if none.
    int nearestCodeLine(const std::string& file, int line) const;
    // Lines of `file` that produced code.
    std::vector<int> codeLines(const std::string& file) const;

private:
    const Result* r_;
    std::map<std::pair<std::string, int>, std::vector<size_t>> byLine_;
    std::map<std::pair<int, uint32_t>, size_t> byPc_;
};

}  // namespace asmcore
