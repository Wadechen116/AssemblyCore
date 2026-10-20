#pragma once
// Section parser for the simulator's environment file.
//
//   [kind name]            e.g. [csr POLL_CTRL], [device imu], [timing], [condition main 0xB]
//   key = value            the key may contain spaces ("reg 0x3A = 0x00, 0x01"); a key may repeat
//   ; comment
#include <string>
#include <utility>
#include <vector>

namespace asmcore {
namespace sim {

struct IniSection {
    std::string kind;                 // lower case
    std::string name;                 // rest of the header, trimmed (may be empty)
    int line = 0;
    std::vector<std::pair<std::string, std::string>> kv;   // key lower-cased, value as written

    const std::string* get(const std::string& key) const {
        for (auto& p : kv) if (p.first == key) return &p.second;
        return nullptr;
    }
    std::vector<std::string> all(const std::string& key) const {
        std::vector<std::string> v;
        for (auto& p : kv) if (p.first == key) v.push_back(p.second);
        return v;
    }
};

// Lines before the first header belong to an implicit section with kind "sim" (so a bare file of key = value works).
std::vector<IniSection> parseIni(const std::string& text, std::vector<std::string>& errors);

}  // namespace sim
}  // namespace asmcore
