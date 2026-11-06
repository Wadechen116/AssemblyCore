// asmdis -- disassemble an image written by asmc (PREFIX.main.bin / PREFIX.peri.bin) back to assembler source.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "asmcore/asmcore.h"
#include "asmcore/disasm.h"
#include "asmcore/eeprom.h"
#include "asmcore/sram.h"

using namespace asmcore;

namespace {
bool readBin(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
bool readText(const std::string& p, std::string& out) {
    std::vector<uint8_t> b;
    if (!readBin(p, b)) return false;
    out.assign(b.begin(), b.end());
    return true;
}
void usage() {
    std::puts(
        "usage: asmdis --main|--peri [options] image.bin\n"
        "       asmdis --eeprom PREFIX.eeprom.bin [--check] [--strict] [-o FILE]     both domains of a burned EEPROM image, with its names\n"
        "  --main | --peri      which domain the image belongs to (required)\n"
        "  --layout FILE        the ImageLayout of the image (PREFIX.main.layout.json written by asmc). Default: image.layout.json next to\n"
        "                       image.bin. Since spec v31 the layout is NOT inside the image, so it is required.\n"
        "  --sram FILE          instead of image.bin: read the logical image back from the SRAM word file (PREFIX.sram.hex) through the\n"
        "                       profile's image base word (an independent check of the word packing); needs --length\n"
        "  --eeprom FILE        instead of image.bin: the EEPROM image asmc --eeprom wrote (PREFIX.eeprom.bin): the domain's code, descriptor\n"
        "                       tables and const pool / DeviceMap are read from the regions of its map file (what the decoder fetches)\n"
        "  --eeprom-map FILE    the map of the EEPROM image (default: PREFIX.eeprom.json next to it); holds the region addresses and the ImageLayout\n"
        "  --eeprom-sym FILE    the names (default: PREFIX.eeprom.sym next to it, if there is one): labels, CSRs, services, devices\n"
        "  --no-names           do not use the symbol file (L_xxxx labels and numbers)\n"
        "  --strict             with --eeprom and --check: a warning (a data-flow rule, an unreachable instruction) fails the check too\n"
        "  With --eeprom and no --main/--peri both domains are written as one source, and --check runs it through the assembler (every rule of\n"
        "  the specification and the profile) and compares the image it makes with the burned one, region by region.\n"
        "  --length N           logical image size in bytes (with --sram; default: the end of the last section of the layout file)\n"
        "  --events FILE        PREFIX.events.json of asmc --artifacts: the event vectors of this domain become labels and '.vector' lines\n"
        "  --profile FILE       the profile the image was assembled with (byte order, DeviceMap packing, ALU codes, SRAM image bases)\n"
        "  --code-size N        exact size of the code in bytes (default: inferred, see docs/DISASM.md)\n"
        "  --labels FILE        label names, one per line: <address> <name>   (e.g. 0x0004 handle_gyro)\n"
        "  --label-prefix P     prefix of generated labels (default L_)\n"
        "  --long-jumps         write unconditional branches as BR.COND ALWAYS / BR.ALWAYS instead of JMP\n"
        "  --annotate           add address and raw bytes as comments\n"
        "  --check              assemble the result again and compare it with the input image\n"
        "  -o FILE              write the source to FILE (default: standard output)\n"
        "Exit status: 0 ok, 1 the image could not be disassembled cleanly or --check found a difference, 2 usage / I/O error.");
}
}  // namespace

int main(int argc, char** argv) {
    bool haveDomain = false, annotate = false, check = false, longJumps = false;
    Domain dom = Domain::Main;
    Profile pf;
    std::string eventsPath, profilePath, labelsPath, outPath, input, prefix = "L_", layoutPath, sramPath, eepromPath, eepromMapPath, eepromSymPath;
    bool noNames = false, strict = false;
    long long lengthArg = 0;
    long long codeSize = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "asmdis: %s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--main") { dom = Domain::Main; haveDomain = true; }
        else if (a == "--peri") { dom = Domain::Peri; haveDomain = true; }
        else if (a == "--profile") profilePath = need();
        else if (a == "--layout") layoutPath = need();
        else if (a == "--events") eventsPath = need();
        else if (a == "--sram") sramPath = need();
        else if (a == "--eeprom") eepromPath = need();
        else if (a == "--eeprom-map") eepromMapPath = need();
        else if (a == "--eeprom-sym") eepromSymPath = need();
        else if (a == "--no-names") noNames = true;
        else if (a == "--strict") strict = true;
        else if (a == "--length") lengthArg = std::atoll(need().c_str());
        else if (a == "--code-size") codeSize = std::atoll(need().c_str());
        else if (a == "--labels") labelsPath = need();
        else if (a == "--label-prefix") prefix = need();
        else if (a == "--long-jumps") longJumps = true;
        else if (a == "--annotate") annotate = true;
        else if (a == "--check") check = true;
        else if (a == "-o") outPath = need();
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "asmdis: unknown option %s\n", a.c_str()); return 2; }
        else input = a;
    }
    if ((!haveDomain && eepromPath.empty()) || (input.empty() && sramPath.empty() && eepromPath.empty())) { usage(); return 2; }
    if (!profilePath.empty()) {
        std::string t;
        std::vector<std::string> errs;
        if (!readText(profilePath, t) || !pf.load(t, errs)) { std::fprintf(stderr, "asmdis: cannot use profile %s\n", profilePath.c_str()); return 2; }
    }
    if (!eepromPath.empty() && !haveDomain) {                               // the whole burned image: both domains, its names, checked against the specification
        if (!profilePath.empty()) {
            std::string t;
            std::vector<std::string> errs;
            if (!readText(profilePath, t) || !pf.load(t, errs)) { std::fprintf(stderr, "asmdis: cannot use profile %s\n", profilePath.c_str()); return 2; }
        }
        const size_t dot = eepromPath.rfind(".bin");
        const std::string base = dot != std::string::npos && dot + 4 == eepromPath.size() ? eepromPath.substr(0, dot) : eepromPath;
        if (eepromMapPath.empty()) eepromMapPath = base + ".json";
        if (eepromSymPath.empty()) eepromSymPath = base + ".sym";
        std::vector<uint8_t> eep;
        std::string t, err;
        EepromMap emap;
        if (!readBin(eepromPath, eep)) { std::fprintf(stderr, "asmdis: cannot read %s\n", eepromPath.c_str()); return 2; }
        if (!readText(eepromMapPath, t) || !loadEepromMapJson(t, emap, err)) { std::fprintf(stderr, "asmdis: the EEPROM map %s: %s\n", eepromMapPath.c_str(), err.empty() ? "cannot be read" : err.c_str()); return 2; }
        EepromSymbols syms;
        bool haveSyms = false;
        if (!noNames && readText(eepromSymPath, t)) {
            if (loadEepromSymbolsJson(t, syms, err)) haveSyms = true;
            else std::fprintf(stderr, "asmdis: %s: %s (the names are left out)\n", eepromSymPath.c_str(), err.c_str());
        }
        EepromDisasmCheck c;
        if (!checkEepromDisassembly(eep, emap, pf, haveSyms ? &syms : nullptr, annotate, c, err)) { std::fprintf(stderr, "asmdis: %s\n", err.c_str()); return 2; }
        const std::string text = "; disassembled from " + eepromPath + (haveSyms ? ", names from " + eepromSymPath : std::string(" (no symbol file: L_xxxx labels)")) + "\n" + c.source;
        if (!outPath.empty()) { std::ofstream o(outPath, std::ios::binary); o << text; if (!o) { std::fprintf(stderr, "asmdis: cannot write %s\n", outPath.c_str()); return 2; } }
        else std::fputs(text.c_str(), stdout);
        const int shift = (int)std::count(text.begin(), text.end() - (long)c.source.size(), '\n');             // (the "disassembled from" line in front)
        for (const DisasmIssue& i : c.decodeIssues) {
            const int line = c.lineOfIssue(i);
            std::fprintf(stderr, "%s %s %s pc %u%s: %s\n", i.fatal ? "decode error" : "decode remark", i.code.c_str(), domainName(i.domain), i.pc,
                         line ? (" (line " + std::to_string(line + shift) + ")").c_str() : "", i.msg.c_str());
        }
        if (!check) return c.fatalDecode() ? 1 : 0;
        for (const Diagnostic& d : c.specDiags)
            std::fprintf(stderr, "spec %s %s line %d: %s\n", d.sev == Diagnostic::Error ? "error" : d.sev == Diagnostic::Warning ? "warning" : "note", d.code.c_str(), d.loc.line > 0 ? d.loc.line + shift : 0, d.msg.c_str());
        if (c.assembles && !c.identical)
            std::fprintf(stderr, "check: DIFFERENT - it assembles to another image: %zu byte(s) differ, the first in %s at 0x%04X\n", c.differentBytes, eepromRegionName(c.differentRegion), c.firstDifference);
        std::fprintf(stderr, "check: %s - %d decode error(s), %d spec error(s), %d warning(s), %s\n", c.ok(strict) ? "OK" : "FAILED", c.fatalDecode(), c.errors(), c.warnings(),
                     !c.assembles ? "it does not assemble" : c.identical ? "it assembles to the identical image" : "it assembles to a DIFFERENT image");
        return c.ok(strict) ? 0 : 1;
    }
    DomainImage im;
    im.domain = dom;
    im.present = true;
    if (!eepromPath.empty()) {                                              // the burned EEPROM: the regions of its map give this domain's logical image
        if (eepromMapPath.empty()) {
            const size_t dot = eepromPath.rfind(".bin");
            eepromMapPath = (dot != std::string::npos && dot + 4 == eepromPath.size() ? eepromPath.substr(0, dot) : eepromPath) + ".json";
        }
        std::vector<uint8_t> eep;
        std::string t, err;
        EepromMap emap;
        if (!readBin(eepromPath, eep)) { std::fprintf(stderr, "asmdis: cannot read %s\n", eepromPath.c_str()); return 2; }
        if (!readText(eepromMapPath, t) || !loadEepromMapJson(t, emap, err)) { std::fprintf(stderr, "asmdis: the EEPROM map %s: %s\n", eepromMapPath.c_str(), err.empty() ? "cannot be read" : err.c_str()); return 2; }
        DomainImage mi, pi;
        if (!domainImagesFromEeprom(eep, emap, mi, pi, err)) { std::fprintf(stderr, "asmdis: %s\n", err.c_str()); return 2; }
        im = dom == Domain::Main ? mi : pi;
        if (!im.present) { std::fprintf(stderr, "asmdis: the EEPROM image holds no %s program\n", domainName(dom)); return 2; }
        layoutPath = "(from the EEPROM map)";
    }
    // ImageLayout is sideband: it comes from the layout file the assembler wrote next to the image
    if (layoutPath.empty() && !input.empty()) {
        size_t dot = input.rfind(".bin");
        layoutPath = (dot != std::string::npos && dot + 4 == input.size() ? input.substr(0, dot) : input) + ".layout.json";
    }
    if (eepromPath.empty()) {
        std::string t, err;
        if (layoutPath.empty() || !readText(layoutPath, t)) {
            std::fprintf(stderr, "asmdis: the ImageLayout file %s cannot be read.\n  Since spec v31 the layout is not part of the image: asmc writes it next to the image as PREFIX.%s.layout.json.\n"
                                 "  Keep the two together, or give it with --layout FILE.\n", layoutPath.empty() ? "(none given)" : layoutPath.c_str(), dom == Domain::Main ? "main" : "peri");
            return 2;
        }
        if (!loadLayoutJson(t, im, err)) { std::fprintf(stderr, "asmdis: %s: %s\n", layoutPath.c_str(), err.c_str()); return 2; }
    }
    if (!sramPath.empty()) {
        // read the words (lines "ADDR DATA", hex), then the image through the profile's image base word
        std::string t;
        if (!readText(sramPath, t)) { std::fprintf(stderr, "asmdis: cannot read %s\n", sramPath.c_str()); return 2; }
        SramContents sram;
        std::istringstream in(t);
        std::string line;
        while (std::getline(in, line)) {
            if (line.compare(0, 2, "//") == 0) continue;
            std::istringstream ls(line);
            std::string a, d;
            if (!(ls >> a >> d)) continue;
            sram[(uint32_t)std::strtoul(a.c_str(), nullptr, 16)] = (uint32_t)std::strtoul(d.c_str(), nullptr, 16);
        }
        uint32_t len = (uint32_t)lengthArg;
        if (!len) for (const SectionInfo& s : im.sections) len = std::max(len, s.offset + s.size);
        if (!len) { std::fprintf(stderr, "asmdis: --sram needs --length (the layout file has no section list)\n"); return 2; }
        if (!imageFromSram(sram, pf, dom, len, im.bytes)) { std::fprintf(stderr, "asmdis: %s does not hold all %u bytes of the %s image at the profile's image base word %d (wrong --profile?)\n", sramPath.c_str(), len, domainName(dom), dom == Domain::Main ? pf.mainImageBaseWord : pf.periImageBaseWord); return 2; }
    } else if (eepromPath.empty() && !readBin(input, im.bytes)) { std::fprintf(stderr, "asmdis: cannot read %s\n", input.c_str()); return 2; }

    std::map<uint32_t, std::string> names;
    if (!labelsPath.empty()) {
        std::string t;
        if (!readText(labelsPath, t)) { std::fprintf(stderr, "asmdis: cannot read %s\n", labelsPath.c_str()); return 2; }
        std::istringstream in(t);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string addr, name;
            if (!(ls >> addr >> name) || addr[0] == ';') continue;
            names[(uint32_t)std::strtoul(addr.c_str(), nullptr, 0)] = name;
        }
    }
    DisasmOptions o;
    o.codeSize = (uint32_t)codeSize;
    o.labelPrefix = prefix;
    o.useJmp = !longJumps;
    if (!names.empty()) o.names = &names;
    if (!eventsPath.empty()) {
        std::string t;
        if (!readText(eventsPath, t)) { std::fprintf(stderr, "asmdis: cannot read %s\n", eventsPath.c_str()); return 2; }
        // minimal scan of the "vectors" array written by asmc: {"domain":"Main","event_id":0,...,"logical_pc":7,...}
        const std::string want = std::string("\"domain\":\"") + domainName(dom) + "\"";
        size_t end = t.find("\"timing\"");
        for (size_t at = t.find("{\"domain\""); at != std::string::npos && at < end; at = t.find("{\"domain\"", at + 1)) {
            size_t close = t.find('}', at);
            std::string obj = t.substr(at, close - at);
            if (obj.find(want) == std::string::npos) continue;
            size_t a = obj.find("\"event_id\":"), b = obj.find("\"logical_pc\":");
            if (a == std::string::npos || b == std::string::npos) continue;
            o.vectors.push_back({std::atoi(obj.c_str() + a + 11), (uint32_t)std::strtoul(obj.c_str() + b + 13, nullptr, 10)});
        }
    }
    DisasmResult r = disassemble(im, pf, o);
    std::fputs(r.issuesText().c_str(), stderr);
    std::string text = r.source(annotate);
    if (outPath.empty()) std::fputs(text.c_str(), stdout);
    else {
        std::ofstream out(outPath, std::ios::binary);
        out << text;
        if (!out) { std::fprintf(stderr, "asmdis: cannot write %s\n", outPath.c_str()); return 2; }
    }
    int status = r.ok ? 0 : 1;
    if (check) {
        Options opt;
        opt.profile = pf;
        std::vector<Source> v{Source{"disassembly", text}};
        Result b = assemble(v, nullptr, opt);
        if (!b.ok) { std::fprintf(stderr, "check: the disassembly does not assemble:\n%s", b.diagnosticsText().c_str()); return 1; }
        const std::vector<uint8_t>& again = b.image(dom).bytes;
        size_t n = std::min(again.size(), im.bytes.size()), at = (size_t)-1;
        for (size_t i = 0; i < n; i++) if (again[i] != im.bytes[i]) { at = i; break; }
        if (at == (size_t)-1 && again.size() != im.bytes.size()) at = n;
        if (at == (size_t)-1) std::fprintf(stderr, "check: OK - the disassembly assembles to the identical %zu-byte image\n", im.bytes.size());
        else { std::fprintf(stderr, "check: DIFFERENT - first difference at byte %zu (image %zu bytes, reassembled %zu bytes)\n", at, im.bytes.size(), again.size()); status = 1; }
    }
    return status;
}
