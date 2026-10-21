// asmhw -- the Hardware Designer on the command line: .achw -> assembler definitions + simulator environment, and back.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "asmcore/hardware.h"

namespace fs = std::filesystem;

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
        "  asmhw gen    board.achw [-o PREFIX]          write PREFIX.inc (assembler definitions) and PREFIX.env.cfg (simulator environment)\n"
        "                                               default PREFIX: board.gen   (existing files are overwritten)\n"
        "  asmhw check  board.achw                      validate; also runs the real assembler and simulator on the generated files\n"
        "  asmhw import --inc defs.inc [--env env.cfg] -o board.achw    build a description from existing files (nothing is lost)\n"
        "A hardware description is the single source of truth for CSRs (address, bit meanings), I2C devices, services and constants:\n"
        "see docs/HARDWARE.md. Exit status: 0 ok, 1 the description has errors, 2 usage / I/O error.");
}

int report(const std::vector<asmcore::HwProblem>& problems, const std::string& file) {
    int errors = 0;
    for (const asmcore::HwProblem& p : problems) {
        std::fprintf(stderr, "%s: %s%s%s: %s\n", file.c_str(), p.error ? "error" : "warning", p.where.empty() ? "" : " ", p.where.c_str(), p.message.c_str());
        errors += p.error;
    }
    return errors;
}

bool load(const std::string& file, asmcore::Hardware& hw) {
    std::string text;
    if (!readFile(file, text)) { std::fprintf(stderr, "asmhw: cannot read %s\n", file.c_str()); return false; }
    std::vector<std::string> errs;
    if (!asmcore::parseHardware(text, hw, errs)) {
        for (auto& e : errs) std::fprintf(stderr, "%s: %s\n", file.c_str(), e.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help")) { usage(); return argc < 2 ? 2 : 0; }
    const std::string cmd = argv[1];
    std::string input, out, incPath, envPath;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "asmhw: %s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "-o") out = need();
        else if (a == "--inc") incPath = need();
        else if (a == "--env") envPath = need();
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "asmhw: unknown option %s\n", a.c_str()); return 2; }
        else input = a;
    }
    if (cmd == "gen" || cmd == "check") {
        if (input.empty()) { usage(); return 2; }
        asmcore::Hardware hw;
        if (!load(input, hw)) return 1;
        std::vector<asmcore::HwProblem> problems = asmcore::validateHardware(hw);
        int errors = report(problems, input);
        if (!errors) errors = report(asmcore::checkHardwareWithTools(hw), input);        // only when the model is sane: the tools then add what it cannot know
        if (errors) { std::fprintf(stderr, "asmhw: %d error(s); nothing written\n", errors); return 1; }
        if (cmd == "check") { std::printf("%s: ok\n", input.c_str()); return 0; }
        std::string prefix = out.empty() ? fs::path(input).replace_extension("").string() + ".gen" : out;
        const std::string src = fs::path(input).filename().string();
        bool ok = writeFile(prefix + ".inc", asmcore::generateInc(hw, src)) & writeFile(prefix + ".env.cfg", asmcore::generateEnv(hw, src));
        if (!ok) { std::fprintf(stderr, "asmhw: cannot write %s.inc / %s.env.cfg\n", prefix.c_str(), prefix.c_str()); return 2; }
        std::printf("wrote %s.inc and %s.env.cfg\n", prefix.c_str(), prefix.c_str());
        return 0;
    }
    if (cmd == "import") {
        if (incPath.empty() || out.empty()) { usage(); return 2; }
        std::string inc, env;
        if (!readFile(incPath, inc)) { std::fprintf(stderr, "asmhw: cannot read %s\n", incPath.c_str()); return 2; }
        if (!envPath.empty() && !readFile(envPath, env)) { std::fprintf(stderr, "asmhw: cannot read %s\n", envPath.c_str()); return 2; }
        asmcore::Hardware hw;
        std::vector<std::string> errs;
        bool ok = asmcore::importHardware(inc, env, hw, errs);
        for (auto& e : errs) std::fprintf(stderr, "asmhw: %s\n", e.c_str());
        if (hw.name.empty()) hw.name = fs::path(out).stem().string();
        if (!writeFile(out, asmcore::serializeHardware(hw))) { std::fprintf(stderr, "asmhw: cannot write %s\n", out.c_str()); return 2; }
        std::printf("wrote %s: %zu CSR(s), %zu device(s), %zu service(s), %zu constant(s)%s\n", out.c_str(), hw.csrs.size(), hw.devices.size(), hw.services.size(), hw.consts.size(), ok ? "" : " (with warnings above)");
        return ok ? 0 : 1;
    }
    usage();
    return 2;
}
