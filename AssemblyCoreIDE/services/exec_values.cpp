#include "exec_values.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <regex>

namespace acide {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string hexBytes(const SimView& v, const std::string& region, int from, int to) {
    const auto it = v.regions.find(region);
    if (it == v.regions.end()) return std::string();
    std::string out;
    char b[8];
    for (int i = from; i <= to && i < (int)it->second.size(); i++) {
        std::snprintf(b, sizeof b, "%s%02X", out.empty() ? "" : " ", it->second[(size_t)i]);
        out += b;
    }
    return out;
}

int64_t signalOf(const SimView& v, const std::string& name, bool& found) {
    for (const auto& kv : v.signalValues) if (kv.first == name) { found = true; return kv.second; }
    found = false;
    return 0;
}

}  // namespace

std::string instructionResult(const std::string& text, int domain, const SimView& v) {
    if (domain < 0 || domain > 1) return std::string();
    const std::string t = upper(text);
    std::string mn;
    for (char c : t) { if (std::isspace((unsigned char)c)) break; mn += c; }
    std::smatch m;
    if (mn == "MOV") {
        static const std::regex dst("->\\s*([A-Z_]+)\\s*(?:\\[\\s*(\\d+)\\s*(?:\\.\\.\\s*(\\d+))?\\s*\\])?");
        if (!std::regex_search(t, m, dst)) return std::string();
        const std::string reg = m[1].str();
        const int a = m[2].matched ? std::atoi(m[2].str().c_str()) : 0;
        const int b = m[3].matched ? std::atoi(m[3].str().c_str()) : m[2].matched ? a : 1;
        std::string region, label;
        if (reg == "MAIN_SCRATCHPAD") { region = "scratch"; label = "scratch[" + std::to_string(a) + (b != a ? ".." + std::to_string(b) : "") + "]"; }
        else if (reg == "ALU_OPA") { region = "opa"; label = "OPA"; }
        else if (reg == "ALU_OPB") { region = "opb"; label = "OPB"; }
        else if (reg == "MAIN_CSR_DATA_BUF") { region = "csrbuf"; label = "csr_buf[" + std::to_string(a) + (b != a ? ".." + std::to_string(b) : "") + "]"; }
        else return std::string();
        const std::string bytes = hexBytes(v, region, a, b);
        return bytes.empty() ? std::string() : label + " = " + bytes;
    }
    if (mn == "ALU") {
        std::string out = "OUT " + hexBytes(v, "out", 0, 1);
        bool f;
        if (signalOf(v, "alu.cmp_true", f) && f) out += " cmp";
        if (signalOf(v, "alu.zero", f) && f) out += " Z";
        if (signalOf(v, "alu.negative", f) && f) out += " N";
        return out;
    }
    if (mn == "EXEC_CSR") {
        static const std::regex name("^EXEC_CSR\\s+[A-Z0-9_]+\\s+([A-Z_][A-Z0-9_]*)");
        if (!std::regex_search(t, m, name)) return std::string();
        for (const auto& c : v.csrs) {
            if (upper(c.first) != m[1].str()) continue;
            char b[48];
            std::snprintf(b, sizeof b, "=0x%02X", c.second);
            return c.first + b;
        }
        return std::string();
    }
    if (mn == "EXEC_PUB") {
        bool f;
        const int64_t svc = signalOf(v, "slot.service_id", f);
        int len = 1;
        if (std::regex_search(t, m, std::regex("LEN\\s*=\\s*(\\d+)"))) len = std::atoi(m[1].str().c_str());
        return std::string("slot svc ") + std::to_string((long long)svc) + " : " + hexBytes(v, "slot", 0, std::max(0, len - 1));
    }
    if (mn == "EXEC_XFER") {
        int len = 1;
        if (std::regex_search(t, m, std::regex("LEN\\s*=\\s*(\\d+)"))) len = std::atoi(m[1].str().c_str());
        const std::string bytes = hexBytes(v, "rx", 0, std::max(0, std::min(len, 8) - 1));
        return bytes.empty() ? std::string() : "rx " + bytes;
    }
    if (mn.compare(0, 2, "BR") == 0 && mn != "BRANCH") {
        std::string cond;
        if (mn == "BR.COND") {
            std::smatch c;
            if (std::regex_search(t, c, std::regex("^BR\\.COND\\s+([A-Z0-9_]+)"))) cond = c[1].str();
        } else if (mn.compare(0, 3, "BR.") == 0) {
            cond = mn.substr(3);
        }
        if (cond.empty()) return std::string();
        for (const auto& c : v.conditions[domain])
            if (c.first == cond) return cond + (c.second ? " \xE2\x97\x8F true" : " \xE2\x97\x8B false");
        return std::string();
    }
    return std::string();
}

}  // namespace acide
