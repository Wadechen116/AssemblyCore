#pragma once
// How a kind of block is drawn, apart from what it means. The model (core) knows nothing about shapes; this table maps a block definition to
// one of a few shapes, so a new kind of block needs a shape only when none of the defaults fits -- and a renderer that does not know a shape
// can fall back to Statement.
#include <map>
#include <string>
#include <vector>

#include "asmcore/blocks.h"

namespace acide {
namespace blocks {

enum class Shape {
    Statement,      // a row with a notch on top and a tab below: it stacks
    Container,      // a head row and one or more slots (a C shape) holding stacks
    Value,          // a plug on the left: it fits into a socket of another block
    Tag,            // a label: a small pill with a name
    Note,           // a comment: a pale note
    Spacer,         // a blank line
    Text            // assembler text kept as it is (Raw): monospace
};

struct ShapeInfo {
    Shape shape = Shape::Statement;
    bool showTitle = true;               // the title ("move") before the fields; the shapes that are only a name or a text do not use it
};

// The shape of the joint of a component: its plug (the tab on its left edge) and the dent of the socket it fits. The shape follows the value TYPE, so
// a number cannot be mistaken for a region or an option, and a socket shows what it takes.
enum class PlugShape {
    None,
    Round,           // int: numbers, names, expressions
    Point,           // region:*  (a pointed tab)
    Square,          // options: alu operation, sign, conditions
    Flag,            // label.flow, label.proc
    Diamond,         // references: csr, service, dev
    Any              // a socket that takes several shapes: a plain square dent, dotted
};
PlugShape plugShapeOf(const std::string& valueType);
PlugShape plugShapeOfSocket(const std::vector<std::string>& accepts);

class ShapeRegistry {
public:
    static const ShapeRegistry& defaults();
    // The explicit entry for a type if there is one, otherwise derived from the definition (slots: Container, valueType: Value, else Statement).
    ShapeInfo infoFor(const asmcore::blocks::BlockDef& def) const;
    void set(const std::string& type, ShapeInfo info) { byType_[type] = info; }

private:
    std::map<std::string, ShapeInfo> byType_;
};

}  // namespace blocks
}  // namespace acide
