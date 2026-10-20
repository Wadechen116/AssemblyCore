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
};
struct DecodedInstr {
    uint32_t length = 1;
    bool valid = true;                // false: not a legal encoding; `problem` says why and `text` is a comment
    std::string text;                 // assembler syntax; "{L}" stands for the label of `target`
    std::string problem;
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
// Label names of one domain from the assembler's symbol table, for DisasmOptions::names.
std::map<uint32_t, std::string> labelNames(const Result& r, Domain d);

}  // namespace asmcore
