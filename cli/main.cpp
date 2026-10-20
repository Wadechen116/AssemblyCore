// asmc -- command-line assembler for the Sensor Micro-Sequencer ISA v1.6 r10.
// The only file in the project that touches the file system; the core library works on text.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "asmcore/asmcore.h"

namespace fs = std::filesystem;

namespace {

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}
bool writeFile(const fs::path& p, const void* data, size_t n) {
    std::ofstream o(p, std::ios::binary);
    if (!o) return false;
    o.write((const char*)data, (std::streamsize)n);
    return (bool)o;
}
bool writeText(const fs::path& p, const std::string& s) { return writeFile(p, s.data(), s.size()); }

void usage() {
    std::puts(
        "usage: asmc [options] file.asm [file2.asm ...]\n"
        "  -o PREFIX            output name prefix (default: the first input without extension)\n"
        "  --call-model M       inline (default) | softret   how CALL / RET are lowered\n"
        "  --profile FILE       target profile (table depths, latencies, softret slots); see profiles/default.profile\n"
        "  -I DIR               include search directory (also the including file's directory)\n"
        "  --list               write PREFIX.lst   (listing with addresses, bytes, cycles, decoded descriptors)\n"
        "  --map                write PREFIX.map   (sections, table usage, procedures, symbols, WCET)\n"
        "  --dbg                write PREFIX.dbg.json (debug information for the simulator / IDE)\n"
        "  --hex                write PREFIX.main.hex / PREFIX.peri.hex (Intel HEX of the logical images)\n"
        "  --artifacts          write the v31 artifact set next to the images (see below)\n"
        "  --wcet A:B           worst-case cycles between Main labels A and B (repeatable)\n"
        "  --pad-tables         pad every descriptor table to the profile depth\n"
        "  --werror             warnings fail the build\n"
        "  --dataflow M         off | warn (default) | error   the data-flow check (docs/ASM_SYNTAX.md, W5xx)\n"
        "  --no-warn CODE       drop a warning code, e.g. W502 (repeatable)\n"
        "  --print-profile      print the effective profile and exit\n"
        "  -h, --help           this text\n"
        "Always writes PREFIX.main.bin / PREFIX.peri.bin (the LOGICAL images: code, descriptor tables, const pool / DeviceMap; no ImageLayout inside)\n"
        "and PREFIX.main.layout.json / PREFIX.peri.layout.json (the six ImageLayout offsets, delivered to the sideband latches). A .bin is meaningless\n"
        "without its layout.json: keep them together.\n"
        "--artifacts adds: PREFIX.sram.hex (the 32-bit words of the one shared program SRAM), PREFIX.boot.json (boot load plan), PREFIX.memory.json\n"
        "(placement proof), PREFIX.sections.json, PREFIX.descriptors.json, PREFIX.services.json, PREFIX.events.json, PREFIX.validation.json,\n"
        "PREFIX.manifest.json.   Exit status: 0 ok, 1 assembly errors, 2 usage / I/O error.");
}

}  // namespace

int main(int argc, char** argv) {
    asmcore::Options opt;
    std::vector<std::string> inputs, incDirs;
    std::string prefix, profilePath;
    bool list = false, map = false, dbg = false, hex = false, printProfile = false, artifacts = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "asmc: %s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "-o") prefix = need("-o");
        else if (a == "--call-model") {
            std::string m = need("--call-model");
            if (m == "inline") opt.callModel = asmcore::CallModel::Inline;
            else if (m == "softret") opt.callModel = asmcore::CallModel::SoftRet;
            else { std::fprintf(stderr, "asmc: --call-model must be inline or softret\n"); return 2; }
        }
        else if (a == "--profile") profilePath = need("--profile");
        else if (a == "-I") incDirs.push_back(need("-I"));
        else if (a == "--list") list = true;
        else if (a == "--map") map = true;
        else if (a == "--dbg") dbg = true;
        else if (a == "--hex") hex = true;
        else if (a == "--artifacts") artifacts = true;
        else if (a == "--wcet") opt.wcetQueries.push_back(need("--wcet"));
        else if (a == "--pad-tables") opt.padTables = true;
        else if (a == "--werror") opt.werror = true;
        else if (a == "--dataflow") {
            std::string m = need("--dataflow");
            if (m == "off") opt.dataflow = asmcore::DataflowMode::Off;
            else if (m == "warn") opt.dataflow = asmcore::DataflowMode::Warn;
            else if (m == "error") opt.dataflow = asmcore::DataflowMode::Error;
            else { std::fprintf(stderr, "asmc: --dataflow must be off, warn or error\n"); return 2; }
        }
        else if (a == "--no-warn") opt.suppress.push_back(need("--no-warn"));
        else if (a == "--print-profile") printProfile = true;
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "asmc: unknown option %s (try --help)\n", a.c_str()); return 2; }
        else inputs.push_back(a);
    }
    if (!profilePath.empty()) {
        std::string text;
        if (!readFile(profilePath, text)) { std::fprintf(stderr, "asmc: cannot read profile %s\n", profilePath.c_str()); return 2; }
        std::vector<std::string> errs;
        if (!opt.profile.load(text, errs)) { for (auto& e : errs) std::fprintf(stderr, "%s: %s\n", profilePath.c_str(), e.c_str()); return 2; }
    }
    if (printProfile) { std::fputs(opt.profile.dump().c_str(), stdout); return 0; }
    if (inputs.empty()) { usage(); return 2; }

    std::vector<asmcore::Source> sources;
    for (auto& in : inputs) {
        asmcore::Source s;
        s.name = in;
        if (!readFile(in, s.text)) { std::fprintf(stderr, "asmc: cannot read %s\n", in.c_str()); return 2; }
        sources.push_back(s);
    }
    if (prefix.empty()) prefix = fs::path(inputs[0]).replace_extension("").string();

    auto resolver = [&](const std::string& from, const std::string& name, asmcore::Source& out) {
        std::vector<fs::path> tries;
        tries.push_back(fs::path(from).parent_path() / name);
        for (auto& d : incDirs) tries.push_back(fs::path(d) / name);
        for (auto& p : tries) {
            if (readFile(p, out.text)) { out.name = p.string(); return true; }
        }
        return false;
    };

    asmcore::Result r = asmcore::assemble(sources, resolver, opt);
    std::fputs(r.diagnosticsText().c_str(), stderr);
    if (!r.ok) {
        std::fprintf(stderr, "asmc: %d error(s), %d warning(s); nothing written\n", r.errorCount(), r.warningCount());
        return 1;
    }
    bool io = true;
    for (asmcore::Domain d : {asmcore::Domain::Main, asmcore::Domain::Peri}) {
        const asmcore::DomainImage& im = r.image(d);
        if (!im.present) continue;
        std::string base = prefix + (d == asmcore::Domain::Main ? ".main" : ".peri");
        io &= writeFile(base + ".bin", im.bytes.data(), im.bytes.size());
        io &= writeText(base + ".layout.json", r.layoutJson(d));                  // the sideband ImageLayout: never inside the .bin
        if (hex) io &= writeText(base + ".hex", r.intelHex(d));
        std::printf("%s: code %u bytes, image %zu bytes -> %s.bin\n", asmcore::domainName(d), im.codeSize, im.bytes.size(), base.c_str());
    }
    if (artifacts) {
        io &= writeText(prefix + ".sram.hex", r.sramWordsHex());
        io &= writeText(prefix + ".boot.json", r.bootPlanJson());
        io &= writeText(prefix + ".memory.json", r.memoryProofJson());
        io &= writeText(prefix + ".sections.json", r.sectionMapJson());
        io &= writeText(prefix + ".descriptors.json", r.descriptorMapJson());
        io &= writeText(prefix + ".services.json", r.servicesJson());
        io &= writeText(prefix + ".events.json", r.eventVectorsJson());
        io &= writeText(prefix + ".validation.json", r.validationReportJson());
        io &= writeText(prefix + ".manifest.json", r.buildManifestJson(sources));
    }
    if (list) io &= writeText(prefix + ".lst", r.listing());
    if (map) io &= writeText(prefix + ".map", r.map());
    if (dbg) io &= writeText(prefix + ".dbg.json", r.debugJson());
    for (const asmcore::WcetResult& w : r.wcet)
        std::printf("WCET %s: %s\n", w.name.c_str(), w.bounded ? (std::to_string(w.cycles) + " cycles").c_str() : "unbounded");
    if (r.warningCount()) std::printf("%d warning(s)\n", r.warningCount());
    if (!io) { std::fprintf(stderr, "asmc: could not write an output file\n"); return 2; }
    return 0;
}
