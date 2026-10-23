#include "blocks/block_tidy.h"

#include <algorithm>
#include <cmath>

namespace acide {
namespace blocks {

using namespace asmcore::blocks;

std::vector<Pt> tidyPlaces(const Document& doc, const DocumentLayout& layout, const TidyOptions& opt) {
    std::vector<Pt> out(doc.sections.size());
    auto snap = [&](double v) { return opt.snap > 0 ? std::round(v / opt.snap) * opt.snap : v; };
    const std::vector<size_t> program = programOrder(doc);
    // the width of the columns
    double w0 = 0, w1 = 0;
    for (size_t i : program) {
        const double w = layout.stacks[i].bounds.w;
        if (doc.sections[i].domain == "peri") w1 = std::max(w1, w); else w0 = std::max(w0, w);
    }
    const double x0 = 0, x1 = w0 + opt.columnGap, x2 = x1 + w1 + opt.columnGap;
    double colY[2] = {0, 0}, lastY = -1;
    for (size_t i : program) {                                              // the importer's rule: y grows strictly in program order, so the order never changes
        const StackLayout& s = layout.stacks[i];
        const int col = doc.sections[i].domain == "peri" ? 1 : 0;
        const double y = std::max(colY[col], lastY + 1);
        out[i] = {snap((col ? x1 : x0) - s.bounds.x), snap(y) - s.bounds.y};
        // snapping may round y down below the last one: keep the order
        colY[col] = snap(y) + s.bounds.h + opt.rowGap;
        lastY = snap(y);
    }
    // the stacks that are not part of the program, in the order they had (top to bottom, then left to right)
    std::vector<size_t> loose;
    for (size_t i = 0; i < doc.sections.size(); i++) if (doc.sections[i].domain == "floating") loose.push_back(i);
    std::stable_sort(loose.begin(), loose.end(), [&](size_t a, size_t b) {
        const Section &p = doc.sections[a], &q = doc.sections[b];
        return p.y != q.y ? p.y < q.y : p.x < q.x;
    });
    double y = 0;
    for (size_t i : loose) {
        const StackLayout& s = layout.stacks[i];
        out[i] = {snap(x2) - s.bounds.x, snap(y) - s.bounds.y};
        y += s.bounds.h + opt.rowGap;
    }
    return out;
}

size_t tidyUp(BlockSession& session, const DocumentLayout& layout, const TidyOptions& opt) {
    const Document& doc = session.document();
    if (layout.stacks.size() != doc.sections.size()) return 0;
    const std::vector<Pt> places = tidyPlaces(doc, layout, opt);
    size_t moved = 0;
    session.beginGroup("tidy");
    for (size_t i = 0; i < places.size(); i++) {
        if (std::fabs(doc.sections[i].x - places[i].x) < 0.01 && std::fabs(doc.sections[i].y - places[i].y) < 0.01) continue;
        if (session.apply([&](Editor& e) { return e.moveSection(i, places[i].x, places[i].y); }).ok) moved++;
    }
    session.endGroup();
    return moved;
}

}  // namespace blocks
}  // namespace acide
