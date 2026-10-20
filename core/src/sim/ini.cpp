#include "ini.h"

#include <sstream>

#include "../util.h"

namespace asmcore {
namespace sim {

std::vector<IniSection> parseIni(const std::string& text, std::vector<std::string>& errors) {
    std::vector<IniSection> out;
    IniSection cur;
    cur.kind = "sim";
    bool curUsed = false;
    std::istringstream in(text);
    std::string raw;
    int n = 0;
    auto flush = [&] {
        if (curUsed || !cur.kv.empty() || cur.kind != "sim") out.push_back(cur);
    };
    while (std::getline(in, raw)) {
        n++;
        std::string line = trim(stripComment(raw));
        if (line.empty()) continue;
        if (line[0] == '[') {
            if (line.back() != ']') { errors.push_back("line " + std::to_string(n) + ": ']' expected"); continue; }
            flush();
            cur = IniSection();
            cur.line = n;
            std::string head = trim(line.substr(1, line.size() - 2));
            size_t sp = head.find_first_of(" \t");
            cur.kind = lower(sp == std::string::npos ? head : head.substr(0, sp));
            cur.name = sp == std::string::npos ? "" : trim(head.substr(sp));
            curUsed = true;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) { errors.push_back("line " + std::to_string(n) + ": 'key = value' expected"); continue; }
        cur.kv.push_back({lower(trim(line.substr(0, eq))), trim(line.substr(eq + 1))});
    }
    flush();
    return out;
}

}  // namespace sim
}  // namespace asmcore
