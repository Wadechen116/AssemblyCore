#pragma once
// What can be put on the canvas: the drawers of the toolbox, the samples in them, the patterns, and the search that Typeblock uses. Everything comes from the
// Registry (a new block definition appears in the toolbox without any UI change); only the sample text of an instruction -- what a fresh block says -- is kept
// here, as plain assembler, because the importer turns it into exactly the blocks a person would build. No Qt.
#include <string>
#include <vector>

#include "asmcore/blocks.h"

namespace acide {
namespace blocks {

struct CatalogEntry {
    std::string id;                    // "type:mov", "pattern:add_two"
    std::string title;                 // "move"
    std::string type;                  // the block type (empty for a pattern)
    std::string category, cls;         // the drawer, and the class (instruction, component, structure, definition, text, pattern)
    int domains = asmcore::blocks::kBoth;
    std::string text;                  // the assembler text of the sample: what a fresh block (or a pattern) is made from ("" for a component)
    std::string description;
    bool component() const { return cls == "component"; }
    bool pattern() const { return cls == "pattern"; }
};

class BlockCatalog {
public:
    explicit BlockCatalog(const asmcore::blocks::Registry& reg = asmcore::blocks::Registry::defaults());

    const std::vector<CatalogEntry>& entries() const { return entries_; }
    const CatalogEntry* find(const std::string& id) const;

    struct Drawer {
        std::string name, cls;
        std::vector<const CatalogEntry*> entries;
    };
    // The drawers in the order of the toolbox: patterns, the instruction drawers, structure, definitions, text, then the components.
    std::vector<Drawer> drawers() const;

    // The entries that match what was typed, best first: by title, type, mnemonic (the first word of the sample), drawer. Nothing for an empty query.
    std::vector<const CatalogEntry*> search(const std::string& query, size_t limit = 12) const;

    // Fresh blocks of an entry, with ids from `doc`: an instruction / structure / definition from its sample text, a component built with its parts, a pattern as
    // several blocks. Never empty.
    std::vector<asmcore::blocks::Block> instantiate(const CatalogEntry& e, asmcore::blocks::Document& doc) const;

    // A typed line (or lines) of assembler as blocks (what Typeblock does when the text is not a name). anyRaw: a line was not understood and stays as text.
    std::vector<asmcore::blocks::Block> fromAssembler(asmcore::blocks::Document& doc, const std::string& text, bool* anyRaw = nullptr) const;
    // True if the text looks like a line of assembler rather than a name to search for (a known mnemonic or directive first, or a label).
    bool looksLikeAssembler(const std::string& text) const;

private:
    const asmcore::blocks::Registry& reg_;
    std::vector<CatalogEntry> entries_;
};

}  // namespace blocks
}  // namespace acide
