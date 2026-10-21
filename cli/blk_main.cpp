// asmblk -- block programs on the command line: assembler text <-> .acblk, and a list of the blocks.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "asmcore/asmcore.h"
#include "asmcore/blocks.h"

namespace fs = std::filesystem;
using namespace asmcore::blocks;

namespace {

bool readFile(const std::string& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}
bool writeFile(const std::string& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary);
    if (!o) return false;
    o << text;
    return (bool)o;
}

void usage() {
    std::puts(
        "usage:\n"
        "  asmblk import prog.asm [-o prog.acblk]      assembler text -> block file (a line the blocks do not cover is kept as a Raw block)\n"
        "  asmblk gen    prog.acblk [-o prog.asm]      block file -> assembler text (default name: prog.generated.asm)\n"
        "  asmblk check  prog.acblk | prog.asm         check the structure of a block file; for a .asm also assemble the text before and after\n"
        "                                              the round trip and compare the images\n"
        "  asmblk blocks                               list the blocks, by drawer, with their fields\n"
        "Exit status: 0 ok, 1 problems found, 2 usage / I/O error.");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help")) { usage(); return argc < 2 ? 2 : 0; }
    const std::string cmd = argv[1];
    std::string input, out;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-o") { if (i + 1 >= argc) { std::fprintf(stderr, "asmblk: -o needs a file name\n"); return 2; } out = argv[++i]; }
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "asmblk: unknown option %s\n", a.c_str()); return 2; }
        else input = a;
    }
    if (cmd == "blocks") {
        std::map<std::string, std::vector<const BlockDef*>> by;
        for (const BlockDef& d : Registry::defaults().all()) by[d.category].push_back(&d);
        for (auto& kv : by) {
            std::printf("%s\n", kv.first.c_str());
            for (const BlockDef* d : kv.second) {
                std::printf("  %-14s %-26s %s", d->type.c_str(), d->title.c_str(), d->domains == kMain ? "[Main] " : d->domains == kPeri ? "[Peri] " : "");
                for (const FieldDef& f : d->fields) std::printf(" %s", f.label.c_str());
                if (!d->stacks.empty()) std::printf("  (holds blocks)");
                std::printf("\n");
            }
        }
        return 0;
    }
    if (input.empty()) { usage(); return 2; }
    std::string text;
    if (!readFile(input, text)) { std::fprintf(stderr, "asmblk: cannot read %s\n", input.c_str()); return 2; }
    if (cmd == "import") {
        ImportStats st;
        Document d = importAsm(text, fs::path(input).stem().string(), &st);
        if (out.empty()) out = fs::path(input).replace_extension(".acblk").string();
        if (!writeFile(out, serializeDocument(d))) { std::fprintf(stderr, "asmblk: cannot write %s\n", out.c_str()); return 2; }
        std::printf("wrote %s: %d line(s) -> %d block(s)", out.c_str(), st.lines, st.blocks);
        if (st.raw) std::printf(", %d kept as Raw text:\n", st.raw), [&] { for (auto& l : st.rawLines) std::printf("  %s\n", l.c_str()); }();
        else std::printf("\n");
        return 0;
    }
    if (cmd == "gen") {
        Document d;
        std::string err;
        if (!parseDocument(text, d, err)) { std::fprintf(stderr, "%s: %s\n", input.c_str(), err.c_str()); return 1; }
        int errors = 0;
        for (const BlockProblem& p : validate(d)) {
            std::fprintf(stderr, "%s: %s block %s: %s\n", input.c_str(), p.error ? "error" : "warning", p.blockId.c_str(), p.message.c_str());
            errors += p.error;
        }
        if (errors) { std::fprintf(stderr, "asmblk: %d structural error(s); nothing written\n", errors); return 1; }
        if (out.empty()) { fs::path p(input); out = (p.parent_path() / (p.stem().string() + ".generated.asm")).string(); }
        if (!writeFile(out, generate(d).text)) { std::fprintf(stderr, "asmblk: cannot write %s\n", out.c_str()); return 2; }
        std::printf("wrote %s\n", out.c_str());
        return 0;
    }
    if (cmd == "check") {
        const bool isAsm = fs::path(input).extension() != ".acblk";
        Document d;
        std::string err;
        std::string original;
        if (isAsm) { original = text; d = importAsm(text); }
        else if (!parseDocument(text, d, err)) { std::fprintf(stderr, "%s: %s\n", input.c_str(), err.c_str()); return 1; }
        int errors = 0;
        for (const BlockProblem& p : validate(d)) {
            std::fprintf(stderr, "%s: %s block %s: %s\n", input.c_str(), p.error ? "error" : "warning", p.blockId.c_str(), p.message.c_str());
            errors += p.error;
        }
        const std::string regenerated = generate(d).text;
        if (isAsm) {
            using namespace asmcore;
            auto resolver = [&](const std::string& from, const std::string& name, Source& o) {
                fs::path f = fs::path(from).parent_path() / name;
                if (!fs::exists(f)) f = fs::path(input).parent_path() / name;
                std::string t;
                if (!readFile(f.string(), t)) return false;
                o.name = f.string();
                o.text = t;
                return true;
            };
            Options opt;
            Result a = assemble({Source{input, original}}, resolver, opt), b = assemble({Source{input, regenerated}}, resolver, opt);
            const bool same = a.ok == b.ok && a.main.bytes == b.main.bytes && a.peri.bytes == b.peri.bytes;
            std::printf("%s: %s the text before and after the round trip assemble to %s\n", input.c_str(), same ? "ok -" : "DIFFERENT -", same ? "the same images" : "different images");
            if (!same) errors++;
        }
        if (!errors) std::printf("%s: %zu block(s), structure ok\n", input.c_str(), d.blockCount());
        return errors ? 1 : 0;
    }
    usage();
    return 2;
}
