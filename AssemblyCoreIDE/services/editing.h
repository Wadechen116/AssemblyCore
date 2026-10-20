#pragma once
// Editor intelligence as pure functions of (text, language, index): completion, hover text, rename. No UI, no threads, so each can be
// tested directly and reused by another front end.
#include <map>
#include <string>
#include <vector>

#include "asmcore/index.h"
#include "asmcore/lang.h"

namespace acide {

struct CompletionItem {
    std::string label;                // what the list shows
    std::string insert;               // what replaces the typed prefix
    std::string detail;               // signature or explanation
    std::string kind;                 // a lang::tokName, or "label" / "csr" / ... for symbols
};

// Items that fit at `col` of `line` in domain `dom`, already filtered by the typed prefix and sorted. `index` may be null.
std::vector<CompletionItem> complete(const asmcore::lang::Language& language, const asmcore::Index* index, const std::string& line, int col, asmcore::lang::Dom dom);

// Text for a tooltip about the word at `col` ("" if there is nothing to say). `result` (optional) supplies symbol values.
std::string hoverText(const asmcore::lang::Language& language, const asmcore::Index* index, const asmcore::Result* result, const std::string& file, int lineNo,
                      const std::string& line, int col, asmcore::lang::Dom dom);

// The domain in force at the start of 1-based line `lineNo` of `text` (follows .main / .peri).
asmcore::lang::Dom domainAtLine(const asmcore::lang::Language& language, const std::string& text, int lineNo);

// Rename: the new text of every file that changes. `texts` maps path -> current text for the files holding occurrences. Fails (returns an
// empty map and sets `problem`) if the new name is not allowed.
std::map<std::string, std::string> renameEdits(const asmcore::Index& index, const asmcore::IndexSymbol& symbol, const std::string& newName,
                                               const std::map<std::string, std::string>& texts, std::string& problem);

}  // namespace acide
