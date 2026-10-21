#include "asmcore/profile_info.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include "util.h"

namespace asmcore {

namespace {

std::map<std::string, std::string> defaultValues() {
    std::map<std::string, std::string> m;
    std::istringstream in(Profile().dump());
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        m[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return m;
}

std::vector<ProfileKeyInfo> buildKeys() {
    std::vector<ProfileKeyInfo> v;
    auto I = [&](const char* name, const char* group, long long lo, long long hi, const char* desc) {
        ProfileKeyInfo k;
        k.name = name; k.group = group; k.type = ProfileKeyInfo::Int; k.lo = lo; k.hi = hi; k.description = desc;
        v.push_back(k);
    };
    auto B = [&](const char* name, const char* group, const char* desc) {
        ProfileKeyInfo k;
        k.name = name; k.group = group; k.type = ProfileKeyInfo::Bool; k.description = desc;
        v.push_back(k);
    };
    auto T = [&](const char* name, const char* group, const char* desc) {
        ProfileKeyInfo k;
        k.name = name; k.group = group; k.type = ProfileKeyInfo::Text; k.description = desc;
        v.push_back(k);
    };
    T("profile_id", "identity", "Names the target this profile describes; it appears in every generated artifact.");
    // descriptor tables
    I("main_move_depth", "tables", 1, 64, "Entries of the Main MOV descriptor table (the instruction byte addresses at most 64).");
    I("main_branch_depth", "tables", 1, 64, "Entries of the Main branch descriptor table.");
    I("main_csr1_depth", "tables", 1, 32, "Entries of the Main one-byte EXEC_CSR descriptor table.");
    I("main_csrm_depth", "tables", 1, 32, "Entries of the Main multi-byte EXEC_CSR descriptor table.");
    I("main_alu_depth", "tables", 1, 64, "Entries of the Main ALU descriptor table.");
    I("peri_xfer1_depth", "tables", 1, 32, "Entries of the Peri one-byte EXEC_XFER descriptor table.");
    I("peri_xferm_depth", "tables", 1, 32, "Entries of the Peri multi-byte EXEC_XFER descriptor table.");
    I("peri_pub_depth", "tables", 1, 64, "Entries of the Peri EXEC_PUB descriptor table.");
    I("peri_branch_depth", "tables", 1, 64, "Entries of the Peri branch descriptor table.");
    I("peri_csr1_depth", "tables", 1, 32, "Entries of the Peri one-byte EXEC_CSR descriptor table.");
    I("device_map_depth", "tables", 1, 8, "Entries of the DeviceMap (EXEC_XFER dev_sel is 3 bits).");
    I("const_pool_max", "tables", 1, 256, "Bytes of MAIN_CONST_POOL (a MOV source offset is 8 bits).");
    // code size
    I("max_code_bytes", "pc_width", 1, 65536, "Upper bound on the executable code of a domain, further limited by its PC width.");
    I("main_pc_width", "pc_width", 8, 16, "Bits of the Main program counter. Ordinary branch targets stay 8 bits whatever this is.");
    I("peri_pc_width", "pc_width", 8, 16, "Bits of the Peri program counter.");
    B("profile_cond0", "tables", "Main branch condition 0xE (PROFILE_COND0) is defined by this target.");
    B("same_region_mov", "same_region", "A MOV between non-overlapping ranges of one region is legal (the spec forbids only overlap; confirm with the RTL tests).");
    B("csr_buf_valid", "same_region", "The optional per-byte 'CSR read-back is valid' protection exists (spec v31 proposes it; do not assume it).");
    // placement
    I("sram_depth_words", "placement", 1, 1 << 24, "Depth of the one shared program SRAM, in 32-bit words.");
    I("main_image_base_word", "placement", 0, 1 << 24, "Physical WORD address where the Main logical image starts.");
    I("peri_image_base_word", "placement", 0, 1 << 24, "Physical WORD address where the Peri logical image starts.");
    I("main_capacity_bytes", "placement", 1, 1 << 26, "Logical bytes reserved for the Main image (its window in the SRAM).");
    I("peri_capacity_bytes", "placement", 1, 1 << 26, "Logical bytes reserved for the Peri image.");
    I("boot_pad_byte", "placement", 0, 255, "Value of the padding between sections and in the last word of an image.");
    // layout
    I("layout_offset_bits", "layout", 8, 32, "Width of each ImageLayout sideband latch; every section offset must fit.");
    static const char* const alignNames[] = {"main_move", "main_branch", "main_csr1", "main_csrm", "main_alu", "main_const", "peri_xfer1", "peri_xferm", "peri_pub", "peri_branch", "peri_csr1", "peri_devmap"};
    for (const char* a : alignNames) {
        I((std::string("align_") + a).c_str(), "layout", -1, 64, "Alignment of this section's start in bytes; -1 = the width of one entry.");
    }
    B("big_endian", "descriptor_endian", "Multi-byte descriptors are stored high byte first. Spec v31 does not restate the byte order: confirm it with the RTL.");
    // DeviceMap packing
    I("device_map_entry_bytes", "device_map", 1, 4, "Bytes of one DeviceMap entry in the image.");
    I("device_map_bus_shift", "device_map", 0, 31, "Bit position of bus_type inside an entry.");
    I("device_map_addr_shift", "device_map", 0, 31, "Bit position of the 7-bit I2C address inside an entry.");
    // ALU numbering
    static const struct { const char* name; int hi; const char* what; } alu[] = {
        {"alu_arith_add", 31, "ADD"}, {"alu_arith_sub", 31, "SUB"}, {"alu_arith_mul", 31, "MUL"}, {"alu_arith_inc", 31, "INC"}, {"alu_arith_dec", 31, "DEC"},
        {"alu_cmp_eq", 3, "CMP.EQ"}, {"alu_cmp_ne", 3, "CMP.NE"}, {"alu_cmp_lt", 3, "CMP.LT"}, {"alu_cmp_ge", 3, "CMP.GE"},
        {"alu_logic_and", 7, "AND"}, {"alu_logic_or", 7, "OR"}, {"alu_logic_xor", 7, "XOR"}, {"alu_logic_not", 7, "NOT"}};
    for (auto& a : alu) I(a.name, "alu", 0, a.hi, a.what);
    for (auto& k : v) if (k.group == "alu") k.description = "Numeric code of ALU " + k.description + " (the spec names the operations but does not number them; each code must be unique within its family).";
    // softret
    I("ret_slot_base", "softret", 0, 31, "First MAIN_SCRATCHPAD byte the softret call model keeps its return slots in (2 bytes per procedure with 2 or more call sites).");
    I("ret_stage_base", "softret", 0, 30, "MAIN_SCRATCHPAD byte pair softret uses to compare a return id (reserved for the assembler).");
    // reset state
    B("scratchpad_reset_defined", "reset", "MAIN_SCRATCHPAD is zero after reset, so reading it before writing is fine.");
    B("csrbuf_reset_defined", "reset", "MAIN_CSR_DATA_BUF is zero after reset.");
    B("peri_rx_reset_defined", "reset", "PERI_RX_BUF is zero after reset.");
    // events
    I("event_count", "events", 1, 64, "Event ids per domain (.vector ID, label). Vector sources, numbering and priority belong to the target.");
    I("event_dispatch", "events", 0, 100000, "Cycles to redirect to the handler vector (a planning number; the RTL must give an upper bound).");
    I("event_eret", "events", 0, 100000, "Cycles for BR.ERET and the refetch of the resume PC (planning).");
    // latency
    I("br_cond", "latency", 0, 100000, "Cycles of a BR.COND, taken or not (planning latency, spec 14.3).");
    I("cascade", "latency", 0, 100000, "Cycles of CASCADE_BEGIN / CASCADE_END.");
    I("alu", "latency", 0, 100000, "Cycles of an ADD / SUB / CMP / LOGIC / SHIFT.");
    I("mul", "latency", 0, 100000, "Cycles of MUL.");
    I("mov_setup", "latency", 0, 100000, "Fixed cycles of a MOV (the total is setup + bytes x mov_byte).");
    I("mov_byte", "latency", 0, 100000, "Cycles per byte of a MOV.");
    I("slot_consume_setup", "latency", 0, 100000, "Fixed cycles of a MOV that consumes the result slot.");
    I("csr_access", "latency", 0, 100000, "Cycles of one CSR access (CSR_ACCESS_CYC).");
    I("csr_byte", "latency", 0, 100000, "Cycles per byte of a multi-byte EXEC_CSR.");
    I("csr_done", "latency", 0, 100000, "Cycles at the end of a multi-byte EXEC_CSR.");
    I("pub_setup", "latency", 0, 100000, "Fixed cycles of EXEC_PUB.");
    I("pub_byte", "latency", 0, 100000, "Cycles per published byte.");
    I("xfer_issue", "latency", 0, 100000, "Cycles to issue an EXEC_XFER, before the I2C bus time.");
    I("cpu_hz", "latency", 1, 1000000000, "ISA clock in Hz (spec 14.1: 27 MHz).");
    I("i2c_hz", "latency", 1, 1000000000, "I2C clock in Hz.");
    const std::map<std::string, std::string> defs = defaultValues();
    for (auto& k : v) { auto it = defs.find(k.name); if (it != defs.end()) k.defaultText = it->second; }
    return v;
}

}  // namespace

const std::vector<ProfileKeyInfo>& profileKeys() {
    static const std::vector<ProfileKeyInfo> k = buildKeys();
    return k;
}

const ProfileKeyInfo* profileKey(const std::string& name) {
    for (const ProfileKeyInfo& k : profileKeys()) if (k.name == name) return &k;
    return nullptr;
}

const std::vector<ProfileGroup>& profileGroups() {
    static const std::vector<ProfileGroup> g = {
        {"identity", "Identity", "", false},
        {"tables", "Descriptor tables", "", false},
        {"pc_width", "Program counter and code size", "How far the code may grow and how wide the program counter is. The spec leaves the PC width and any narrower code limit to the target; ordinary branches still reach only bytes 0..255.", true},
        {"same_region", "Optional hardware features", "Whether a non-overlapping MOV inside one region is allowed, and whether CSR read-back bytes carry a valid bit. The v31 spec does not decide either; the RTL tests do.", true},
        {"placement", "Program SRAM placement", "Where the Main and Peri images sit in the one shared SRAM and how much room each has. The spec shows a toy layout only; the real depth, base addresses and capacities are RTL parameters.", true},
        {"layout", "Image layout and alignment", "How wide the sideband offset latches are and how sections are aligned. Not stated by the spec; wrong values make the hardware read the wrong table.", true},
        {"descriptor_endian", "Descriptor byte order", "In which byte order multi-byte descriptors are stored. v31 says to concatenate 'in the existing byte order' and leaves the serialization to the target; the SRAM lane mapping alone does not settle it.", true},
        {"device_map", "DeviceMap entry packing", "Which bits of a DeviceMap entry hold the bus type and the I2C address, and how many bytes an entry takes. The spec gives only the bit widths.", true},
        {"alu", "ALU sub-operation numbers", "The spec names ADD, SUB, MUL, CMP.EQ ... but never gives their numeric codes. The numbers here are placeholders: a wrong code makes the hardware do a different operation.", true},
        {"softret", "Software return (softret)", "", false},
        {"reset", "State after reset", "", false},
        {"events", "Hardware events", "Number of events and the cycles to enter a handler and return from it. Vector sources, priorities and the cycle bounds are defined by the target.", true},
        {"latency", "Latency numbers", "Cycle counts used for the listing, WCET and the simulator. They are the PLANNING numbers of spec section 14, not signoff values; response times are only as good as these.", true},
    };
    return g;
}

std::vector<ProfileGap> profileGaps(const Profile& p) {
    std::vector<ProfileGap> gaps;
    for (const ProfileGroup& g : profileGroups()) {
        if (!g.confirmable || p.confirmed.count(g.name)) continue;
        ProfileGap gap;
        gap.group = g.name;
        gap.title = g.title;
        gap.why = g.why;
        for (const ProfileKeyInfo& k : profileKeys()) if (k.group == g.name) gap.keys.push_back(k.name);
        gaps.push_back(gap);
    }
    return gaps;
}

// ---- text editing --------------------------------------------------------------------------------------------------------------------
namespace {

struct Line { std::string text, eol; };

std::vector<Line> splitLines(const std::string& text) {
    std::vector<Line> v;
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        Line l;
        if (nl == std::string::npos) { l.text = text.substr(at); at = text.size(); }
        else { l.text = text.substr(at, nl - at); l.eol = "\n"; at = nl + 1; }
        if (!l.text.empty() && l.text.back() == '\r') { l.text.pop_back(); l.eol = "\r\n"; }
        v.push_back(l);
    }
    return v;
}

// the key of a "key = value" line (lower case), or ""
std::string lineKey(const std::string& line) {
    std::string code = trim(stripComment(line));
    if (code.empty() || code[0] == '[') return "";
    size_t eq = code.find('=');
    return eq == std::string::npos ? "" : lower(trim(code.substr(0, eq)));
}

}  // namespace

std::string profileGetKey(const std::string& text, const std::string& key, bool& found) {
    found = false;
    std::string value;
    for (const Line& l : splitLines(text)) {
        if (lineKey(l.text) != lower(key)) continue;
        std::string code = trim(stripComment(l.text));
        value = trim(code.substr(code.find('=') + 1));
        found = true;
    }
    return value;
}

std::string profileUpdateKey(const std::string& text, const std::string& key, const std::string& value) {
    std::vector<Line> lines = splitLines(text);
    int last = -1;
    for (size_t i = 0; i < lines.size(); i++) if (lineKey(lines[i].text) == lower(key)) last = (int)i;
    if (last >= 0) {
        const std::string& old = lines[last].text;
        size_t first = old.find_first_not_of(" \t");
        std::string indent = first == std::string::npos ? "" : old.substr(0, first);
        std::string tail;                                             // the comment, with the spacing in front of it
        size_t semi = stripComment(old).size();
        if (semi < old.size()) {
            size_t sp = semi;
            while (sp > 0 && (old[sp - 1] == ' ' || old[sp - 1] == '\t')) sp--;
            tail = old.substr(sp);
        }
        const std::string name = trim(old.substr(first == std::string::npos ? 0 : first, old.find('=') - (first == std::string::npos ? 0 : first)));
        lines[last].text = indent + name + " = " + value + tail;
    } else {
        if (!lines.empty() && lines.back().eol.empty()) lines.back().eol = "\n";
        lines.push_back({key + " = " + value, "\n"});
    }
    std::string out;
    for (const Line& l : lines) out += l.text + l.eol;
    return out;
}

std::string profileRemoveKey(const std::string& text, const std::string& key) {
    std::string out;
    for (const Line& l : splitLines(text)) if (lineKey(l.text) != lower(key)) out += l.text + l.eol;
    return out;
}

}  // namespace asmcore
