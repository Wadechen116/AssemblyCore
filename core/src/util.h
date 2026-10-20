#pragma once
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace asmcore {

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
inline std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
inline bool isIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_' || c == '.' || c == '@' || c == '$' || c == '?'; }
inline bool isIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '@' || c == '$' || c == '?'; }
inline bool isIdent(const std::string& s) {
    if (s.empty() || !isIdentStart(s[0])) return false;
    for (char c : s) if (!isIdentChar(c)) return false;
    return true;
}

// A string as the inside of a JSON string literal (quotes, backslash and control characters escaped; UTF-8 passes through).
std::string jsonEscape(const std::string& s);

// Removes a trailing ";" comment (outside quotes).
std::string stripComment(const std::string& line);

// Splits on top-level separators (default ','): not inside [] () or quotes.
std::vector<std::string> splitTop(const std::string& s, char sep = ',');

// Finds `needle` at top level (outside [] () and quotes); npos if absent.
size_t findTop(const std::string& s, const std::string& needle);

// Expression evaluator. Numbers: 123, 0x7F, 7Fh, 0b101, 101b, 'A'. Operators: ( ) unary + - ~, * / %, + -, << >>, &, ^, |.
// Identifiers are looked up through `lookup`; on failure `err` says what was wrong.
using SymbolLookup = std::function<bool(const std::string& name, int64_t& value)>;
bool evalExpr(const std::string& text, const SymbolLookup& lookup, int64_t& out, std::string& err);

}  // namespace asmcore
