#pragma once
// AssemblyCore disassembler: an image back to assembler source.
//
// The central promise: for any image the assembler produced, the text from disassemble().source() assembles to the SAME image,
// byte for byte (tests/disasm_tests.cpp checks it on every instruction form and on the examples, in both call models). That makes
// "assemble -> disassemble -> assemble -> compare" a regression test for the whole tool chain.
//
// Pipeline (every stage can be replaced):
//   ImageView      finds the tables and the code through the ImageLayout tail        (src/image_view.*)
//   code extent    from the debug information if given, otherwise inferred (docs/DISASM.md)
//   DecoderSet     one decode function per opcode and domain                          (this header: replace entries to change)
//   labels         from the branch targets, named from the symbol table when available
//   formatter      DisasmResult::source()
//
// The bit layouts are not repeated here: decoders unpack descriptors through src/codec.h, the same file the assembler packs with.
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"
#include "asmcore/eeprom.h"

namespace asmcore {

// ---- the extension point: instruction decoders ------------------------------------------------------------------------------------
// What a decoder is shown. It never sees the image buffer directly, so it keeps working if the image format changes.
struct DecodeContext {
    Domain domain = Domain::Main;
    uint32_t pc = 0;                  // address of the instruction byte
    int opcode = 0;                   // instruction byte [7:6]
    int arg = 0;                      // instruction byte [5:0]
    uint32_t codeLimit = 0;           // bytes at or after this address are not code
    const AluCodes* alu = nullptr;    // numeric codes of the ALU sub-operations (profile); nullptr = the defaults
    // The byte at a code address (0 outside the image).
    std::function<uint32_t(uint32_t pos)> byteAt;
    // Entry `idx` of a descriptor table; `table` is the ImageLayout index (spec 3.1 order), `entryBytes` its width. False if the
    // entry lies outside the table.
    std::function<bool(int table, uint32_t idx, int entryBytes, uint32_t& value)> entry;
    // The DeviceMap entry byte for EXEC_XFER.dev_sel (Peri); false if absent.
    std::function<bool(uint32_t dev, uint32_t& value)> deviceMap;
    const Profile* profile = nullptr; // region rules of the target (same-region MOV, DeviceMap layout); nullptr = the defaults
    uint32_t constSize = 0;           // bytes of MAIN_CONST_POOL (a MOV from it must stay inside); 0 = not known, not checked
    // Whether a named branch condition has a definition where the program runs (the simulator: its ConditionMap with the environment file);
    // empty = every named condition counts as defined (the disassembler alone cannot know a profile-defined one).
    std::function<bool(int code)> conditionDefined;
};
// Two kinds of problem in the bytes of an instruction (docs/DISASM.md "A / B"):
//   A  not a legal encoding: reserved bits, a table index, a reserved code ... -- it cannot be written as an instruction (valid = false, text is a comment)
//   B  a legal encoding that breaks a rule of the spec: a MOV range outside its region, a region in the wrong direction, a branch target outside
//      the code, a DeviceMap entry that is not there ... -- it is written as the instruction it encodes (the assembler rejects it), and the
//      hardware faults when it executes it (which stage detects it is the target's matter). specProblem says which rule.
// A third kind, C, is not in the bytes at all: the state when it runs (an empty result slot, an ERET outside a handler); only running finds it.
struct DecodedInstr {
    uint32_t length = 1;
    bool valid = true;                // false: not a legal encoding (A); `problem` says why and `text` is a comment
    std::string text;                 // assembler syntax; "{L}" stands for the label of `target`
    std::string problem;              // with valid: a remark (D202), not an error
    std::string specProblem;          // B: a legal encoding against a rule of the spec ("" = none); text is the instruction
    bool branch = false;              // has a branch target
    bool uncond = false;              // never falls through (the next address is not reached from here)
    bool eret = false;                // BR.ERET: ends an event handler, but more code (another handler) may follow, so it does not mark the end of the code
    uint32_t target = 0;
};
using DecodeFn = std::function<DecodedInstr(const DecodeContext&)>;
struct DecoderSet {
    DecodeFn main[4], peri[4];        // indexed by opcode
    static DecoderSet defaults();     // the baseline of the spec
};

// ---- options and results ---------------------------------------------------------------------------------------------------------------
struct DisasmOptions {
    // Exact size of the code in bytes. 0 = use DomainImage::codeSize, and if that is 0 too, infer it: the code ends after an
    // unconditional branch that nothing in the BranchDesc table targets.
    uint32_t codeSize = 0;
    // pc -> label name (use labelNames(result, domain)); other targets get labelPrefix + address.
    const std::map<uint32_t, std::string>* names = nullptr;
    std::string labelPrefix = "L_";
    const DecoderSet* decoders = nullptr;         // default: DecoderSet::defaults()
    // Event vectors (id, pc) of this domain, e.g. from PREFIX.events.json. They are not part of the image, so without them handler code is
    // simply decoded as unreachable code; with them the text gets a label and a ".vector id, label" line and assembles with the same checks.
    std::vector<std::pair<int, uint32_t>> vectors;
    bool useJmp = true;                           // unconditional branches as JMP
    // Called for every line; the returned text is appended as a comment (the disassembler has no opinion about its content).
    std::function<std::string(uint32_t pc, const std::string& text)> commentHook;
};
struct DisasmIssue {
    Domain domain = Domain::Main;                 // the domain whose image it is about
    uint32_t pc = 0;
    std::string code;                             // D1xx: image, D2xx: instruction, D3xx: tables
    std::string msg;
    bool fatal = true;                            // fatal: the text cannot reproduce this image; otherwise only a remark
};
struct DisasmLine {
    uint32_t pc = 0;
    uint32_t length = 0;
    std::vector<uint8_t> bytes;
    std::string label;                            // label defined at this address ("" if none)
    std::string text;                             // assembler syntax (a comment if !valid)
    bool valid = true;
    std::string comment;                          // from the commentHook
};
struct DisasmResult {
    Domain domain = Domain::Main;
    bool ok = false;                              // no fatal issue: every instruction decoded, every branch lands on an instruction
    uint32_t codeSize = 0;
    std::vector<std::string> definitions;         // .const / .devmap lines
    std::vector<DisasmLine> lines;
    std::vector<DisasmIssue> issues;

    // Assembler text. `annotate` adds "; pc: bytes" comments; the result still assembles to the same image when ok.
    std::string source(bool annotate = false) const;
    std::string issuesText() const;
};

// Disassemble one domain image. `profile` supplies the layout assumptions (offset width, byte order) the assembler used.
DisasmResult disassemble(const DomainImage& image, const Profile& profile, const DisasmOptions& opt = DisasmOptions());
// Both domains of an assembler result as one source file (labels keep their original names).
std::string disassembleResult(const Result& r, const Profile& profile, std::vector<DisasmIssue>* issues = nullptr, bool annotate = false);
// A burned EEPROM image (eeprom.h) back to source, both domains: the regions of `map` give each domain's code and tables. With `symbols`
// (PREFIX.eeprom.sym) the names come back: labels, CSRs (with their .csr lines), services (.service), DeviceMap devices, and the const
// pool symbols as comments; the text still assembles to the same image. False with `error` if the image cannot be read through the map.
// `lineOf`, when given, gets the line of `source` (1-based) of every instruction: [domain 0 Main / 1 Peri][pc].
bool disassembleEeprom(const std::vector<uint8_t>& image, const EepromMap& map, const Profile& profile, const EepromSymbols* symbols, bool annotate,
                       std::string& source, std::vector<DisasmIssue>* issues, std::string& error, std::map<uint32_t, int>* lineOf = nullptr);

// The disassembly of an EEPROM image, checked: is it a program of this project's specification? Three things are checked, and anything that
// is not is reported:
//   decode     the bytes are legal encodings (reserved bits, table indexes, branch targets on an instruction ...)       -> decodeIssues
//   spec       the text goes through the assembler: every rule of docs/ASM_SYNTAX.md, the profile's limits and the data-flow check  -> specDiags
//   identity   it assembles back to the same bytes in every region of the image                                         -> identical
struct EepromDisasmCheck {
    std::string source;
    std::vector<DisasmIssue> decodeIssues;
    std::vector<Diagnostic> specDiags;            // lines refer to `source`
    std::map<uint32_t, int> lineOf[2];            // [0] Main, [1] Peri: pc -> its line in `source` (1-based)
    // The line of `source` a decode issue is about: the instruction at its pc (D2xx); 0 for an issue of the image or a table (D1xx, D3xx).
    int lineOfIssue(const DisasmIssue& i) const {
        if (i.code.compare(0, 2, "D2") != 0) return 0;
        const auto& m = lineOf[i.domain == Domain::Peri ? 1 : 0];
        auto it = m.find(i.pc);
        return it == m.end() ? 0 : it->second;
    }
    bool assembles = false;                       // the assembler found no error
    bool identical = false;
    size_t differentBytes = 0;                    // in the regions, against the image
    int differentRegion = -1;                     // the first region with a difference (EepromRegionId)
    uint32_t firstDifference = 0;                 // its EEPROM address
    int fatalDecode() const { int n = 0; for (const DisasmIssue& i : decodeIssues) n += i.fatal; return n; }
    int errors() const { int n = 0; for (const Diagnostic& d : specDiags) n += d.sev == Diagnostic::Error; return n; }
    int warnings() const { int n = 0; for (const Diagnostic& d : specDiags) n += d.sev == Diagnostic::Warning; return n + (int)decodeIssues.size() - fatalDecode(); }
    // strict: a warning (a data-flow rule, an unreachable instruction, a decoder remark) fails it too
    bool ok(bool strict = false) const { return fatalDecode() == 0 && assembles && identical && (!strict || warnings() == 0); }
};
bool checkEepromDisassembly(const std::vector<uint8_t>& image, const EepromMap& map, const Profile& profile, const EepromSymbols* symbols, bool annotate,
                            EepromDisasmCheck& out, std::string& error);
// Label names of one domain from the assembler's symbol table, for DisasmOptions::names.
std::map<uint32_t, std::string> labelNames(const Result& r, Domain d);

}  // namespace asmcore
