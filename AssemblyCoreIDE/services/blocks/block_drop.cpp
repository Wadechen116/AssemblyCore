#include "blocks/block_drop.h"

#include <algorithm>
#include <cmath>

namespace acide {
namespace blocks {

using namespace asmcore::blocks;

namespace {

const std::vector<Block>* listOf(const Document& doc, size_t stack, const std::string& container, const std::string& slot) {
    if (stack >= doc.sections.size()) return nullptr;
    if (container.empty()) return &doc.sections[stack].blocks;
    const Block* c = doc.find(container);
    return c ? c->stack(slot) : nullptr;
}

double dist(Pt a, Pt b) { return std::hypot(a.x - b.x, a.y - b.y); }

std::string reasons(const std::vector<Violation>& v, const std::string& error) {
    std::string s = describe(v);
    if (s.empty()) s = error;
    return s;
}

}  // namespace

Where whereOf(const Document& doc, size_t stack, const ConnectionPoint& p) {
    Where w;
    w.section = stack;
    if (p.kind == ConnectionPoint::Slot) {                                   // the start of a slot
        w.container = p.blockId;
        w.slot = p.slot;
        const std::vector<Block>* l = listOf(doc, stack, w.container, w.slot);
        if (l && !l->empty()) w.before = l->front().id;
        return w;
    }
    if (p.blockId.empty()) {                                                 // under the hat: the start of the section
        const std::vector<Block>* l = listOf(doc, stack, "", "");
        if (l && !l->empty()) w.before = l->front().id;
        return w;
    }
    const Location loc = locate(doc, p.blockId);
    if (!loc.found) return w;
    w.container = loc.container;
    w.slot = loc.slot;
    const std::vector<Block>* l = listOf(doc, loc.section, loc.container, loc.slot);
    if (!l) return w;
    w.section = loc.section;
    if (p.kind == ConnectionPoint::Previous) { w.before = p.blockId; return w; }
    if (loc.index + 1 < l->size()) w.before = (*l)[loc.index + 1].id;       // under p.blockId: in front of its next sibling (or at the end)
    return w;
}

DropTarget findDropTarget(BlockSession& session, const DocumentLayout& layout, size_t payload, Pt origin, const DropOptions& opt) {
    DropTarget best;
    const Document& doc = session.document();
    if (payload >= layout.stacks.size() || payload >= doc.sections.size()) return best;
    const StackLayout& ps = layout.stacks[payload];
    if (doc.sections[payload].domain != "floating" || ps.top.empty()) return best;
    const BlockBox& first = ps.blocks[(size_t)ps.top[0]];
    const BlockDef* fd = session.registry().find(first.type);
    const bool value = fd && !fd->valueType.empty();

    auto consider = [&](DropTarget cand) {
        if (cand.kind == DropTarget::None) return;
        // a target that fits always beats one that does not; then the nearer
        const bool candFits = cand.fits(), bestFits = best.fits();
        if (best.kind == DropTarget::None || (candFits && !bestFits) || (candFits == bestFits && cand.distance < best.distance)) best = cand;
    };

    if (value) {
        const Pt tip{origin.x + first.rect.x, origin.y + first.rect.center().y};
        for (size_t si = 0; si < layout.stacks.size(); si++) {
            if (si == payload) continue;
            const StackLayout& ts = layout.stacks[si];
            for (const ConnectionPoint& cp : ts.points) {
                if (cp.kind != ConnectionPoint::Socket) continue;
                const Pt q{ts.origin.x + cp.pos.x, ts.origin.y + cp.pos.y};
                const double d = dist(tip, q);
                if (d > opt.snapRadius) continue;
                DropTarget t;
                t.kind = DropTarget::Socket;
                t.hostId = cp.blockId;
                t.socket = cp.slot;
                t.distance = d;
                t.marker = q;
                const BlockBox* host = ts.find(cp.blockId);
                if (host)
                    for (const SocketBox& sb : host->sockets)
                        if (sb.name == cp.slot) t.socketRect = Rect{ts.origin.x + sb.rect.x, ts.origin.y + sb.rect.y, sb.rect.w, sb.rect.h};
                const Outcome o = session.probe([&](Editor& e) { return e.plugSection(payload, cp.blockId, cp.slot, true); });
                t.fit = o.ok ? o.fit : Fit::No;
                t.violations = o.violations;
                t.reason = o.ok && o.fit == Fit::Ok ? std::string() : reasons(o.violations, o.error);
                consider(t);
            }
        }
        return best;
    }

    const Pt top{origin.x + first.rect.x, origin.y + first.rect.y};
    for (size_t si = 0; si < layout.stacks.size(); si++) {
        if (si == payload) continue;
        const StackLayout& ts = layout.stacks[si];
        const bool floating = doc.sections[si].domain == "floating";
        for (const ConnectionPoint& cp : ts.points) {
            // under a block, in a slot, under a hat; above the first block of a floating stack
            const bool ok = cp.kind == ConnectionPoint::Next || cp.kind == ConnectionPoint::Slot ||
                            (cp.kind == ConnectionPoint::Previous && floating && !ts.top.empty() && ts.blocks[(size_t)ts.top[0]].id == cp.blockId);
            if (!ok) continue;
            const Pt q{ts.origin.x + cp.pos.x, ts.origin.y + cp.pos.y};
            const double d = dist(top, q);
            if (d > opt.snapRadius) continue;
            DropTarget t;
            t.kind = DropTarget::Insert;
            t.where = whereOf(doc, si, cp);
            t.marker = q;
            t.distance = d;
            const Outcome o = session.probe([&](Editor& e) { return e.joinSection(payload, t.where, true); });
            t.fit = o.ok ? o.fit : Fit::No;
            t.violations = o.violations;
            t.reason = o.ok && o.fit == Fit::Ok ? std::string() : reasons(o.violations, o.error);
            consider(t);
        }
    }
    return best;
}

Outcome performDrop(BlockSession& session, size_t payload, const DropTarget& t) {
    if (t.kind == DropTarget::Socket) return session.apply([&](Editor& e) { return e.plugSection(payload, t.hostId, t.socket); });
    if (t.kind == DropTarget::Insert) return session.apply([&](Editor& e) { return e.joinSection(payload, t.where); });
    Outcome none;
    none.error = "nothing to drop on";
    return none;
}

}  // namespace blocks
}  // namespace acide
