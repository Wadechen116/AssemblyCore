#pragma once
// A small JSON value, reader and writer for the tool's own files (.acblk). Objects keep their key order, so what is written is stable
// and diff-friendly. Not a general-purpose library: numbers are doubles, strings are UTF-8 with the usual escapes (\uXXXX is read, and
// written only for control characters).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace asmcore {
namespace json {

struct Value {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    static Value makeString(const std::string& s) { Value v; v.kind = String; v.str = s; return v; }
    static Value makeNumber(double d) { Value v; v.kind = Number; v.num = d; return v; }
    static Value makeBool(bool x) { Value v; v.kind = Bool; v.b = x; return v; }
    static Value makeArray() { Value v; v.kind = Array; return v; }
    static Value makeObject() { Value v; v.kind = Object; return v; }

    const Value* get(const std::string& key) const {
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    Value& set(const std::string& key, Value v) { obj.emplace_back(key, std::move(v)); return obj.back().second; }
    void push(Value v) { arr.push_back(std::move(v)); }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}
    // false with error() set (message with line and column)
    bool parse(Value& out) {
        if (!value(out, 0)) return false;
        ws();
        if (i_ != s_.size()) return fail("unexpected text after the JSON value");
        return true;
    }
    const std::string& error() const { return err_; }

private:
    const std::string& s_;
    size_t i_ = 0;
    std::string err_;

    bool fail(const std::string& m) {
        if (!err_.empty()) return false;
        int line = 1, col = 1;
        for (size_t k = 0; k < i_ && k < s_.size(); k++) { if (s_[k] == '\n') { line++; col = 1; } else col++; }
        err_ = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + m;
        return false;
    }
    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++; }
    bool lit(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (s_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    bool str(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return fail("a string was expected");
        i_++;
        out.clear();
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_];
            if (c == '\\') {
                if (++i_ >= s_.size()) return fail("unterminated escape");
                switch (s_[i_]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i_ + 4 >= s_.size()) return fail("bad \\u escape");
                        unsigned cp = (unsigned)std::strtoul(s_.substr(i_ + 1, 4).c_str(), nullptr, 16);
                        i_ += 4;
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out += s_[i_];
                }
            } else out += c;
            i_++;
        }
        if (i_ >= s_.size()) return fail("unterminated string");
        i_++;
        return true;
    }
    bool value(Value& v, int depth) {
        if (depth > 64) return fail("nesting is too deep");
        ws();
        if (i_ >= s_.size()) return fail("unexpected end of the file");
        char c = s_[i_];
        if (c == '{') {
            v.kind = Value::Object;
            i_++;
            ws();
            if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_] != ':') return fail("':' was expected");
                i_++;
                Value child;
                if (!value(child, depth + 1)) return false;
                v.obj.emplace_back(k, std::move(child));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') { i_++; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
                return fail("',' or '}' was expected");
            }
        }
        if (c == '[') {
            v.kind = Value::Array;
            i_++;
            ws();
            if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
            for (;;) {
                Value child;
                if (!value(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') { i_++; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
                return fail("',' or ']' was expected");
            }
        }
        if (c == '"') { v.kind = Value::String; return str(v.str); }
        if (lit("true")) { v.kind = Value::Bool; v.b = true; return true; }
        if (lit("false")) { v.kind = Value::Bool; v.b = false; return true; }
        if (lit("null")) { v.kind = Value::Null; return true; }
        char* end = nullptr;
        double d = std::strtod(s_.c_str() + i_, &end);
        if (end == s_.c_str() + i_) return fail("a value was expected");
        v.kind = Value::Number;
        v.num = d;
        i_ = (size_t)(end - s_.c_str());
        return true;
    }
};

inline std::string escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04X", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// Pretty-printed with two-space indentation. Short arrays / objects of scalars stay on one line.
inline void write(const Value& v, std::string& out, int indent = 0) {
    auto pad = [&](int n) { out.append((size_t)n * 2, ' '); };
    switch (v.kind) {
        case Value::Null: out += "null"; break;
        case Value::Bool: out += v.b ? "true" : "false"; break;
        case Value::Number: { char b[40]; std::snprintf(b, sizeof b, "%.15g", v.num); out += b; break; }
        case Value::String: out += "\"" + escape(v.str) + "\""; break;
        case Value::Array:
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.arr.size(); i++) { pad(indent + 1); write(v.arr[i], out, indent + 1); out += i + 1 < v.arr.size() ? ",\n" : "\n"; }
            pad(indent);
            out += "]";
            break;
        case Value::Object: {
            if (v.obj.empty()) { out += "{}"; break; }
            bool flat = true;                                              // an object of scalars is written on one line
            for (const auto& kv : v.obj) if (kv.second.kind == Value::Array || kv.second.kind == Value::Object) flat = false;
            if (flat && v.obj.size() <= 6) {
                out += "{";
                for (size_t i = 0; i < v.obj.size(); i++) { out += (i ? ", " : "") + std::string("\"") + escape(v.obj[i].first) + "\": "; write(v.obj[i].second, out, indent); }
                out += "}";
                break;
            }
            out += "{\n";
            for (size_t i = 0; i < v.obj.size(); i++) {
                pad(indent + 1);
                out += "\"" + escape(v.obj[i].first) + "\": ";
                write(v.obj[i].second, out, indent + 1);
                out += i + 1 < v.obj.size() ? ",\n" : "\n";
            }
            pad(indent);
            out += "}";
            break;
        }
    }
}

}  // namespace json
}  // namespace asmcore
