#include "fs.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fsys = std::filesystem;

namespace acide {

namespace {
fsys::path toPath(const std::string& p) { return fsys::u8path(p); }
}

bool RealFileSystem::read(const std::string& path, std::string& out) {
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}
bool RealFileSystem::write(const std::string& path, const std::string& text) {
    std::error_code ec;
    fsys::create_directories(toPath(path).parent_path(), ec);        // an output directory that does not exist yet
    std::ofstream o(toPath(path), std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << text;
    return (bool)o;
}
bool RealFileSystem::exists(const std::string& path) {
    std::error_code ec;
    return fsys::exists(toPath(path), ec);
}
std::vector<std::string> RealFileSystem::list(const std::string& dir) {
    std::vector<std::string> v;
    std::error_code ec;
    for (fsys::directory_iterator it(toPath(dir), ec), end; !ec && it != end; it.increment(ec)) v.push_back(it->path().filename().u8string());
    std::sort(v.begin(), v.end());
    return v;
}
long long RealFileSystem::modified(const std::string& path) {
    std::error_code ec;
    auto t = fsys::last_write_time(toPath(path), ec);
    if (ec) return -1;
    // the epoch of file_time_type differs between standard libraries (libstdc++ counts from 2174, so today is negative): microseconds plus
    // an offset keep the stamp positive, and -1 free for "absent"
    return (1LL << 60) + std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
}

bool MemoryFileSystem::read(const std::string& path, std::string& out) {
    std::lock_guard<std::mutex> l(m_);
    auto it = files_.find(path);
    if (it == files_.end()) return false;
    out = it->second.first;
    return true;
}
bool MemoryFileSystem::write(const std::string& path, const std::string& text) {
    std::lock_guard<std::mutex> l(m_);
    files_[path] = {text, ++clock_};
    return true;
}
bool MemoryFileSystem::exists(const std::string& path) {
    std::lock_guard<std::mutex> l(m_);
    return files_.count(path) != 0;
}
std::vector<std::string> MemoryFileSystem::list(const std::string& dir) {
    std::lock_guard<std::mutex> l(m_);
    std::string d = dir;
    if (!d.empty() && d.back() != '/') d += '/';
    std::vector<std::string> v;
    for (auto& kv : files_) {
        if (kv.first.compare(0, d.size(), d) != 0) continue;
        std::string rest = kv.first.substr(d.size());
        std::string name = rest.substr(0, rest.find('/'));
        if (std::find(v.begin(), v.end(), name) == v.end()) v.push_back(name);
    }
    return v;
}
long long MemoryFileSystem::modified(const std::string& path) {
    std::lock_guard<std::mutex> l(m_);
    auto it = files_.find(path);
    return it == files_.end() ? -1 : it->second.second;
}

std::string dirName(const std::string& p) {
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? "" : p.substr(0, i);
}
std::string baseName(const std::string& p) {
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}
static bool isAbsolute(const std::string& p) { return !p.empty() && (p[0] == '/' || (p.size() > 1 && p[1] == ':')); }
std::string joinPath(const std::string& dir, const std::string& rel) {
    std::string full = (isAbsolute(rel) || dir.empty()) ? rel : dir + "/" + rel;
    std::vector<std::string> parts;
    std::string prefix;
    size_t i = 0;
    if (!full.empty() && full[0] == '/') { prefix = "/"; i = 1; }
    else if (full.size() > 1 && full[1] == ':') { prefix = full.substr(0, 2) + (full.size() > 2 && full[2] == '/' ? "/" : ""); i = prefix.size(); }
    while (i <= full.size()) {
        size_t j = full.find('/', i);
        if (j == std::string::npos) j = full.size();
        std::string seg = full.substr(i, j - i);
        if (seg == "..") { if (!parts.empty() && parts.back() != "..") parts.pop_back(); else if (prefix.empty()) parts.push_back(seg); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        i = j + 1;
    }
    std::string out = prefix;
    for (size_t k = 0; k < parts.size(); k++) out += (k ? "/" : "") + parts[k];
    return out;
}
std::string relativeTo(const std::string& base, const std::string& path) {
    std::string b = base;
    if (!b.empty() && b.back() != '/') b += '/';
    return !b.empty() && path.compare(0, b.size(), b) == 0 ? path.substr(b.size()) : path;
}

}  // namespace acide
