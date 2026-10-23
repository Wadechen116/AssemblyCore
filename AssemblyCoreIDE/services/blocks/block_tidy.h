#pragma once
// "Tidy up": gives every stack a place so that nothing overlaps and the program reads from top to bottom -- and the program stays exactly what it was, because
// the order the text is generated in IS the canvas order (docs/BLOCKS.md): the definitions and the Main / Peri sections keep their order, each one strictly
// below the one before it. Definitions and Main go in the left column, Peri in the next one (as the importer lays a file out); stacks that lie on the canvas
// without belonging to the program stand in a column of their own on the right, in the order they had. No Qt.
#include "blocks/block_layout.h"
#include "blocks/block_session.h"

namespace acide {
namespace blocks {

struct TidyOptions {
    double columnGap = 70;                     // between the columns
    double rowGap = 36;                        // between stacks of a column
    double snap = 0;                           // > 0: positions are rounded to this grid
};

// The place each stack would get (canvas coordinates of the top-left of its picture), in the order of Document::sections.
std::vector<Pt> tidyPlaces(const asmcore::blocks::Document& doc, const DocumentLayout& layout, const TidyOptions& opt = TidyOptions());

// Moves the stacks there (one undo step). Returns how many stacks moved.
size_t tidyUp(BlockSession& session, const DocumentLayout& layout, const TidyOptions& opt = TidyOptions());

}  // namespace blocks
}  // namespace acide
