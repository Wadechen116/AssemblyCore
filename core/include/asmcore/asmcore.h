#pragma once
// AssemblyCore -- assembler for the Sensor Micro-Sequencer ISA v1.6 r10 (Main / Peri).
//
// Public interface of the core library. The command-line assembler (asmc) and the IDE are both clients of this header;
// nothing here depends on a UI or on the file system (sources are passed in as text, includes through a callback).
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "asmcore/latency.h"

namespace asmcore {

enum class Domain { Main, Peri };
const char* domainName(Domain d);

struct SrcLoc {
    std::string file;
    int line = 0;
    // Columns of the offending text on that line, 0-based, [col, endCol). endCol == 0: not known (the whole line). Filled for
    // diagnostics by the assembler; a source location from anywhere else leaves them 0.
    int col = 0, endCol = 0;
};

struct Diagnostic {
    enum Severity { Error, Warning, Note };
    Severity sev = Error;
    SrcLoc loc;
    std::string code;   // E1xx syntax, E2xx encoding, E3xx validation, W... warnings; see docs/ASM_SYNTAX.md
    std::string msg;
};

// How CALL / RET are lowered. See docs/ASM_SYNTAX.md ("Procedures").
enum class CallModel {
    Inline,   // the body is copied to every CALL site; no runtime return mechanism
    SoftRet,  // body emitted once; the caller stores a return id, RET dispatches on it (Main only: Peri has no MOV / ALU)
};

// The numeric codes of the ALU sub-operations. Spec v31 6.3 lists their NAMES and the bit fields but not the numbers: an approved RTL
// profile supplies them. The defaults are the order in which the spec lists the names (ASSUMED until the profile says otherwise).
struct AluCodes {
    int arith[5] = {0, 1, 2, 3, 4};          // ADD SUB MUL INC DEC  -> arith_op  (5 bits, descriptor [4:0])
    int cmp[4] = {0, 1, 2, 3};               // EQ NE LT GE          -> cmp_op    (2 bits, descriptor [4:3])
    int logic[4] = {0, 1, 2, 3};             // AND OR XOR NOT       -> logic_op  (3 bits, descriptor [5:3])
};

// Section numbers of the profile's per-section alignment: the six sections of MainImageLayout, then the six of PeriImageLayout.
enum ProfileSection { PS_MAIN_MOVE = 0, PS_MAIN_BRANCH, PS_MAIN_CSR1, PS_MAIN_CSRM, PS_MAIN_ALU, PS_MAIN_CONST,
                      PS_PERI_XFER1, PS_PERI_XFERM, PS_PERI_PUB, PS_PERI_BRANCH, PS_PERI_CSR1, PS_PERI_DEVMAP, PS_COUNT };

// Target profile = the RTL parameters the assembler must agree with (spec 3.1: "Descriptor table depth/count is an RTL parameter
// and must match the compiler/assembler target profile").
struct Profile {
    std::string profileId = "planning-default";   // names the target this profile describes (appears in every artifact)
    // Groups of values (profile_info.h) that someone has checked against the hardware: `confirmed = alu, placement`. Everything else is a
    // planning value and is listed as a gap (profileGaps) -- never an error: the tools work with planning values, they just say so.
    std::set<std::string> confirmed;
    // descriptor table depths (entries); defaults are the full arg-addressable depth
    int mainMoveDepth = 64, mainBranchDepth = 64, mainCsr1Depth = 32, mainCsrMDepth = 32, mainAluDepth = 64;
    int periXfer1Depth = 32, periXferMDepth = 32, periPubDepth = 64, periBranchDepth = 64, periCsr1Depth = 32;
    int deviceMapDepth = 8;                 // EXEC_XFER.dev_sel is 3 bits
    int maxCodeBytes = 256;                 // upper bound on the executable code of a domain (further limited by its PC width)
    // Program memory (spec v31 3.1): ONE 32-bit synchronous SRAM (clock = 2 x the ISA clock) holds both logical images. The numbers below
    // are RTL parameters the spec does not give; the defaults are a PLANNING layout (two 2 KiB windows) so that images can be produced and
    // inspected, not values of an approved target.
    int physicalSramDepthWords = 1024;
    int mainImageBaseWord = 0, periImageBaseWord = 512;      // physical WORD addresses (not byte addresses)
    int mainCapacityBytes = 2048, periCapacityBytes = 2048;  // the reserved window of each image, in logical bytes
    int mainPcWidth = 8, periPcWidth = 8;                    // architectural PC width in bits; the code of a domain is at most 2^width bytes
    int bootPadByte = 0;                                     // fills the last word of an image and the gaps between sections (never executed)
    int layoutOffsetBits = 16;                               // width of each ImageLayout sideband field (spec: profile / boot interface)
    int sectionAlign[PS_COUNT] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   // bytes; -1 = the width of the section's entries
    int deviceMapEntryBytes = 1;                             // DEVICE_MAP_ENTRY_BYTES
    int deviceMapBusShift = 7, deviceMapAddrShift = 0;       // bit positions inside an entry (bus_type 1 bit, i2c_addr_7b 7 bits)
    AluCodes alu;
    int constPoolMax = 256;                 // MOV src_offset is 8 bits
    int eventCount = 8;                     // event ids 0..eventCount-1 per domain (vector sources / numbering are profile-defined, v31 16.7)
    bool csrBufValid = false;               // the (optional, v31 proposed) per-byte csr_buf_valid protection exists: a MOV from a byte whose READ failed faults
    bool sameRegionMov = true;              // MOV between NON-overlapping ranges of one region (spec v31 5.2 forbids only overlap; confirm with the RTL tests)
    bool profileCond0 = false;              // Main ConditionMap 0xE is "profile-defined"; illegal unless the profile defines it
    // SoftRet call model: where the assembler keeps its private state (must not be used by the program)
    int retSlotBase = 24;                   // MAIN_SCRATCHPAD[retSlotBase ..): one 2-byte return slot per procedure with >= 2 call sites
    int retStageBase = 30;                  // MAIN_SCRATCHPAD[retStageBase .. +1]: staging for the return-id comparison (constants cannot be MOVed to ALU_OPB directly)
    // image
    bool bigEndian = false;                 // byte order of multi-byte descriptors in the image (the spec does not give it)
    // Reset values (spec 3.3.8: "the implementation must declare"). When true, the data-flow check does not require the region to be
    // written before it is read. Per-range alternative: the `.assume` directive.
    bool scratchpadResetDefined = false, csrBufResetDefined = false, periRxResetDefined = false;
    Latency latency;

    // Reads "key = value" lines ([section] headers allowed). Returns false and fills `errors` on a bad line.
    bool load(const std::string& text, std::vector<std::string>& errors);
    std::string dump() const;
};

struct Source {
    std::string name;
    std::string text;
};
// Called for `.include "name"`; `from` is the including file. Return false if the file cannot be found.
using IncludeResolver = std::function<bool(const std::string& from, const std::string& name, Source& out)>;

// What the data-flow check (spec 3.3.9, 12.8: operands loaded, buffers written before use, slot consumed correctly...) does with a finding.
enum class DataflowMode { Off, Warn, Error };

struct Options {
    Profile profile;
    CallModel callModel = CallModel::Inline;
    bool werror = false;          // warnings fail the build
    bool padTables = false;       // pad each descriptor table to the profile depth (illegal-descriptor fill = 0)
    DataflowMode dataflow = DataflowMode::Warn;   // W5xx findings: ignored / warnings (E5xx with Error)
    std::vector<std::string> suppress;            // warning codes to drop, e.g. "W502" (errors cannot be suppressed)
    // WCET queries: "start_label:end_label" pairs, Main only (also `.wcet` in source)
    std::vector<std::string> wcetQueries;
};

// ---- debug information (consumed by the simulator and the IDE) -------------------------------------------------------------
struct Frame {                    // one level of the CALL chain an instruction was expanded from
    std::string proc;
    SrcLoc callSite;
};
struct InstrInfo {
    Domain domain = Domain::Main;
    uint32_t pc = 0;
    uint32_t length = 0;
    SrcLoc loc;                   // where the text came from (for expansions: the line inside the procedure)
    std::string text;
    std::vector<Frame> frames;    // innermost last; empty for code written directly in the flow
    bool synthetic = false;       // generated by the assembler (SoftRet call / return sequences)
    std::string note;
    int cycles = 0;               // from the latency model (profile.latency)
    // The descriptor this instruction selects: the name of the section of DomainImage::sections ("MainMoveDesc_32b"...) and the index in it.
    std::string table;
    int descIndex = -1;
    // Control flow, for analyses that work on the image rather than on the source.
    bool isBranch = false;
    bool fallsThrough = true;     // execution can continue at pc + length (false after an unconditional branch and for CASCADE_BEGIN)
    uint32_t target = 0;          // branch target pc (if isBranch)
};
struct ProcInfo {
    Domain domain = Domain::Main;
    std::string name;
    SrcLoc loc;
    bool emitted = false;         // SoftRet: has code of its own; Inline: false
    uint32_t startPc = 0, endPc = 0;
    int callSites = 0;
    int slot = -1;                // SoftRet: MAIN_SCRATCHPAD offset of its return slot (-1: none needed)
    // Main only: the worst case over its call sites of the time from the call to the return (placeholder latencies). 0 and unbounded when
    // there is no call site or the body has a loop.
    bool wcetBounded = false;
    int wcetCycles = 0;
};
struct CallSiteInfo {
    Domain domain = Domain::Main;
    std::string callee;
    SrcLoc loc;
    uint32_t transferPc = 0;      // SoftRet: pc of the BR that enters the procedure
    uint32_t returnPc = 0;        // SoftRet: pc execution resumes at
    int retId = 0;                // SoftRet: 16-bit id stored in the callee's return slot (0: not needed)
    std::string caller;           // the procedure whose body contains the CALL ("" in the flow)
    // Main only: worst-case cycles from the CALL (the call sequence included) to the instruction after it.
    bool wcetBounded = false;
    int wcetCycles = 0;
};
struct VectorInfo {                // `.vector ID, label`: the entry of an event handler (domain-local, full-width in hardware)
    Domain domain = Domain::Main;
    int id = 0;
    std::string label;
    uint32_t pc = 0;
    SrcLoc loc;
};
// Static event timing of one domain (spec v31 16.9). Parts are kept apart because they have different owners:
// pending -> next certified checkpoint (the program), dispatch (frontend), handler (the program), ERET + refetch (frontend).
struct EventTiming {
    bool present = false;                 // the domain declares at least one .vector
    bool checkpointBounded = false;       // every foreground cycle contains an event_safe BR
    int checkpointCycles = 0;             // worst cycles from any point to the next event_safe BR (engine-only, placeholder latencies)
    int checkpoints = 0;                  // number of event_safe BR in the domain
    int dispatchCycles = 0, eretCycles = 0;
    struct Handler {
        int id = 0;
        std::string label;
        bool bounded = false;
        int cycles = 0;                   // vector -> BR.ERET, longest path
    };
    std::vector<Handler> handlers;
    bool bounded = false;                 // everything above is bounded
    int worstCycles = 0;                  // checkpoint + dispatch + worst handler + ERET (only meaningful when bounded)
    std::string note;
};
struct SymbolInfo {
    std::string name, kind;       // kind: equ | csr | service | const | label | proc | devmap
    int64_t value = 0;
    SrcLoc loc;
};
struct SectionInfo {
    std::string name;
    uint32_t offset = 0, size = 0;
    int entries = 0, entryBytes = 0;
    int depth = 0;                // capacity allowed by the profile (0: not applicable)
};
struct WcetResult {
    std::string name, from, to;
    bool bounded = false;
    int cycles = 0;
    std::string note;
};
struct DomainImage {
    Domain domain = Domain::Main;
    bool present = false;
    // The LOGICAL image (spec v31 3.1): executable bytecode from byte 0, then the descriptor tables and the const pool / DeviceMap, with
    // any section padding the profile requires. It does NOT contain ImageLayout: that goes to independent sideband latches (`layout`).
    std::vector<uint8_t> bytes;
    uint32_t codeSize = 0;
    std::vector<SectionInfo> sections;       // "code" and the six sections located by `layout`
    // MainImageLayout / PeriImageLayout: the logical byte offset of each of the six sections, in the order of spec 3.1.
    std::vector<uint32_t> layout;
    // Physical placement in the shared program SRAM: the image packed into 32-bit words (byte +0 in bits [7:0]; the last word padded).
    uint32_t baseWord = 0;                   // physical word address of logical word 0
    uint32_t reservedWords = 0;              // size of the window reserved for this image (from the profile capacity)
    std::vector<uint32_t> words;
};
const char* const* layoutFieldNames(Domain d);          // the six ImageLayout field names, in order (main_move_desc_offset ...)

struct Result {
    bool ok = false;              // no errors (and no warnings if Options::werror)
    std::vector<Diagnostic> diags;
    DomainImage main, peri;
    std::vector<InstrInfo> instrs;
    std::vector<ProcInfo> procs;
    std::vector<CallSiteInfo> callSites;
    std::vector<SymbolInfo> symbols;
    std::vector<WcetResult> wcet;
    std::vector<VectorInfo> vectors;
    EventTiming eventTiming[2];   // indexed by domain (Main = 0, Peri = 1)
    int maxCallDepth = 0;
    std::string callModel;        // "inline" | "softret"

    Profile profile;              // the target profile the program was assembled for (artifacts quote it)

    std::string listing() const;
    std::string map() const;
    // ---- build artifacts of spec v31 (see docs/ASM_SYNTAX.md, "artifacts"); all JSON is UTF-8, numbers are decimal unless a field says hex
    std::string layoutJson(Domain d) const;              // the six ImageLayout sideband offsets + the section ranges
    std::string sectionMapJson() const;                  // code span, every section, padding, logical sizes and physical words
    std::string descriptorMapJson() const;               // every descriptor table: index, raw bytes, value, decoded text, users
    std::string sramWordsHex() const;                    // "ADDR DATA" per 32-bit word of the one shared program SRAM
    std::string bootPlanJson() const;                    // what boot loads: SRAM words, sideband layout fields, order
    std::string memoryProofJson() const;                 // occupied ranges, reserved windows, the rules checked
    std::string servicesJson() const;                    // service ids, what Peri publishes
    std::string eventVectorsJson() const;                // domain-qualified event vectors
    std::string validationReportJson() const;            // diagnostics and the readiness level
    std::string buildManifestJson(const std::vector<Source>& sources) const;   // spec, tool, profile, checksums
    std::string debugJson() const;                       // see docs/DEBUG_INFO.md
    std::string intelHex(Domain d) const;
    const DomainImage& image(Domain d) const { return d == Domain::Main ? main : peri; }
    int errorCount() const;
    int warningCount() const;
    std::string diagnosticsText() const;                 // "file:line: error E201: ..." per line
};

Result assemble(const std::vector<Source>& sources, const IncludeResolver& resolver, const Options& opt);

// Reads a PREFIX.<domain>.layout.json written by layoutJson(): fills `im.layout` (and the section list, if present) so a bare .bin can be
// disassembled. The image bytes are the caller's.
bool loadLayoutJson(const std::string& json, DomainImage& im, std::string& error);

}  // namespace asmcore
