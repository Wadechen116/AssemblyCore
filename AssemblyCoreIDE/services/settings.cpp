#include "settings.h"

#include <algorithm>
#include <sstream>

namespace acide {

std::string Settings::get(const std::string& k, const std::string& fb) const {
    auto it = map_.find(k);
    return it == map_.end() ? fb : it->second;
}
int Settings::getInt(const std::string& k, int fb) const {
    auto it = map_.find(k);
    if (it == map_.end()) return fb;
    try { return std::stoi(it->second); } catch (...) { return fb; }
}
bool Settings::getBool(const std::string& k, bool fb) const {
    auto it = map_.find(k);
    return it == map_.end() ? fb : (it->second == "1" || it->second == "true");
}
void Settings::set(const std::string& k, const std::string& v) {
    auto it = map_.find(k);
    if (it != map_.end() && it->second == v) return;
    map_[k] = v;
    changed.fire(k);
}
std::vector<std::string> Settings::getList(const std::string& k) const {
    std::vector<std::string> v;
    for (int i = 0;; i++) {
        auto it = map_.find(k + "." + std::to_string(i));
        if (it == map_.end()) break;
        v.push_back(it->second);
    }
    return v;
}
void Settings::setList(const std::string& k, const std::vector<std::string>& v) {
    for (int i = 0; map_.erase(k + "." + std::to_string(i)); i++) {}
    for (size_t i = 0; i < v.size(); i++) map_[k + "." + std::to_string(i)] = v[i];
    changed.fire(k);
}
void Settings::pushRecent(const std::string& k, const std::string& item, size_t max) {
    std::vector<std::string> v = getList(k);
    v.erase(std::remove(v.begin(), v.end(), item), v.end());
    v.insert(v.begin(), item);
    if (v.size() > max) v.resize(max);
    setList(k, v);
}
std::string Settings::serialize() const {
    std::ostringstream o;
    for (auto& kv : map_) o << kv.first << " = " << kv.second << "\n";
    return o.str();
}
void Settings::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
        auto trim = [](std::string s) { size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t"); return b == std::string::npos ? std::string() : s.substr(b, e - b + 1); };
        map_[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}
bool Settings::load(IFileSystem& fs, const std::string& path) {
    std::string t;
    if (!fs.read(path, t)) return false;
    parse(t);
    return true;
}
bool Settings::save(IFileSystem& fs, const std::string& path) const { return fs.write(path, serialize()); }

}  // namespace acide
