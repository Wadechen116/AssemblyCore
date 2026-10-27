#include "block_link.h"

#include <algorithm>

namespace acide {

namespace {

std::string dirOf(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}

std::string baseOf(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string stemOf(const std::string& p) {
    const std::string b = baseOf(p);
    const size_t d = b.find_last_of('.');
    return d == std::string::npos || d == 0 ? b : b.substr(0, d);
}

// relative to the project's folder, or "" if it is outside
std::string relativeTo(const Project& pr, const std::string& abs) {
    const std::string base = pr.baseDir();
    const std::string prefix = base.empty() || base.back() == '/' ? base : base + "/";
    if (abs.compare(0, prefix.size(), prefix) != 0) return "";
    return abs.substr(prefix.size());
}

}  // namespace

std::string generatedPathFor(const std::string& acblkPath, const asmcore::blocks::Document& doc) {
    if (!doc.output.empty()) return joinPath(dirOf(acblkPath), doc.output);
    return dirOf(acblkPath) + stemOf(acblkPath) + ".generated.asm";
}

std::string generatedHeader(const std::string& acblkPath) { return "Generated from " + baseOf(acblkPath) + " - edit the blocks, not this file"; }

bool attachBlocks(Workspace& ws, const std::string& acblkPath, const std::string& generatedPath, std::string& error, bool* changed) {
    if (changed) *changed = false;
    if (!ws.hasProject()) return true;
    Project& pr = ws.project();
    const std::string blk = relativeTo(pr, acblkPath);
    if (blk.empty()) return true;
    const std::string gen = relativeTo(pr, generatedPath);
    bool edited = false;
    if (std::find(pr.blocks.begin(), pr.blocks.end(), blk) == pr.blocks.end()) { pr.blocks.push_back(blk); edited = true; }
    if (!gen.empty() && std::find(pr.sources.begin(), pr.sources.end(), gen) == pr.sources.end()) { pr.sources.push_back(gen); edited = true; }
    if (!edited) return true;
    if (changed) *changed = true;
    if (!ws.saveProject(error)) return false;
    ws.projectEdited();
    return true;
}

bool detachBlocks(Workspace& ws, const std::string& acblkPath, std::string& error) {
    if (!ws.hasProject()) return true;
    Project& pr = ws.project();
    const std::string blk = relativeTo(pr, acblkPath);
    auto it = std::find(pr.blocks.begin(), pr.blocks.end(), blk);
    if (blk.empty() || it == pr.blocks.end()) return true;
    pr.blocks.erase(it);
    if (!ws.saveProject(error)) return false;
    ws.projectEdited();
    return true;
}

std::map<std::string, std::string> generatedFiles(Workspace& ws) {
    std::map<std::string, std::string> m;
    if (!ws.hasProject()) return m;
    for (const std::string& rel : ws.project().blocks) {
        const std::string abs = ws.project().absolute(rel);
        std::string text;
        if (const Document* d = ws.find(abs)) text = d->text;
        else if (!ws.fileSystem()->read(abs, text)) continue;
        asmcore::blocks::Document doc;
        std::string err;
        if (!asmcore::blocks::parseDocument(text, doc, err)) continue;
        m[generatedPathFor(abs, doc)] = abs;
    }
    return m;
}

}  // namespace acide
