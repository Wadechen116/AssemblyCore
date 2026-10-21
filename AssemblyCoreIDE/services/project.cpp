#include "project.h"

#include <algorithm>
#include <sstream>

#include "fs.h"

namespace acide {

std::string Project::baseDir() const { return dirName(file); }
std::string Project::absolute(const std::string& rel) const { return joinPath(baseDir(), rel); }

asmcore::Options Project::options() const {
    asmcore::Options o;
    o.callModel = callModel;
    o.dataflow = dataflow;
    o.suppress = suppress;
    o.werror = werror;
    o.wcetQueries = wcet;
    return o;
}

namespace {
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}
}

bool Project::parse(const std::string& text, Project& p, std::vector<std::string>& errors) {
    p = Project();
    std::istringstream in(text);
    std::string raw;
    int no = 0;
    bool ok = true;
    auto bad = [&](const std::string& m) { errors.push_back("line " + std::to_string(no) + ": " + m); ok = false; };
    while (std::getline(in, raw)) {
        no++;
        size_t c = raw.find('#');
        std::string line = trim(c == std::string::npos ? raw : raw.substr(0, c));
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { bad("expected 'key = value'"); continue; }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "name") p.name = v;
        else if (k == "source") p.sources.push_back(v);
        else if (k == "include_dir") p.includeDirs.push_back(v);
        else if (k == "profile") p.profile = v;
        else if (k == "environment") p.environment = v;
        else if (k == "hardware") p.hardware = v;
        else if (k == "scenario") p.scenarios.push_back(v);
        else if (k == "call_model") {
            if (v == "inline") p.callModel = asmcore::CallModel::Inline;
            else if (v == "softret") p.callModel = asmcore::CallModel::SoftRet;
            else bad("call_model must be inline or softret");
        } else if (k == "dataflow") {
            if (v == "off") p.dataflow = asmcore::DataflowMode::Off;
            else if (v == "warn") p.dataflow = asmcore::DataflowMode::Warn;
            else if (v == "error") p.dataflow = asmcore::DataflowMode::Error;
            else bad("dataflow must be off, warn or error");
        } else if (k == "suppress") p.suppress.push_back(v);
        else if (k == "wcet") p.wcet.push_back(v);
        else if (k == "werror") p.werror = v == "1" || v == "true" || v == "yes";
        else if (k == "output_dir") p.outputDir = v;
        else if (k == "output_prefix") p.outputPrefix = v;
        else p.extra.push_back({k, v});
    }
    if (p.outputPrefix.empty()) p.outputPrefix = p.name;
    return ok;
}

std::string Project::serialize() const {
    std::ostringstream o;
    o << "# AssemblyCoreIDE project\n";
    o << "name = " << name << "\n";
    for (auto& s : sources) o << "source = " << s << "\n";
    for (auto& s : includeDirs) o << "include_dir = " << s << "\n";
    if (!profile.empty()) o << "profile = " << profile << "\n";
    if (!environment.empty()) o << "environment = " << environment << "\n";
    if (!hardware.empty()) o << "hardware = " << hardware << "\n";
    for (auto& s : scenarios) o << "scenario = " << s << "\n";
    o << "call_model = " << (callModel == asmcore::CallModel::SoftRet ? "softret" : "inline") << "\n";
    o << "dataflow = " << (dataflow == asmcore::DataflowMode::Off ? "off" : dataflow == asmcore::DataflowMode::Error ? "error" : "warn") << "\n";
    for (auto& s : suppress) o << "suppress = " << s << "\n";
    for (auto& s : wcet) o << "wcet = " << s << "\n";
    if (werror) o << "werror = 1\n";
    o << "output_dir = " << outputDir << "\n";
    if (!outputPrefix.empty() && outputPrefix != name) o << "output_prefix = " << outputPrefix << "\n";
    for (auto& kv : extra) o << kv.first << " = " << kv.second << "\n";
    return o.str();
}

}  // namespace acide
