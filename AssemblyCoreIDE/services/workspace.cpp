#include "workspace.h"

#include <algorithm>

namespace acide {

Workspace::Workspace(std::shared_ptr<IFileSystem> fs) : fs_(std::move(fs)) {}

bool Workspace::openProject(const std::string& file, std::vector<std::string>& errors) {
    std::string text;
    if (!fs_->read(file, text)) { errors.push_back("cannot read " + file); return false; }
    Project p;
    if (!Project::parse(text, p, errors)) return false;
    p.file = file;
    if (p.name.empty()) {
        std::string b = baseName(file);
        p.name = b.substr(0, b.find_last_of('.'));
        if (p.outputPrefix.empty()) p.outputPrefix = p.name;
    }
    project_ = p;
    docs_.clear();
    revision_++;
    projectChanged.fire();
    return true;
}

void Workspace::newProject(const std::string& file, const std::string& name) {
    project_ = Project();
    project_.file = file;
    project_.name = name;
    project_.outputPrefix = name;
    docs_.clear();
    revision_++;
    projectChanged.fire();
}

bool Workspace::saveProject(std::string& error) {
    if (project_.file.empty()) { error = "the project has no file name"; return false; }
    if (!fs_->write(project_.file, project_.serialize())) { error = "cannot write " + project_.file; return false; }
    return true;
}

void Workspace::projectEdited() {
    revision_++;
    projectChanged.fire();
}

Document* Workspace::open(const std::string& path) {
    auto it = docs_.find(path);
    if (it != docs_.end()) return &it->second;
    Document d;
    d.path = path;
    if (!fs_->read(path, d.text)) return nullptr;
    d.diskStamp = fs_->modified(path);
    auto res = docs_.emplace(path, std::move(d));
    documentOpened.fire(path);
    return &res.first->second;
}

Document* Workspace::find(const std::string& path) {
    auto it = docs_.find(path);
    return it == docs_.end() ? nullptr : &it->second;
}

void Workspace::setText(const std::string& path, const std::string& text) {
    Document* d = open(path);
    if (!d) {
        Document n;                                         // a new file not on disk yet
        n.path = path;
        n.dirty = true;
        d = &docs_.emplace(path, std::move(n)).first->second;
    }
    if (d->text == text) return;
    d->text = text;
    d->version++;
    d->dirty = true;
    revision_++;
    documentChanged.fire(path);
}

bool Workspace::save(const std::string& path, std::string& error) {
    Document* d = find(path);
    if (!d) { error = "not open: " + path; return false; }
    if (!fs_->write(path, d->text)) { error = "cannot write " + path; return false; }
    d->dirty = false;
    d->diskStamp = fs_->modified(path);
    documentChanged.fire(path);
    return true;
}

bool Workspace::saveAll(std::string& error) {
    bool ok = true;
    for (auto& kv : docs_) if (kv.second.dirty && !save(kv.first, error)) ok = false;
    return ok;
}

void Workspace::close(const std::string& path) {
    if (docs_.erase(path)) { revision_++; documentClosed.fire(path); }
}

std::vector<std::string> Workspace::openPaths() const {
    std::vector<std::string> v;
    for (auto& kv : docs_) v.push_back(kv.first);
    return v;
}

std::vector<std::string> Workspace::externallyChanged() {
    std::vector<std::string> v;
    for (auto& kv : docs_)
        if (!kv.second.dirty && kv.second.diskStamp >= 0 && fs_->modified(kv.first) != kv.second.diskStamp) v.push_back(kv.first);
    return v;
}

bool Workspace::reload(const std::string& path) {
    Document* d = find(path);
    if (!d) return false;
    std::string t;
    if (!fs_->read(path, t)) return false;
    d->text = t;
    d->version++;
    d->dirty = false;
    d->diskStamp = fs_->modified(path);
    revision_++;
    documentChanged.fire(path);
    return true;
}

void Workspace::keepBuffer(const std::string& path) {
    Document* d = find(path);
    if (!d) return;
    d->diskStamp = fs_->modified(path);
    d->dirty = true;                                         // it differs from what is on disk now
    documentChanged.fire(path);
}

bool Workspace::anyDirty() const {
    for (auto& kv : docs_) if (kv.second.dirty) return true;
    return false;
}

BuildInput Workspace::snapshot() const {
    BuildInput in;
    in.fs = fs_;
    in.options = project_.options();
    in.revision = revision_;
    for (auto& s : project_.includeDirs) in.includeDirs.push_back(project_.absolute(s));
    if (!project_.profile.empty()) in.profilePath = project_.absolute(project_.profile);
    in.prefix = project_.absolute(project_.outputDir) + "/" + (project_.outputPrefix.empty() ? project_.name : project_.outputPrefix);
    for (auto& kv : docs_) in.overlay[kv.first] = kv.second.text;
    for (const std::string& rel : project_.sources) {
        std::string path = project_.absolute(rel);
        asmcore::Source s;
        s.name = path;
        auto it = docs_.find(path);
        if (it != docs_.end()) s.text = it->second.text;
        else fs_->read(path, s.text);                        // a missing file stays empty; the build reports it
        in.sources.push_back(s);
    }
    return in;
}

}  // namespace acide
