#pragma once
// AssemblyCore language service, part 1: what the language consists of, and how a line of source is cut into tokens.
//
// This is the layer an editor stands on (syntax colouring, completion, hover, formatting) and has no dependency on any UI. It does
// NOT assemble anything: it needs no symbol table, no includes and works on a single line, so it is fast enough to run on every
// keystroke and stays useful while the program is half typed.
//
// The vocabulary comes from the same tables the assembler encodes with (src/isa.h), so a spec change shows up here by itself. A tool
// that adds an extension can add entries to a Language and every consumer sees them.
#include <set>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"

namespace asmcore {
namespace lang {

enum class Tok {
    Whitespace, Comment, Label, Directive, Mnemonic, SubOp, Region, Condition, ArgName, Number, String, Symbol, Operator, Punct, Unknown
};
const char* tokName(Tok t);                          // "mnemonic", "region", ... (a stable key for theme files)

enum class Dom { None, Main, Peri };                 // the domain a statement belongs to (.main / .peri); None before the first directive
enum DomMask { kMain = 1, kPeri = 2, kBoth = 3 };

struct Token {
    int col = 0, len = 0;                            // 0-based column and length in the line
    Tok kind = Tok::Unknown;
};

// One word of the vocabulary.
struct Entry {
    std::string name;                                // upper case, the spelling in the documentation
    Tok kind = Tok::Mnemonic;
    int domains = kBoth;
    std::string signature;                           // "MOV src -> dst", shown in parameter hints
    std::string doc;                                 // one or two sentences
    std::string spec;                                // spec section, "5.1"
    std::string insert;                              // text a completion inserts when it differs from the name
    std::string group;                               // finer class for SubOp: "alu", "alumod" (signed / unsigned), "csr", "xfer"; empty otherwise
};

class Language {
public:
    // The vocabulary of the baseline spec (honours the profile, e.g. PROFILE_COND0 only when enabled).
    static Language defaults(const Profile& pf = Profile());

    void add(const Entry& e);
    const Entry* find(Tok kind, const std::string& name, Dom d = Dom::None) const;     // name: any case; aliases of regions included
    std::vector<const Entry*> all(Tok kind, Dom d = Dom::None) const;                  // in declaration order, filtered by domain
    const std::vector<Entry>& entries() const { return entries_; }

    // Cut one line into tokens covering it from the first to the last column (whitespace tokens included, so the list can be painted
    // directly). `dom` is the domain at the start of the line and is updated by .main / .peri. If `known` is given, a word that is
    // neither vocabulary nor in the set is Tok::Unknown instead of Tok::Symbol.
    std::vector<Token> tokenizeLine(const std::string& line, Dom& dom, const std::set<std::string>* known = nullptr) const;

    // The mnemonic of a statement ("" if the line has none) and which operand the column is in (0-based, counted by commas and '->');
    // what completion and parameter hints need to know about the cursor.
    // operand: index counted by commas and '->'; word: how many complete words precede the cursor inside that operand ("WRITE8 csr, imm":
    // `WRITE8` is word 0 of operand 0, `csr` word 1); afterEquals: the cursor follows a '=' (a value, not a name).
    struct Cursor { std::string mnemonic; Dom dom = Dom::None; int operand = -1; int word = 0; bool afterEquals = false; bool atStatementStart = false; std::string wordPrefix; };
    Cursor cursorAt(const std::string& line, int col, Dom dom) const;

private:
    std::vector<Entry> entries_;
};

}  // namespace lang
}  // namespace asmcore
