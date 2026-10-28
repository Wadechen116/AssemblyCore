#include "sim_params_store.h"

namespace acide {

void SimParamsStore::touch() {
    dirty_ = true;
    changed.fire();
}

bool SimParamsStore::set(const std::string& key, const std::string& text, std::string& error) {
    if (!p_.set(key, text, error)) return false;
    touch();
    return true;
}

void SimParamsStore::clear(const std::string& key) {
    p_.clear(key);
    touch();
}

void SimParamsStore::clearAll() {
    p_.clearAll();
    touch();
}

bool SimParamsStore::setStatus(const std::string& key, const std::string& status, std::string& error) {
    if (!p_.setStatus(key, status, error)) return false;
    touch();
    return true;
}

void SimParamsStore::setNote(const std::string& key, const std::string& note) {
    p_.setNote(key, note);
    touch();
}

bool SimParamsStore::load(const std::string& absPath, std::vector<std::string>& errors) {
    std::string text;
    if (const Document* d = ws_.find(absPath)) text = d->text;
    else if (!ws_.fileSystem()->read(absPath, text)) { errors.push_back("cannot read " + absPath); return false; }
    asmcore::SimParams n;
    const bool ok = n.load(text, errors);
    p_ = n;
    path_ = absPath;
    dirty_ = false;
    changed.fire();
    return ok;
}

bool SimParamsStore::save(std::string& error) {
    if (path_.empty()) { error = "these values belong to no file yet: use Save As"; return false; }
    ws_.setText(path_, p_.serialize());
    if (!ws_.save(path_, error)) return false;
    dirty_ = false;
    changed.fire();
    return true;
}

bool SimParamsStore::saveAs(const std::string& absPath, std::string& error) {
    path_ = absPath;
    if (!save(error)) return false;
    if (ws_.hasProject()) {
        Project& pr = ws_.project();
        const std::string base = pr.baseDir();
        const std::string prefix = base.empty() || base.back() == '/' ? base : base + "/";
        if (absPath.compare(0, prefix.size(), prefix) == 0 && pr.simParams != absPath.substr(prefix.size())) {
            pr.simParams = absPath.substr(prefix.size());
            if (!ws_.saveProject(error)) return false;
            ws_.projectEdited();
        }
    }
    return true;
}

void SimParamsStore::closeFile() {
    path_.clear();
    dirty_ = !p_.keys().empty();
    changed.fire();
}

void SimParamsStore::syncWithProject() {
    if (!ws_.hasProject()) { if (!path_.empty()) closeFile(); return; }
    const Project& pr = ws_.project();
    if (pr.simParams.empty()) { if (!path_.empty()) closeFile(); return; }
    const std::string abs = pr.absolute(pr.simParams);
    if (abs == path_) return;
    std::vector<std::string> errs;
    load(abs, errs);
}

}  // namespace acide
