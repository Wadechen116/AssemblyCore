#pragma once
// Where can the stack that is being dragged be dropped? No Qt: it works on the layout (where everything is) and asks the editor, as a dry run, whether
// the rules allow it -- the same answer the drop itself will get, so what the canvas shows while dragging is what happens when the mouse is released.
//
//   a lone component  -> the sockets near its plug         (Socket target; plugSection)
//   instructions      -> the connection points near their top: under a block, in the slot of a container, under a hat, above a floating stack (Insert target; joinSection)
#include <string>
#include <vector>

#include "asmcore/blocks_edit.h"
#include "blocks/block_layout.h"
#include "blocks/block_session.h"

namespace acide {
namespace blocks {

struct DropTarget {
    enum Kind { None, Socket, Insert } kind = None;
    asmcore::blocks::Fit fit = asmcore::blocks::Fit::No;
    std::vector<asmcore::blocks::Violation> violations;
    std::string reason;                           // the reasons in plain words (for a tooltip), "" if it fits
    // Socket:
    std::string hostId, socket;
    Rect socketRect;                              // canvas coordinates: the hole
    // Insert:
    asmcore::blocks::Where where;
    Pt marker;                                    // canvas coordinates of the connection point (where a bar is drawn)
    double distance = 0;
    bool fits() const { return kind != None && fit != asmcore::blocks::Fit::No; }
};

struct DropOptions {
    double snapRadius = 28;                       // how near the payload must come (canvas units)
};

// The place in the document of a connection point of a stack ("insert here").
asmcore::blocks::Where whereOf(const asmcore::blocks::Document& doc, size_t stack, const ConnectionPoint& p);

// The best place for the floating stack `payload`, drawn with its origin at `origin`. kind None if nothing is near.
DropTarget findDropTarget(BlockSession& session, const DocumentLayout& layout, size_t payload, Pt origin, const DropOptions& opt = DropOptions());

// The drop itself: the same operation the probe asked about. Returns what the editor said (ok = it was done).
asmcore::blocks::Outcome performDrop(BlockSession& session, size_t payload, const DropTarget& target);

}  // namespace blocks
}  // namespace acide
