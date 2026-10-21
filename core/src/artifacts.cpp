// Build artifacts of spec v31 (handoff 11): layout, section map, descriptor map, SRAM words, boot plan, memory proof, services, events,
// validation report, manifest. Everything here is derived from a finished Result; nothing is re-assembled.
#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

#include "asmcore/diagnostics.h"
#include "asmcore/profile_info.h"
#include "asmcore/profile_info.h"
#include "asmcore/sram.h"
#include "codec.h"
#include "internal.h"
#include "util.h"

namespace asmcore {

namespace {

std::string hx(uint64_t v, int digits) {
    char b[24];
    std::snprintf(b, sizeof b, "%0*llX", digits, (unsigned long long)v);
    return b;
}
std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }
std::string fnv1a64(const std::string& data) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : data) { h ^= c; h *= 1099511628211ULL; }
    return hx(h, 16);
}
std::string bytesHex(const std::vector<uint8_t>& b, size_t from, size_t n) {
    std::string s;
    for (size_t i = from; i < from + n && i < b.size(); i++) s += (i > from ? " " : "") + hx(b[i], 2);
    return s;
}
const char* const kSpec = "MicroISA-v31";

uint32_t entryValue(const DomainImage& im, const SectionInfo& s, int idx, bool big) {
    uint32_t v = 0;
    for (int k = 0; k < s.entryBytes; k++) {
        size_t at = (size_t)s.offset + (size_t)idx * (size_t)s.entryBytes + (size_t)k;
        if (at >= im.bytes.size()) break;
        int sh = big ? 8 * (s.entryBytes - 1 - k) : 8 * k;
        v |= (uint32_t)im.bytes[at] << sh;
    }
    return v;
}

}  // namespace

// ---- layout.json -----------------------------------------------------------------------------------------------------------------------
std::string Result::layoutJson(Domain d) const {
    const DomainImage& im = image(d);
    std::ostringstream o;
    o << "{\n \"spec\":\"" << kSpec << "\",\n \"unit\":\"logical_byte_offset\",\n \"domain\":\"" << domainName(d) << "\",\n \"profile\":" << q(profile.profileId) << ",\n";
    o << " \"logical_code_start\":0,\n \"code_bytes\":" << im.codeSize << ",\n \"logical_image_bytes\":" << im.bytes.size() << ",\n \"sideband_field_bits\":" << profile.layoutOffsetBits << ",\n";
    o << " \"fields\":{";
    const char* const* names = layoutFieldNames(d);
    for (size_t i = 0; i < im.layout.size() && i < 6; i++) o << (i ? "," : "") << "\n  \"" << names[i] << "\":" << im.layout[i];
    o << "\n },\n \"sections\":[";
    for (size_t i = 0; i < im.sections.size(); i++) {
        const SectionInfo& s = im.sections[i];
        o << (i ? "," : "") << "\n  {\"name\":" << q(s.name) << ",\"offset\":" << s.offset << ",\"size\":" << s.size << ",\"entries\":" << s.entries << ",\"entry_bytes\":" << s.entryBytes << ",\"depth\":" << s.depth << "}";
    }
    o << "\n ],\n \"note\":\"ImageLayout is delivered to the independent sideband latches; it is NOT part of the image bytes and is never prepended or appended to them. Values are logical BYTE offsets, not physical word addresses.\"\n}\n";
    return o.str();
}

// ---- section_map.json ------------------------------------------------------------------------------------------------------------------
std::string Result::sectionMapJson() const {
    std::ostringstream o;
    o << "{\n \"spec\":\"" << kSpec << "\",\n \"profile\":" << q(profile.profileId) << ",\n \"domains\":[";
    bool first = true;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = image(d);
        if (!im.present) continue;
        o << (first ? "" : ",") << "\n  {\"domain\":\"" << domainName(d) << "\",\"logical_image_bytes\":" << im.bytes.size() << ",\"spans\":[";
        uint32_t at = 0;
        bool f = true;
        auto span = [&](const char* kind, const std::string& name, uint32_t off, uint32_t size) {
            o << (f ? "" : ",") << "\n   {\"kind\":\"" << kind << "\",\"name\":" << q(name) << ",\"offset\":" << off << ",\"end_exclusive\":" << off + size << ",\"size\":" << size << "}";
            f = false;
        };
        for (const SectionInfo& s : im.sections) {
            if (s.offset > at) span("padding", "padding", at, s.offset - at);
            span(s.name == "code" ? "code" : "section", s.name, s.offset, s.size);
            at = std::max(at, s.offset + s.size);
        }
        if (at < im.bytes.size()) span("padding", "padding", at, (uint32_t)im.bytes.size() - at);
        const uint32_t used = (uint32_t)im.words.size();
        o << "\n  ],\"physical\":{\"base_word\":" << im.baseWord << ",\"occupied_words\":" << used << ",\"reserved_words\":" << im.reservedWords << ",\"padding_bytes_in_last_word\":" << (used * 4 - im.bytes.size()) << "}}";
        first = false;
    }
    o << "\n ]\n}\n";
    return o.str();
}

// ---- descriptor_map.json ---------------------------------------------------------------------------------------------------------------
std::string Result::descriptorMapJson() const {
    std::ostringstream o;
    o << "{\n \"spec\":\"" << kSpec << "\",\n \"descriptor_byte_order\":\"" << (profile.bigEndian ? "big" : "little") << "\",\n \"domains\":[";
    bool firstD = true;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = image(d);
        if (!im.present) continue;
        o << (firstD ? "" : ",") << "\n  {\"domain\":\"" << domainName(d) << "\",\"tables\":[";
        firstD = false;
        bool firstT = true;
        for (const SectionInfo& s : im.sections) {
            if (s.name == "code") continue;
            o << (firstT ? "" : ",") << "\n   {\"table\":" << q(s.name) << ",\"offset\":" << s.offset << ",\"entry_bytes\":" << s.entryBytes << ",\"count\":" << s.entries << ",\"depth\":" << s.depth << ",";
            firstT = false;
            if (s.name == "MAIN_CONST_POOL") {
                o << "\"bytes\":\"" << bytesHex(im.bytes, s.offset, s.size) << "\"}";
                continue;
            }
            o << "\"entries\":[";
            for (int i = 0; i < s.entries; i++) {
                const uint32_t v = entryValue(im, s, i, profile.bigEndian);
                o << (i ? "," : "") << "\n    {\"index\":" << i << ",\"bytes\":\"" << bytesHex(im.bytes, s.offset + (uint32_t)i * (uint32_t)s.entryBytes, (size_t)s.entryBytes) << "\",\"value\":" << v;
                if (s.name == "DeviceMap") {
                    codec::DevEntry e = codec::unpackDev(profile, v);
                    o << ",\"bus_type\":" << e.busType << ",\"i2c_addr_7b\":" << e.addr7;
                } else {
                    std::string text;
                    std::ostringstream users;
                    int n = 0;
                    for (const InstrInfo& in : instrs) {
                        if (in.domain != d || in.table != s.name || in.descIndex != i) continue;
                        if (text.empty()) text = in.text;
                        users << (n++ ? "," : "") << "{\"file\":" << q(in.loc.file) << ",\"line\":" << in.loc.line << ",\"pc\":" << in.pc << "}";
                    }
                    o << ",\"decoded\":" << q(text) << ",\"used\":" << (n ? "true" : "false") << ",\"users\":[" << users.str() << "]";
                }
                o << "}";
            }
            o << "\n   ]}";
        }
        o << "\n  ]}";
    }
    o << "\n ]\n}\n";
    return o.str();
}

// ---- shared_program_sram_words.hex -----------------------------------------------------------------------------------------------------
std::string Result::sramWordsHex() const {
    std::ostringstream o;
    o << "// shared program SRAM of profile " << profile.profileId << ": one 32-bit synchronous SRAM (clock = 2 x ISA clock) holds both logical images.\n";
    o << "// Each line: physical WORD address, 32-bit data (logical byte +0 in bits [7:0]). Main and Peri never share a word; the last word of an image is padded with 0x"
      << hx((uint32_t)profile.bootPadByte, 2) << ".\n";
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = image(d);
        if (!im.present) continue;
        o << "// " << domainName(d) << ": words " << im.baseWord << ".." << (im.words.empty() ? im.baseWord : im.baseWord + (uint32_t)im.words.size() - 1) << " of window [" << im.baseWord << ", " << im.baseWord + im.reservedWords << ")\n";
    }
    for (const WordWrite& w : sramWrites(*this)) o << hx(w.address, 4) << " " << hx(w.data, 8) << " // " << domainName(w.domain) << "\n";
    return o.str();
}

// ---- boot_load_plan.json ---------------------------------------------------------------------------------------------------------------
std::string Result::bootPlanJson() const {
    std::ostringstream o;
    o << "{\n \"spec\":\"" << kSpec << "\",\n \"profile\":" << q(profile.profileId) << ",\n";
    o << " \"sequence\":[\"hold the Main and Peri sequencers in reset / halt (the program SRAM holds nothing valid after power-on)\","
         "\"write every 32-bit word of the table 'sram_writes' to the ONE shared SRAM (no byte write enables)\","
         "\"load MainImageLayout and PeriImageLayout into their independent sideband latches ('sideband_writes')\","
         "\"assert image_valid for each loaded image, lock the shared SRAM write path, clear the runtime state of both Frontends\","
         "\"release the Main and Peri sequencers: both start at logical PC 0\"],\n";
    o << " \"pad_byte\":" << profile.bootPadByte << ",\n \"sram_writes\":[";
    std::vector<WordWrite> w = sramWrites(*this);
    for (size_t i = 0; i < w.size(); i++) o << (i ? "," : "") << "\n  {\"domain\":\"" << domainName(w[i].domain) << "\",\"word\":" << w[i].address << ",\"word_hex\":\"0x" << hx(w[i].address, 4) << "\",\"data\":\"0x" << hx(w[i].data, 8) << "\"}";
    o << "\n ],\n \"sideband_writes\":[";
    bool first = true;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = image(d);
        if (!im.present) continue;
        const char* const* names = layoutFieldNames(d);
        for (size_t i = 0; i < im.layout.size() && i < 6; i++) {
            o << (first ? "" : ",") << "\n  {\"domain\":\"" << domainName(d) << "\",\"field\":\"" << names[i] << "\",\"value\":" << im.layout[i] << ",\"bits\":" << profile.layoutOffsetBits << "}";
            first = false;
        }
    }
    o << "\n ],\n \"notes\":[\"Only the DATA is fixed here. The order and protocol of the sideband loads, the write ports and the lock/valid handshake are defined by the target profile / boot interface contract.\","
         "\"The external ROM container (how the images and sideband fields are stored and checked) is boot / product integration, not part of the ISA; no header is invented here.\"]\n}\n";
    return o.str();
}

// ---- memory_proof.json -----------------------------------------------------------------------------------------------------------------
std::string Result::memoryProofJson() const {
    MemoryProof p = memoryProof(*this, profile);
    std::ostringstream o;
    auto win = [&](const MemoryProof::Window& w) {
        o << "{\"domain\":\"" << w.domain << "\",\"base_word\":" << w.baseWord << ",\"occupied_words\":" << w.occupiedWords << ",\"occupied_range\":[" << w.baseWord << "," << w.baseWord + w.occupiedWords << "]"
          << ",\"reserved_words\":" << w.reservedWords << ",\"reserved_window\":[" << w.baseWord << "," << w.baseWord + w.reservedWords << "]"
          << ",\"logical_bytes\":" << w.logicalBytes << ",\"capacity_bytes\":" << w.capacityBytes << "}";
    };
    o << "{\n \"spec\":\"" << kSpec << "\",\n \"profile\":" << q(profile.profileId) << ",\n \"sram_depth_words\":" << p.depthWords << ",\n \"unit\":\"32-bit words; ranges are half-open [first, end)\",\n \"main\":";
    win(p.main);
    o << ",\n \"peri\":";
    win(p.peri);
    o << ",\n \"checks\":{\"occupied_ranges_disjoint\":" << (p.occupiedDisjoint ? "true" : "false") << ",\"reserved_windows_disjoint\":" << (p.reservedDisjoint ? "true" : "false")
      << ",\"windows_inside_sram\":" << (p.insideDepth ? "true" : "false") << ",\"images_within_capacity\":" << (p.withinCapacity ? "true" : "false") << "},\n \"ok\":" << (p.ok() ? "true" : "false") << ",\n \"notes\":[";
    for (size_t i = 0; i < p.notes.size(); i++) o << (i ? "," : "") << q(p.notes[i]);
    o << "]\n}\n";
    return o.str();
}

// ---- services.json ---------------------------------------------------------------------------------------------------------------------
std::string Result::servicesJson() const {
    std::ostringstream o;
    o << "{\n \"spec\":\"" << kSpec << "\",\n \"drop_policy\":\"keep_old_drop_new\",\n \"services\":[";
    bool first = true;
    for (const SymbolInfo& s : symbols) {
        if (s.kind != "service") continue;
        o << (first ? "" : ",") << "\n  {\"name\":" << q(s.name) << ",\"service_id\":" << s.value << ",\"publishes\":[";
        first = false;
        bool fp = true;
        if (peri.present)
            for (const InstrInfo& in : instrs) {
                if (in.domain != Domain::Peri || in.table != "PeriPublishDesc_16b") continue;
                for (const SectionInfo& sec : peri.sections) {
                    if (sec.name != in.table) continue;
                    codec::Pub pb = codec::unpackPub(entryValue(peri, sec, in.descIndex, profile.bigEndian));
                    if (pb.service != s.value) continue;
                    o << (fp ? "" : ",") << "{\"src_ptr\":" << pb.src << ",\"len\":" << pb.len << ",\"file\":" << q(in.loc.file) << ",\"line\":" << in.loc.line << "}";
                    fp = false;
                }
            }
        o << "]}";
    }
    o << "\n ],\n \"note\":\"service_id is the logical payload contract of the ResultSlot (1..15); dev_sel is the physical endpoint. The slot has no payload length or result type: the Main handler must know both from this contract.\"\n}\n";
    return o.str();
}

// ---- event_vectors.json ----------------------------------------------------------------------------------------------------------------
std::string Result::eventVectorsJson() const {
    std::ostringstream o;
    o << "{\n \"spec\":\"" << kSpec << "\",\n \"vectors\":[";
    for (size_t i = 0; i < vectors.size(); i++) {
        const VectorInfo& v = vectors[i];
        o << (i ? "," : "") << "\n  {\"domain\":\"" << domainName(v.domain) << "\",\"event_id\":" << v.id << ",\"label\":" << q(v.label) << ",\"logical_pc\":" << v.pc << ",\"file\":" << q(v.loc.file) << ",\"line\":" << v.loc.line << "}";
    }
    o << "\n ],\n \"timing\":[";
    bool first = true;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const EventTiming& t = eventTiming[domIdx(d)];
        if (!t.present) continue;
        o << (first ? "" : ",") << "\n  {\"domain\":\"" << domainName(d) << "\",\"event_safe_branches\":" << t.checkpoints << ",\"pending_to_checkpoint\":" << (t.checkpointBounded ? std::to_string(t.checkpointCycles) : std::string("null"))
          << ",\"dispatch\":" << t.dispatchCycles << ",\"eret_and_refetch\":" << t.eretCycles << ",\"handlers\":[";
        for (size_t i = 0; i < t.handlers.size(); i++)
            o << (i ? "," : "") << "{\"event_id\":" << t.handlers[i].id << ",\"label\":" << q(t.handlers[i].label) << ",\"handler_cycles\":" << (t.handlers[i].bounded ? std::to_string(t.handlers[i].cycles) : std::string("null")) << "}";
        o << "],\"bounded\":" << (t.bounded ? "true" : "false") << ",\"worst_response_cycles\":" << (t.bounded ? std::to_string(t.worstCycles) : std::string("null")) << ",\"note\":" << q(t.note) << "}";
        first = false;
    }
    o << "\n ],\n \"note\":\"Event vector sources, numbering, priority and loading are defined by the target profile; this lists the vectors the program declares with .vector. A null value means: no upper bound can be shown (do not claim a response time).\"\n}\n";
    return o.str();
}

// ---- validation_report.json ------------------------------------------------------------------------------------------------------------
std::string Result::validationReportJson() const {
    std::ostringstream o;
    o << "{\n \"spec\":\"" << kSpec << "\",\n \"profile\":" << q(profile.profileId) << ",\n \"ok\":" << (ok ? "true" : "false") << ",\n \"errors\":" << errorCount() << ",\n \"warnings\":" << warningCount() << ",\n \"diagnostics\":[";
    for (size_t i = 0; i < diags.size(); i++) {
        const Diagnostic& d = diags[i];
        o << (i ? "," : "") << "\n  {\"severity\":\"" << (d.sev == Diagnostic::Error ? "error" : d.sev == Diagnostic::Warning ? "warning" : "note") << "\",\"code\":" << q(d.code) << ",\"file\":" << q(d.loc.file) << ",\"line\":" << d.loc.line
          << ",\"col\":" << d.loc.col << ",\"end_col\":" << d.loc.endCol << ",\"message\":" << q(d.msg);
        if (const DiagInfo* info = diagInfo(d.code)) { if (info->specName[0]) o << ",\"spec_name\":" << q(info->specName); o << ",\"fix\":" << q(info->fix); }
        o << "}";
    }
    o << "\n ]\n}\n";
    return o.str();
}

// ---- build_manifest.json ---------------------------------------------------------------------------------------------------------------
std::string Result::buildManifestJson(const std::vector<Source>& sources) const {
    std::ostringstream o;
    o << "{\n \"spec\":\"" << kSpec << "\",\n \"tool\":\"asmcore\",\n \"hash\":\"fnv1a64 (an integrity check, not a security hash)\",\n \"profile\":{\"id\":" << q(profile.profileId) << ",\"hash\":\"" << fnv1a64(profile.dump()) << "\",\"unconfirmed\":[";
    {
        const std::vector<ProfileGap> gaps = profileGaps(profile);
        for (size_t i = 0; i < gaps.size(); i++) o << (i ? "," : "") << q(gaps[i].group);
    }
    o << "],\"note\":\"groups listed in unconfirmed are planning values nobody has confirmed against the hardware: images, timing and WCET are planning-only\"},\n";
    o << " \"call_model\":\"" << callModel << "\",\n \"sources\":[";
    for (size_t i = 0; i < sources.size(); i++) o << (i ? "," : "") << "\n  {\"name\":" << q(sources[i].name) << ",\"bytes\":" << sources[i].text.size() << ",\"hash\":\"" << fnv1a64(sources[i].text) << "\"}";
    o << "\n ],\n \"images\":[";
    bool first = true;
    for (Domain d : {Domain::Main, Domain::Peri}) {
        const DomainImage& im = image(d);
        if (!im.present) continue;
        std::string layoutBytes;
        for (uint32_t v : im.layout) layoutBytes += std::to_string(v) + ",";
        o << (first ? "" : ",") << "\n  {\"domain\":\"" << domainName(d) << "\",\"logical_bytes\":" << im.bytes.size() << ",\"hash\":\"" << fnv1a64(std::string(im.bytes.begin(), im.bytes.end())) << "\",\"layout_hash\":\"" << fnv1a64(layoutBytes) << "\"}";
        first = false;
    }
    o << "\n ]\n}\n";
    return o.str();
}

// ---- reading a layout.json back --------------------------------------------------------------------------------------------------------
namespace {

struct Json {
    enum Kind { Null, Num, Str, Arr, Obj, Bool } kind = Null;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* get(const std::string& k) const { for (auto& kv : obj) if (kv.first == k) return &kv.second; return nullptr; }
};

struct JsonParser {
    const std::string& s;
    size_t i = 0;
    std::string err;
    explicit JsonParser(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; }
    bool fail(const std::string& m) { if (err.empty()) err = m + " at offset " + std::to_string(i); return false; }
    bool value(Json& j, int depth) {
        if (depth > 32) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            j.kind = Json::Obj;
            i++;
            ws();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            for (;;) {
                ws();
                Json k;
                if (!string(k)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("':' expected");
                i++;
                Json v;
                if (!value(v, depth + 1)) return false;
                j.obj.push_back({k.str, v});
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            j.kind = Json::Arr;
            i++;
            ws();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            for (;;) {
                Json v;
                if (!value(v, depth + 1)) return false;
                j.arr.push_back(v);
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return fail("',' or ']' expected");
            }
        }
        if (c == '"') return string(j);
        if (s.compare(i, 4, "true") == 0) { j.kind = Json::Bool; j.num = 1; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { j.kind = Json::Bool; j.num = 0; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; return true; }
        char* end = nullptr;
        double d = std::strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) return fail("a value expected");
        j.kind = Json::Num;
        j.num = d;
        i = (size_t)(end - s.c_str());
        return true;
    }
    bool string(Json& j) {
        if (i >= s.size() || s[i] != '"') return fail("a string expected");
        i++;
        j.kind = Json::Str;
        j.str.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                i++;
                switch (s[i]) { case 'n': j.str += '\n'; break; case 't': j.str += '\t'; break; case 'r': j.str += '\r'; break; case 'u': i += 4; j.str += '?'; break; default: j.str += s[i]; }
            } else j.str += s[i];
            i++;
        }
        if (i >= s.size()) return fail("unterminated string");
        i++;
        return true;
    }
};

}  // namespace

bool loadLayoutJson(const std::string& text, DomainImage& im, std::string& error) {
    JsonParser p(text);
    Json root;
    if (!p.value(root, 0) || root.kind != Json::Obj) { error = "not a layout file: " + (p.err.empty() ? std::string("a JSON object is expected") : p.err); return false; }
    if (const Json* dom = root.get("domain")) {
        if (dom->str != domainName(im.domain)) { error = "the layout file is for the " + dom->str + " domain, not " + domainName(im.domain); return false; }
    }
    const Json* fields = root.get("fields");
    if (!fields || fields->kind != Json::Obj) { error = "the layout file has no \"fields\" object (the six ImageLayout offsets)"; return false; }
    const char* const* names = layoutFieldNames(im.domain);
    im.layout.clear();
    for (int i = 0; i < 6; i++) {
        const Json* f = fields->get(names[i]);
        if (!f || f->kind != Json::Num || f->num < 0 || f->num > 4294967295.0) { error = std::string("the layout file lacks a valid ") + names[i]; return false; }
        im.layout.push_back((uint32_t)f->num);
    }
    im.sections.clear();
    if (const Json* secs = root.get("sections")) {
        for (const Json& s : secs->arr) {
            const Json* n = s.get("name");
            const Json* o = s.get("offset");
            const Json* z = s.get("size");
            if (!n || !o || !z) continue;
            SectionInfo si;
            si.name = n->str;
            si.offset = (uint32_t)o->num;
            si.size = (uint32_t)z->num;
            if (const Json* e = s.get("entries")) si.entries = (int)e->num;
            if (const Json* e = s.get("entry_bytes")) si.entryBytes = (int)e->num;
            if (const Json* e = s.get("depth")) si.depth = (int)e->num;
            im.sections.push_back(si);
        }
        for (const SectionInfo& s : im.sections) if (s.name == "code") im.codeSize = s.size;
    }
    return true;
}

}  // namespace asmcore
