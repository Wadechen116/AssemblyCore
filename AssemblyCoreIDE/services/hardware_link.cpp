#include "hardware_link.h"

#include <algorithm>

namespace acide {

namespace {
std::string relativeToProject(const Project& p, const std::string& abs) {
    const std::string base = p.baseDir();
    if (!base.empty() && abs.compare(0, base.size(), base) == 0) {
        std::string r = abs.substr(base.size());
        while (!r.empty() && (r[0] == '/' || r[0] == '\\')) r.erase(0, 1);
        return r;
    }
    return abs;
}
std::string fileName(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}
std::string stem(const std::string& path) {
    std::string f = fileName(path);
    size_t d = f.rfind('.');
    return d == std::string::npos ? f : f.substr(0, d);
}
}  // namespace

HardwareFiles hardwareFiles(const std::string& achwPath, const asmcore::Hardware& hw) {
    HardwareFiles f;
    f.achw = achwPath;
    f.include = asmcore::hardwareIncludePath(achwPath, hw);
    f.env = asmcore::hardwareEnvPath(achwPath, hw);
    return f;
}

bool loadHardware(Workspace& ws, const std::string& achwPath, asmcore::Hardware& hw, std::vector<std::string>& errors) {
    std::string text;
    if (Document* d = ws.find(achwPath)) text = d->text;
    else if (!ws.fileSystem()->read(achwPath, text)) { errors.push_back("cannot read " + achwPath); return false; }
    return asmcore::parseHardware(text, hw, errors);
}

void applyHardware(Workspace& ws, const std::string& achwPath, const asmcore::Hardware& hw) {
    const HardwareFiles f = hardwareFiles(achwPath, hw);
    const std::string src = fileName(achwPath);
    ws.setText(f.achw, asmcore::serializeHardware(hw));
    ws.setText(f.include, asmcore::generateInc(hw, src));
    ws.setText(f.env, asmcore::generateEnv(hw, src));
}

bool attachHardware(Workspace& ws, const std::string& achwPath, const asmcore::Hardware& hw, std::string& error) {
    if (!ws.hasProject()) { error = "open or create a project first"; return false; }
    Project& p = ws.project();
    p.hardware = relativeToProject(p, achwPath);
    if (hw.includeFile.empty()) {
        const std::string inc = relativeToProject(p, asmcore::hardwareIncludePath(achwPath, hw));
        if (std::find(p.sources.begin(), p.sources.end(), inc) == p.sources.end()) p.sources.insert(p.sources.begin(), inc);
    }
    if (hw.envFile.empty() && p.environment.empty()) p.environment = relativeToProject(p, asmcore::hardwareEnvPath(achwPath, hw));
    ws.projectEdited();
    return ws.saveProject(error);
}

bool createHardware(Workspace& ws, const std::string& achwPath, std::string& error) {
    asmcore::Hardware hw;
    hw.name = stem(achwPath);
    hw.description = "";
    applyHardware(ws, achwPath, hw);
    const HardwareFiles f = hardwareFiles(achwPath, hw);
    for (const std::string& p : {f.achw, f.include, f.env})
        if (!ws.save(p, error)) return false;
    return attachHardware(ws, achwPath, hw, error);
}

bool adoptHardware(Workspace& ws, const std::string& achwPath, const std::string& incPath, const std::string& envPath, std::vector<std::string>& messages, std::string& error) {
    std::string inc, env;
    if (Document* d = ws.find(incPath)) inc = d->text; else if (!ws.fileSystem()->read(incPath, inc)) { error = "cannot read " + incPath; return false; }
    if (!envPath.empty()) {
        if (Document* d = ws.find(envPath)) env = d->text; else if (!ws.fileSystem()->read(envPath, env)) { error = "cannot read " + envPath; return false; }
    }
    asmcore::Hardware hw;
    std::vector<std::string> errs;
    bool ok = asmcore::importHardware(inc, env, hw, errs);
    for (auto& e : errs) messages.push_back(e);
    hw.name = stem(achwPath);
    // generating over the files that were read keeps every source that includes them working
    auto rel = [&](const std::string& target) {
        std::string a = achwPath.substr(0, achwPath.find_last_of("/\\") == std::string::npos ? 0 : achwPath.find_last_of("/\\") + 1);
        if (!a.empty() && target.compare(0, a.size(), a) == 0) return target.substr(a.size());
        return target;
    };
    hw.includeFile = rel(incPath);
    if (!envPath.empty()) hw.envFile = rel(envPath);
    applyHardware(ws, achwPath, hw);
    if (!ws.save(achwPath, error)) return false;
    // the originals are NOT rewritten here: the designer writes them on the first save, after the person has looked at the result
    (void)ok;
    return attachHardware(ws, achwPath, hw, error);
}

}  // namespace acide
