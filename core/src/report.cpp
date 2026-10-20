// Text and JSON outputs: listing, map, debug information, Intel HEX, diagnostics.
#include <algorithm>
#include <cstdio>
#include <sstream>

#include "codec.h"
#include "internal.h"

namespace asmcore {

namespace {

using namespace isa;

std::string hex(uint32_t v, int digits) {
    char b[16];
    std::snprintf(b, sizeof b, "%0*X", digits, v);
    return b;
}

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char ch : s) {
        unsigned char c = (unsigned char)ch;
        switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += ch;
        }
    }
    return o;
}

std::string regName(int id) { return id >= 0 && id <= 8 ? kRegions[id].name : "?"; }

std::string condName(const NamedCode* tab, size_t n, int code) {
    for (size_t i = 0; i < n; i++) if (tab[i].code == code) return tab[i].name;
    return "0x" + hex((uint32_t)code, 1);
}

// one-line decode of a descriptor, for the listing
std::string decode(const std::string& table, uint32_t v) {
    char b[160];
    b[0] = 0;
    if (table == "MainMoveDesc_32b") {
        codec::Move m = codec::unpackMove(v);
        std::snprintf(b, sizeof b, "%s[%d..%d] -> %s[%d..%d]", regName(m.srcRegion).c_str(), m.srcOff, m.srcOff + m.len - 1, regName(m.dstRegion).c_str(), m.dstOff, m.dstOff + m.len - 1);
    } else if (table == "MainBranchDesc_16b") {
        codec::MainBranch br = codec::unpackMainBranch(v);
        if (br.type == MB_COND) std::snprintf(b, sizeof b, "BR.COND %s -> %02X", condName(kMainConds, sizeof(kMainConds) / sizeof(kMainConds[0]), br.arg).c_str(), br.target);
        else if (br.type == MB_CASCADE_BEGIN) std::snprintf(b, sizeof b, "CASCADE_BEGIN sel=%d block0=%02X", br.arg, br.target);
        else if (br.type == MB_CASCADE_END) std::snprintf(b, sizeof b, "CASCADE_END idx=%d exit=%02X", br.arg, br.target);
        else std::snprintf(b, sizeof b, "reserved");
    } else if (table == "PeriBranchDesc_16b") {
        codec::PeriBranch br = codec::unpackPeriBranch(v);
        std::snprintf(b, sizeof b, "BR.%s -> %02X", condName(kPeriConds, sizeof(kPeriConds) / sizeof(kPeriConds[0]), br.type).c_str(), br.target);
    } else if (table == "MainCsrOneByteDesc_16b" || table == "PeriCsrOneByteDesc_16b") {
        static const char* nm[] = {"WRITE8", "RMW_SET8", "RMW_CLR8", "READ8"};
        codec::Csr1 c = codec::unpackCsr1(v);
        std::snprintf(b, sizeof b, "%s csr=%03X", nm[c.op], c.addr);
    } else if (table == "MainCsrMultiByteDesc_16b") {
        codec::CsrM c = codec::unpackCsrM(v);
        std::snprintf(b, sizeof b, "%s page=%X len=%d ptr=%d", c.op == 3 ? "READ_BUF" : c.op == 0 ? "WRITE_SP" : "reserved", c.page, c.len, c.ptr);
    } else if (table == "MainAluDesc_8b") {
        static const char* cls[] = {"ARITH", "CMP", "LOGIC", "SHIFT"};
        std::snprintf(b, sizeof b, "%s arg=%02X", cls[codec::unpackAlu(v, AluCodes()).cls], v & 63);
    } else if (table == "PeriXferOneByteDesc_16b") {
        codec::Xfer1 x = codec::unpackXfer1(v);
        std::snprintf(b, sizeof b, "%s dev=%d addrlen=%d addr=%03X", x.rw ? "READ" : "WRITE", x.dev, x.addrLen, x.addr);
    } else if (table == "PeriXferMultiByteDesc_16b") {
        codec::XferM x = codec::unpackXferM(v);
        std::snprintf(b, sizeof b, "%s dev=%d addrlen=%d len=%d ptr=%d", x.rw ? "READ" : "WRITE", x.dev, x.addrLen, x.len, x.ptr);
    } else if (table == "PeriPublishDesc_16b") {
        codec::Pub p = codec::unpackPub(v);
        std::snprintf(b, sizeof b, "service=%d src=%d len=%d", p.service, p.src, p.len);
    }
    return b;
}

}  // namespace

std::string Result::diagnosticsText() const {
    std::ostringstream o;
    for (const Diagnostic& d : diags) {
        if (!d.loc.file.empty()) o << d.loc.file << ":" << d.loc.line << ": ";
        o << (d.sev == Diagnostic::Error ? "error " : d.sev == Diagnostic::Warning ? "warning " : "note ") << d.code << ": " << d.msg << "\n";
    }
    return o.str();
}

std::string Result::listing() const {
    std::ostringstream o;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = image(d);
        if (!im.present) continue;
        o << "; ==== " << domainName(d) << " : code " << im.codeSize << " bytes, image " << im.bytes.size() << " bytes (call model: " << callModel << ") ====\n";
        o << "; PC    BYTES          CYC  SOURCE\n";
        for (const InstrInfo& ii : instrs) {
            if (ii.domain != d) continue;
            std::string by;
            for (uint32_t i = 0; i < ii.length && ii.pc + i < im.bytes.size(); i++) by += hex(im.bytes[ii.pc + i], 2) + " ";
            char line[512];
            std::string origin;
            if (!ii.frames.empty()) {
                origin = "   ; in";
                for (const Frame& f : ii.frames) origin += " " + f.proc + "@" + std::to_string(f.callSite.line);
            }
            if (!ii.note.empty()) origin += "   ; " + ii.note;
            std::snprintf(line, sizeof line, "%s  %-13s %3d  %-5d %s%s\n", hex(ii.pc, 4).c_str(), by.c_str(), ii.cycles, ii.loc.line, ii.text.c_str(), origin.c_str());
            o << line;
        }
        o << "\n; ---- tables ----\n";
        for (const SectionInfo& s : im.sections) {
            if (s.name == "code") continue;
            o << "; " << s.name << " @" << hex(s.offset, 4) << " (" << s.entries << " x " << s.entryBytes << " bytes)\n";
            if (s.name.find("Desc_") != std::string::npos) {
                for (int i = 0; i < s.entries; i++) {
                    uint32_t v = 0;
                    for (int k = 0; k < s.entryBytes; k++) v |= (uint32_t)im.bytes[s.offset + i * s.entryBytes + k] << (profile.bigEndian ? 8 * (s.entryBytes - 1 - k) : 8 * k);
                    o << ";   [" << i << "] " << hex(v, s.entryBytes * 2) << "  " << decode(s.name, v) << "\n";
                }
            } else if (s.name == "MAIN_CONST_POOL" || s.name == "DeviceMap") {
                o << ";   ";
                for (uint32_t k = 0; k < s.size; k++) o << hex(im.bytes[s.offset + k], 2) << " ";
                o << "\n";
            }
        }
        o << "\n; ---- ImageLayout: sideband latches, NOT part of the image bytes (logical byte offsets) ----\n";
        const char* const* fn = layoutFieldNames(d);
        for (size_t i = 0; i < im.layout.size() && i < 6; i++) o << ";   " << fn[i] << " = " << im.layout[i] << " (0x" << hex(im.layout[i], 4) << ")\n";
        o << "; ---- physical placement: shared SRAM, words " << im.baseWord << ".." << (im.words.empty() ? im.baseWord : im.baseWord + (uint32_t)im.words.size() - 1) << " of the window [" << im.baseWord << ", " << im.baseWord + im.reservedWords << ") ----\n";
        o << "\n";
    }
    return o.str();
}

std::string Result::map() const {
    std::ostringstream o;
    o << "call model: " << callModel << ", max call depth " << maxCallDepth << "\n";
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = image(d);
        if (!im.present) continue;
        o << "\n" << domainName(d) << " image: " << im.bytes.size() << " bytes\n";
        o << "  offset  size   entries  use      section\n";
        for (const SectionInfo& s : im.sections) {
            char line[256];
            std::string use = s.depth ? std::to_string(s.entries) + "/" + std::to_string(s.depth) : "";
            std::snprintf(line, sizeof line, "  %s  %-5u  %-7d  %-8s %s\n", hex(s.offset, 4).c_str(), s.size, s.entries, use.c_str(), s.name.c_str());
            o << line;
        }
    }
    if (!procs.empty()) {
        o << "\nprocedures\n";
        for (const ProcInfo& p : procs) {
            o << "  " << domainName(p.domain) << " " << p.name << "  call sites " << p.callSites;
            if (p.emitted) o << "  code " << hex(p.startPc, 4) << ".." << hex(p.endPc, 4) << (p.slot >= 0 ? "  return slot SCRATCHPAD[" + std::to_string(p.slot) + ".." + std::to_string(p.slot + 1) + "]" : "");
            else o << "  (inlined)";
            o << "\n";
        }
    }
    o << "\nsymbols\n";
    for (const SymbolInfo& s : symbols) o << "  " << s.kind << "  " << s.name << " = " << s.value << (s.kind == "label" ? " (0x" + hex((uint32_t)s.value, 4) + ")" : (s.value >= 0 ? " (0x" + hex((uint32_t)s.value, 1) + ")" : "")) << "\n";
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const EventTiming& t = eventTiming[domIdx(d)];
        if (!t.present) continue;
        o << "\nevents (" << domainName(d) << ", placeholder latencies; " << t.checkpoints << " event_safe branch(es))\n";
        for (const VectorInfo& v : vectors) if (v.domain == d) o << "  vector " << v.id << " -> " << v.label << " @" << hex(v.pc, 4) << "\n";
        o << "  pending -> next checkpoint: " << (t.checkpointBounded ? std::to_string(t.checkpointCycles) + " cycles" : "unbounded") << "\n  dispatch: " << t.dispatchCycles << "   ERET + refetch: " << t.eretCycles << "\n";
        for (const EventTiming::Handler& h : t.handlers) o << "  handler " << h.id << " (" << h.label << "): " << (h.bounded ? std::to_string(h.cycles) + " cycles" : "unbounded") << "\n";
        o << "  response (excluding detection-to-pending): " << (t.bounded ? std::to_string(t.worstCycles) + " cycles" : "no upper bound") << "\n  " << t.note << "\n";
    }
    if (!wcet.empty()) {
        o << "\nWCET (placeholder latencies)\n";
        for (const WcetResult& w : wcet) o << "  " << w.name << "  " << w.from << " -> " << w.to << "  " << (w.bounded ? std::to_string(w.cycles) + " cycles" : "unbounded") << "  (" << w.note << ")\n";
    }
    return o.str();
}

std::string Result::intelHex(Domain d) const {
    const DomainImage& im = image(d);
    std::ostringstream o;
    const std::vector<uint8_t>& b = im.bytes;
    for (size_t a = 0; a < b.size(); a += 16) {
        size_t n = std::min<size_t>(16, b.size() - a);
        unsigned sum = (unsigned)n + ((a >> 8) & 255) + (a & 255);
        o << ":" << hex((uint32_t)n, 2) << hex((uint32_t)a, 4) << "00";
        for (size_t i = 0; i < n; i++) { o << hex(b[a + i], 2); sum += b[a + i]; }
        o << hex((256 - (sum & 255)) & 255, 2) << "\n";
    }
    o << ":00000001FF\n";
    return o.str();
}

std::string Result::debugJson() const {
    std::ostringstream o;
    auto loc = [&](const SrcLoc& l) { return "\"file\":\"" + jsonEsc(l.file) + "\",\"line\":" + std::to_string(l.line); };
    o << "{\n \"tool\":\"asmcore\",\"isa\":\"Sensor Micro-Sequencer ISA v1.6 r10\",\"callModel\":\"" << callModel << "\",\"maxCallDepth\":" << maxCallDepth << ",\n";
    o << " \"domains\":[";
    bool first = true;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = image(d);
        if (!im.present) continue;
        o << (first ? "" : ",") << "\n  {\"name\":\"" << domainName(d) << "\",\"codeSize\":" << im.codeSize << ",\"imageSize\":" << im.bytes.size() << ",\"baseWord\":" << im.baseWord << ",\"reservedWords\":" << im.reservedWords << ",\"layout\":[";
        for (size_t i = 0; i < im.layout.size(); i++) o << (i ? "," : "") << im.layout[i];
        o << "],\"sections\":[";
        for (size_t i = 0; i < im.sections.size(); i++) {
            const SectionInfo& s = im.sections[i];
            o << (i ? "," : "") << "{\"name\":\"" << s.name << "\",\"offset\":" << s.offset << ",\"size\":" << s.size << ",\"entries\":" << s.entries << ",\"depth\":" << s.depth << "}";
        }
        o << "]}";
        first = false;
    }
    o << "\n ],\n \"instrs\":[";
    for (size_t i = 0; i < instrs.size(); i++) {
        const InstrInfo& ii = instrs[i];
        o << (i ? "," : "") << "\n  {\"domain\":\"" << domainName(ii.domain) << "\",\"pc\":" << ii.pc << ",\"len\":" << ii.length << "," << loc(ii.loc) << ",\"text\":\"" << jsonEsc(ii.text) << "\",\"cycles\":" << ii.cycles
          << ",\"synthetic\":" << (ii.synthetic ? "true" : "false");
        if (!ii.note.empty()) o << ",\"note\":\"" << jsonEsc(ii.note) << "\"";
        if (!ii.table.empty()) o << ",\"table\":\"" << ii.table << "\",\"desc\":" << ii.descIndex;
        if (ii.isBranch) o << ",\"branch\":true,\"target\":" << ii.target;
        if (!ii.fallsThrough) o << ",\"fall\":false";
        if (!ii.frames.empty()) {
            o << ",\"frames\":[";
            for (size_t k = 0; k < ii.frames.size(); k++) o << (k ? "," : "") << "{\"proc\":\"" << jsonEsc(ii.frames[k].proc) << "\"," << loc(ii.frames[k].callSite) << "}";
            o << "]";
        }
        o << "}";
    }
    o << "\n ],\n \"procs\":[";
    for (size_t i = 0; i < procs.size(); i++) {
        const ProcInfo& p = procs[i];
        o << (i ? "," : "") << "\n  {\"domain\":\"" << domainName(p.domain) << "\",\"name\":\"" << jsonEsc(p.name) << "\"," << loc(p.loc) << ",\"emitted\":" << (p.emitted ? "true" : "false") << ",\"startPc\":" << p.startPc << ",\"endPc\":" << p.endPc << ",\"callSites\":" << p.callSites << ",\"slot\":" << p.slot << ",\"wcetBounded\":" << (p.wcetBounded ? "true" : "false") << ",\"wcetCycles\":" << p.wcetCycles << "}";
    }
    o << "\n ],\n \"callSites\":[";
    for (size_t i = 0; i < callSites.size(); i++) {
        const CallSiteInfo& cs = callSites[i];
        o << (i ? "," : "") << "\n  {\"domain\":\"" << domainName(cs.domain) << "\",\"callee\":\"" << jsonEsc(cs.callee) << "\"," << loc(cs.loc) << ",\"transferPc\":" << cs.transferPc << ",\"returnPc\":" << cs.returnPc << ",\"retId\":" << cs.retId << ",\"caller\":\"" << jsonEsc(cs.caller) << "\",\"wcetBounded\":" << (cs.wcetBounded ? "true" : "false") << ",\"wcetCycles\":" << cs.wcetCycles << "}";
    }
    o << "\n ],\n \"symbols\":[";
    for (size_t i = 0; i < symbols.size(); i++) {
        const SymbolInfo& s = symbols[i];
        o << (i ? "," : "") << "\n  {\"name\":\"" << jsonEsc(s.name) << "\",\"kind\":\"" << s.kind << "\",\"value\":" << s.value << "," << loc(s.loc) << "}";
    }
    o << "\n ],\n \"vectors\":[";
    for (size_t i = 0; i < vectors.size(); i++)
        o << (i ? "," : "") << "\n  {\"domain\":\"" << domainName(vectors[i].domain) << "\",\"id\":" << vectors[i].id << ",\"label\":\"" << jsonEsc(vectors[i].label) << "\",\"pc\":" << vectors[i].pc << "," << loc(vectors[i].loc) << "}";
    o << "\n ],\n \"wcet\":[";
    for (size_t i = 0; i < wcet.size(); i++) {
        const WcetResult& w = wcet[i];
        o << (i ? "," : "") << "\n  {\"name\":\"" << jsonEsc(w.name) << "\",\"from\":\"" << jsonEsc(w.from) << "\",\"to\":\"" << jsonEsc(w.to) << "\",\"bounded\":" << (w.bounded ? "true" : "false") << ",\"cycles\":" << w.cycles << ",\"note\":\"" << jsonEsc(w.note) << "\"}";
    }
    o << "\n ]\n}\n";
    return o.str();
}

}  // namespace asmcore
