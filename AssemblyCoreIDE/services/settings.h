#pragma once
// User settings: a flat key/value store saved as "key = value" lines. The keys are plain strings owned by whoever uses them; nothing
// here knows what a font or a theme is.
#include <map>
#include <string>
#include <vector>

#include "fs.h"
#include "signal.h"

namespace acide {

class Settings {
public:
    std::string get(const std::string& key, const std::string& fallback = "") const;
    int getInt(const std::string& key, int fallback = 0) const;
    bool getBool(const std::string& key, bool fallback = false) const;
    void set(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int v) { set(key, std::to_string(v)); }
    void setBool(const std::string& key, bool v) { set(key, v ? "1" : "0"); }
    bool has(const std::string& key) const { return map_.count(key) != 0; }
    // Lists are stored as key.0, key.1, ...
    std::vector<std::string> getList(const std::string& key) const;
    void setList(const std::string& key, const std::vector<std::string>& v);
    // Moves `item` to the front of a most-recently-used list of at most `max` entries.
    void pushRecent(const std::string& key, const std::string& item, size_t max = 10);

    bool load(IFileSystem& fs, const std::string& path);
    bool save(IFileSystem& fs, const std::string& path) const;
    std::string serialize() const;
    void parse(const std::string& text);

    Signal<const std::string&> changed;

private:
    std::map<std::string, std::string> map_;
};

}  // namespace acide
