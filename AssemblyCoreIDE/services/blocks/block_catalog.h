#pragma once
// What can be put on the canvas: the drawers of the toolbox, the samples in them, the patterns, and the search that Typeblock uses. Everything comes from the
// Registry (a new block definition appears in the toolbox without any UI change); only the sample text of an instruction -- what a fresh block says -- is kept
// here, as plain assembler, because the importer turns it into exactly the blocks a person would build. No Qt.
#include <string>
#include <vector>

#include "asmcore/blocks.h"
#include "asmcore/hardware.h"

namespace acide {
namespace blocks {

// A block of the user's own in a drawer of the toolbox: a name and the assembler text it is made from (several lines make several blocks; in a component drawer
// the text is an operand, e.g. MAIN_SCRATCHPAD[4..7] or LIMIT + 1). Kept in the settings, one line each.
struct CustomBlock {
    std::string drawer, title, text;
};
std::string encodeCustom(const CustomBlock& c);                      // one line, no line break in it
bool decodeCustom(const std::string& line, CustomBlock& out);

struct CatalogEntry {
    std::string id;                    // "type:mov", "pattern:add_two"
    std::string title;                 // "move"
    std::string type;                  // the block type (empty for a pattern)
    std::string category, cls;         // the drawer, and the class (instruction, component, structure, definition, text, pattern)
    int domains = asmcore::blocks::kBoth;
    std::string text;                  // the assembler text of the sample: what a fresh block (or a pattern) is made from ("" for a component)
    std::string description;
    bool custom = false;               // made by the user (can be edited and deleted)
    bool hardware = false;             // made from the hardware description of the project (follows it; shown with the name of the part in it)
    bool operand = false;              // a component whose `text` is an operand (a name, a range): custom components and the names of the description
    bool component() const { return cls == "component"; }
    bool pattern() const { return cls == "pattern"; }
};

class BlockCatalog {
public:
    explicit BlockCatalog(const asmcore::blocks::Registry& reg = asmcore::blocks::Registry::defaults());

    const std::vector<CatalogEntry>& entries() const { return entries_; }
    const CatalogEntry* find(const std::string& id) const;

    // ---- the hardware description of the project: every part of it brings its own blocks (a CSR: write / set bits / clear bits / read back; a device: read / write;
    // a service: publish; a constant: load it; a name: the name itself). Drawers "Hardware CSR", "Hardware Devices", "Hardware Services", "Hardware Data", "Hardware Names".
    // Returns whether the entries changed (the toolbox is built again only then).
    bool setHardware(const asmcore::Hardware& hw);
    bool clearHardware() { return setHardware(asmcore::Hardware()); }
    size_t hardwareCount() const { return hardware_.size(); }

    // ---- the user's own blocks
    void setCustom(std::vector<CustomBlock> c);                       // replaces them all; the entries are rebuilt
    const std::vector<CustomBlock>& custom() const { return custom_; }
    static std::string customId(const CustomBlock& c) { return "custom:" + c.drawer + "/" + c.title; }
    std::string drawerClass(const std::string& drawer) const;         // the class of the blocks in a drawer ("instruction" if the drawer is unknown)
    struct Check {
        bool ok = false;
        std::string message;           // why not, or a note ("2 lines are not understood and stay as text")
        int blocks = 0, raw = 0;
    };
    // Can this be a block of the toolbox? A title, a text that makes blocks (an operand in a component drawer), and no other block with that title in the drawer
    // (`replacing`: the id of the entry being edited, which may keep its name).
    Check checkCustom(const CustomBlock& c, const std::string& replacing = "") const;

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
    void rebuild();
    const asmcore::blocks::Registry& reg_;
    std::vector<CatalogEntry> builtin_, hardware_, entries_;
    std::vector<CustomBlock> custom_;
};

}  // namespace blocks
}  // namespace acide
