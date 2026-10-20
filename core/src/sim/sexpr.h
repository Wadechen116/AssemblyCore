#pragma once
// Expression engine shared by branch conditions, breakpoint conditions, watch expressions, `run until` and scenario `expect`.
//
//   literals   123  0x7F  7Fh  0b101  101b            operators (C precedence)  ! ~ -   * / %   + -   << >>   < <= > >=   == !=   &  ^  |  &&  ||
//   names      slot.valid  vblank_open  main.pc       indexing  scratch[3]   scratch[8..9] (little-endian value of the bytes)
//
// An expression is compiled once against a Binder (names -> integer handles); evaluating it later costs no string lookups.
#include <cstdint>
#include <memory>
#include <string>

namespace asmcore {
namespace sim {

class ExprBinder {
public:
    virtual ~ExprBinder() {}
    // Returns a handle >= 0, or -1 if the name is unknown. `indexed` tells whether the name was followed by [..].
    virtual int bind(const std::string& name, bool indexed) = 0;
};
class ExprEnv {
public:
    virtual ~ExprEnv() {}
    // idx / len are 0 / 0 for a plain name; for name[i] they are i / 1; for name[a..b] they are a / b-a+1.
    virtual int64_t get(int handle, int64_t idx, int len) const = 0;
};

class Expr {
public:
    Expr() {}
    static bool compile(const std::string& text, ExprBinder& binder, Expr& out, std::string& error);
    bool valid() const { return (bool)root_; }
    int64_t eval(const ExprEnv& env) const;
    const std::string& text() const { return text_; }
    struct Node;
private:
    std::shared_ptr<const Node> root_;
    std::string text_;
};

}  // namespace sim
}  // namespace asmcore
