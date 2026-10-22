#include "block_painter.h"

#include <QFontMetricsF>
#include <QPen>

namespace acide {

using namespace blocks;
using asmcore::blocks::FieldKind;

QColor toQColor(const Rgba& c) { return QColor(c.r, c.g, c.b, c.a); }

BlockPaintState StackPaintInfo::stateOf(const std::string& id) const {
    BlockPaintState s;
    s.selected = selected && selected->count(id);
    s.hovered = !hovered.empty() && hovered == id;
    s.hatched = hatched && hatched->count(id);
    s.error = errors && errors->count(id);
    s.warning = warnings && warnings->count(id);
    return s;
}

static QRectF qr(const Rect& r) { return QRectF(r.x, r.y, r.w, r.h); }

namespace {

constexpr double kRadius = 6, kNotchX = 14, kNotchW = 20, kNotchH = 5;

QPainterPath roundedRect(const QRectF& r, double rad) {
    QPainterPath p;
    p.addRoundedRect(r, rad, rad);
    return p;
}
// The joint between stacked blocks: a smooth, rounded hill under a block (tab) and the matching hollow in the top of the block below (notch). Both are the
// same curve, so a block fits exactly into the one above it; there are no corners.
QPainterPath hill(double x0, double y0, double dir) {          // dir: +1 a hill below the edge y0 (tab), -1... (the notch uses the same curve downward from the top edge)
    QPainterPath n;
    const double W = kNotchW, H = kNotchH * dir;
    n.moveTo(x0, y0);
    n.cubicTo(x0 + W * 0.22, y0, x0 + W * 0.25, y0 + H, x0 + W * 0.5, y0 + H);
    n.cubicTo(x0 + W * 0.75, y0 + H, x0 + W * 0.78, y0, x0 + W, y0);
    n.closeSubpath();
    return n;
}
QPainterPath notch(const QRectF& r) { return hill(r.left() + kNotchX, r.top(), 1); }           // the hollow on top of a block (it goes down into the block)
QPainterPath tab(const QRectF& r, double left) { return hill(r.left() + left, r.bottom(), 1); }   // the hill under a block (it goes down out of the block)

// A component's outline: a body with a small corner radius and, on the left edge, the tab of its type. The same function draws the dent of an empty
// socket, so a tab fits its dent exactly and a tab of another shape visibly does not.
QPainterPath plugPath(const QRectF& body, PlugShape shape, double depth, double radius = 4) {
    QPainterPath p = roundedRect(body, radius);
    if (shape == PlugShape::None || depth <= 0) return p;
    const double cy = body.center().y(), l = body.left(), half = std::min(7.0, body.height() / 2 - 2);
    QPainterPath t;
    switch (shape) {
        case PlugShape::Round:
            t.addEllipse(QRectF(l - depth, cy - half, 2 * depth, 2 * half));
            break;
        case PlugShape::Point:
            t.moveTo(l, cy - half); t.lineTo(l - depth, cy); t.lineTo(l, cy + half); t.closeSubpath();
            break;
        case PlugShape::Flag:
            t.moveTo(l, cy - half); t.lineTo(l - depth, cy - half * 0.45); t.lineTo(l - depth, cy + half * 0.45); t.lineTo(l, cy + half); t.closeSubpath();
            break;
        case PlugShape::Diamond:
            t.moveTo(l, cy - half); t.lineTo(l - depth * 0.55, cy - half); t.lineTo(l - depth, cy); t.lineTo(l - depth * 0.55, cy + half); t.lineTo(l, cy + half); t.closeSubpath();
            break;
        case PlugShape::Square:
        case PlugShape::Any:
        default:
            t.addRect(QRectF(l - depth, cy - half, depth + 1, 2 * half));
            break;
    }
    return p | t;
}

void drawText(PaintContext& c, const QRectF& r, const QString& s, const QFont& f, const QColor& col, bool strike = false, bool italic = false) {
    QFont ff = f;
    ff.setStrikeOut(strike);
    ff.setItalic(italic);
    c.p.setFont(ff);
    c.p.setPen(col);
    c.p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, s);
}

// ---- fields
class ChipFieldPainter : public IFieldPainter {
public:
    void paint(PaintContext& c, const FieldBox& f, const BlockBox& b, const QColor&) override {
        const QRectF r = qr(f.rect);
        c.p.setPen(QPen(toQColor(c.theme.chipBorder), 1));
        c.p.setBrush(toQColor(c.theme.chip));
        c.p.drawRoundedRect(r, 4, 4);
        QColor tc = toQColor(c.theme.chipText);
        if (b.disabled) tc.setAlphaF(0.55f);
        if (f.empty) tc.setAlphaF(0.45f);
        drawText(c, r.adjusted(6, 0, -6, 0), QString::fromUtf8(f.text.c_str()), c.mono, tc, b.disabled, f.empty);
    }
};

class FlagFieldPainter : public IFieldPainter {
public:
    void paint(PaintContext& c, const FieldBox& f, const BlockBox& b, const QColor& text) override {
        const QRectF r = qr(f.rect);
        const double s = 12;
        const QRectF box(r.left() + 5, r.center().y() - s / 2, s, s);
        c.p.setPen(QPen(toQColor(c.theme.chipBorder), 1));
        c.p.setBrush(toQColor(c.theme.chip));
        c.p.drawRoundedRect(box, 2, 2);
        if (f.checked) {
            c.p.setPen(QPen(toQColor(c.theme.chipText), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            c.p.drawPolyline(QPolygonF({QPointF(box.left() + 2.5, box.center().y()), QPointF(box.left() + 5, box.bottom() - 3), QPointF(box.right() - 2.5, box.top() + 3)}));
        }
        QColor tc = text;
        if (!f.checked) tc.setAlphaF(0.6f);
        drawText(c, QRectF(box.right() + 5, r.top(), r.right() - box.right() - 5, r.height()), QString::fromUtf8(f.text.c_str()), c.ui, tc, b.disabled && f.checked);
    }
};

// the end-of-line comment of a block: one or several lines, as the layout fitted them
void paintComment(PaintContext& c, const BlockBox& b, const QColor& text) {
    if (b.commentLines.empty() || b.commentRect.empty()) return;
    QColor cc = text;
    cc.setAlphaF(0.7f);
    QString all;
    for (size_t i = 0; i < b.commentLines.size(); i++) all += (i ? "\n" : "") + QString::fromUtf8(b.commentLines[i].c_str());
    QFont f = c.ui;
    f.setItalic(true);
    c.p.setFont(f);
    c.p.setPen(cc);
    c.p.drawText(qr(b.commentRect), Qt::AlignLeft | Qt::AlignTop, all);
}

// While the pointer is over a block that has no comment, the empty zone at its end hints that a double click writes one there
void paintTailHint(PaintContext& c, const BlockBox& b, const BlockPaintState& st, const QColor& text) {
    if (!st.hovered || !b.commentLines.empty() || b.tailRect.empty()) return;
    QColor hc = text;
    hc.setAlphaF(0.45f);
    c.p.setFont(c.ui);
    c.p.setPen(hc);
    c.p.drawText(qr(b.tailRect), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("; \u2026"));
}

// ---- shapes
// the row shared by Statement, Container and Value: title, labelled fields, sockets and the comment
void paintRow(PaintContext& c, const BlockBox& b, const QColor& text) {
    if (!b.title.empty()) drawText(c, qr(b.titleRect), QString::fromUtf8(b.title.c_str()), c.ui, text, b.disabled);
    for (const FieldBox& f : b.fields) {
        if (!f.label.empty() && !f.labelRect.empty()) {
            QColor lc = text;
            lc.setAlphaF(0.8f);
            drawText(c, qr(f.labelRect), QString::fromUtf8(f.label.c_str()), c.ui, lc, b.disabled);
        }
        c.owner.paintField(c, f, b, text);
    }
    for (const SocketBox& s : b.sockets) {
        if (!s.label.empty() && !s.labelRect.empty()) drawText(c, qr(s.labelRect), QString::fromUtf8(s.label.c_str()), c.ui, text, b.disabled);
        if (s.child < 0 && !s.empty) {                           // nothing plugged in, but text typed: a chip in the shape of the dent
            c.p.setPen(QPen(toQColor(c.theme.chipBorder), 1));
            c.p.setBrush(toQColor(c.theme.chip));
            c.p.drawPath(plugPath(qr(s.rect), s.plug, s.plugDepth));
            QColor tc = toQColor(c.theme.chipText);
            if (b.disabled) tc.setAlphaF(0.55f);
            drawText(c, qr(s.rect).adjusted(6, 0, -6, 0), QString::fromUtf8(s.text.c_str()), c.mono, tc, b.disabled);
        } else if (s.child < 0) {                                // an empty socket: a black hole in the shape of what fits
            c.p.setPen(QPen(toQColor(c.theme.holeBorder), 1));
            c.p.setBrush(toQColor(c.theme.hole));
            const QPainterPath hole = plugPath(qr(s.rect), s.plug, s.plugDepth);
            c.p.drawPath(hole);
            c.p.setPen(QPen(QColor(0, 0, 0, 120), 2));               // a little depth: a shadow along the inside of the top and left edges
            c.p.setClipPath(hole);
            c.p.drawLine(qr(s.rect).topLeft() + QPointF(-s.plugDepth, 1), qr(s.rect).topRight() + QPointF(0, 1));
            c.p.setClipping(false);
        }
    }
    paintComment(c, b, text);
}

void finishBlock(PaintContext& c, const BlockBox& b, const QPainterPath& path, const BlockPaintState& st) {
    if (st.hatched) {
        c.p.save();
        c.p.setClipPath(path);
        QColor h = toQColor(c.theme.hatch);
        h.setAlphaF(0.55f);
        c.p.setPen(Qt::NoPen);
        c.p.setBrush(QBrush(h, Qt::BDiagPattern));
        c.p.drawPath(path);
        c.p.restore();
    }
    if (st.hovered && !st.selected) {
        c.p.setPen(Qt::NoPen);
        c.p.setBrush(toQColor(c.theme.hover));
        c.p.drawPath(path);
    }
    if (st.selected) {
        c.p.setBrush(Qt::NoBrush);
        c.p.setPen(QPen(toQColor(c.theme.selection), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        c.p.drawPath(path);
    }
    if (st.error || st.warning) {                                // the badge at the top right corner of the first row
        const double d = 14;
        const QRectF head = qr(b.head);
        const QRectF badge(head.right() - d / 2, head.top() - d / 2 + 2, d, d);
        c.p.setPen(QPen(Qt::white, 1.5));
        c.p.setBrush(toQColor(st.error ? c.theme.error : c.theme.warning));
        c.p.drawEllipse(badge);
        c.p.setPen(Qt::white);
        QFont f = c.ui;
        f.setBold(true);
        f.setPixelSize(10);
        c.p.setFont(f);
        c.p.drawText(badge, Qt::AlignCenter, "!");
    }
}

class StatementPainter : public IShapePainter {
public:
    void paint(PaintContext& c, const BlockBox& b, const BlockPaintState& st) override {
        const QPainterPath path = BlockPainter::outline(b);
        const QColor fill = c.owner.fillOf(b);
        c.p.setPen(QPen(fill.darker(135), 1));
        c.p.setBrush(fill);
        c.p.drawPath(path);
        const QColor rowText = toQColor(c.theme.textOn(Rgba{(uint8_t)fill.red(), (uint8_t)fill.green(), (uint8_t)fill.blue(), 255}));
        paintRow(c, b, rowText);
        paintTailHint(c, b, st, rowText);
        for (const SlotBox& sl : b.slotBoxes) {                              // an empty slot of a container is a black hole too: instructions can be dropped into it
            if (!sl.items.empty()) continue;
            c.p.setPen(QPen(toQColor(c.theme.holeBorder), 1));
            c.p.setBrush(toQColor(c.theme.hole));
            c.p.drawRoundedRect(qr(sl.rect).adjusted(1, 1, -1, -1), 3, 3);
        }
        finishBlock(c, b, path, st);
    }
};

class PillPainter : public IShapePainter {                       // Value and Tag
public:
    void paint(PaintContext& c, const BlockBox& b, const BlockPaintState& st) override {
        const QPainterPath path = BlockPainter::outline(b);
        const QColor fill = c.owner.fillOf(b);
        c.p.setPen(QPen(fill.darker(135), 1));
        c.p.setBrush(fill);
        c.p.drawPath(path);
        const QColor text = toQColor(c.theme.textOn(Rgba{(uint8_t)fill.red(), (uint8_t)fill.green(), (uint8_t)fill.blue(), 255}));
        if (b.shape == Shape::Tag) {
            for (const FieldBox& f : b.fields) drawText(c, qr(f.rect).adjusted(6, 0, 0, 0), QString::fromUtf8(f.text.c_str()), c.mono, text, b.disabled);
            paintComment(c, b, text);
            paintTailHint(c, b, st, text);
        } else {
            paintRow(c, b, text);
        }
        finishBlock(c, b, path, st);
    }
};

class NotePainter : public IShapePainter {                       // Note (comment) and Text (raw assembler)
public:
    void paint(PaintContext& c, const BlockBox& b, const BlockPaintState& st) override {
        const QPainterPath path = BlockPainter::outline(b);
        const bool note = b.shape == Shape::Note;
        QColor fill = note ? toQColor(c.theme.note) : c.owner.fillOf(b);
        if (b.disabled) fill = fill.lighter(110);
        c.p.setPen(QPen(fill.darker(125), 1, note ? Qt::SolidLine : Qt::DashLine));
        c.p.setBrush(fill);
        c.p.drawPath(path);
        const QColor text = note ? toQColor(c.theme.noteText) : toQColor(c.theme.textOn(Rgba{(uint8_t)fill.red(), (uint8_t)fill.green(), (uint8_t)fill.blue(), 255}));
        for (const FieldBox& f : b.fields) {
            QString t = QString::fromUtf8(f.text.c_str());
            if (note) t = "; " + t;
            drawText(c, qr(f.rect), t, note ? c.ui : c.mono, text, b.disabled, note);
        }
        if (!note) { paintComment(c, b, text); paintTailHint(c, b, st, text); }
        finishBlock(c, b, path, st);
    }
};

class SpacerPainter : public IShapePainter {
public:
    void paint(PaintContext& c, const BlockBox& b, const BlockPaintState& st) override {
        const QRectF r = qr(b.rect);
        QColor line = toQColor(c.theme.floatingOutline);
        line.setAlphaF(0.4f);
        c.p.setPen(QPen(line, 1, Qt::DotLine));
        c.p.drawLine(QPointF(r.left() + 4, r.center().y()), QPointF(r.right() - 4, r.center().y()));
        if (st.selected) {
            c.p.setBrush(Qt::NoBrush);
            c.p.setPen(QPen(toQColor(c.theme.selection), 2));
            c.p.drawRect(r);
        }
    }
};

}  // namespace

QPainterPath BlockPainter::outline(const BlockBox& b) {
    const QRectF r = qr(b.rect);
    switch (b.shape) {
        case Shape::Value: return plugPath(r, b.plug, b.plugDepth);
        case Shape::Spacer: return roundedRect(r, 3);
        case Shape::Tag:                                                // labels, comments and assembler text are stack blocks too: the same hollow and hill
        case Shape::Note:
        case Shape::Text: {
            QPainterPath p = roundedRect(r, kRadius - 1);
            p -= notch(r);
            p |= tab(r, kNotchX);
            return p;
        }
        case Shape::Container: {
            QPainterPath p;
            if (b.slotBoxes.empty()) { p = roundedRect(r, kRadius); }
            else {
                const QRectF head = qr(b.head);
                p = roundedRect(head, kRadius);
                const double barW = b.slotBoxes[0].rect.x - b.rect.x;
                p |= roundedRect(QRectF(r.left(), head.bottom() - kRadius, barW, r.bottom() - head.bottom() + kRadius), kRadius);
                p |= roundedRect(QRectF(r.left(), b.slotBoxes.back().rect.y + b.slotBoxes.back().rect.h, r.width(), r.bottom() - (b.slotBoxes.back().rect.y + b.slotBoxes.back().rect.h)), kRadius);
                // the inner corners of the slotBoxes stay square: they are where the stacks inside connect
            }
            p -= notch(r);
            p |= tab(r, kNotchX);
            return p;
        }
        case Shape::Statement:
        default: {
            QPainterPath p = roundedRect(r, kRadius);
            p -= notch(r);
            p |= tab(r, kNotchX);
            return p;
        }
    }
}

QColor BlockPainter::fillOf(const BlockBox& b) const {
    Rgba c = theme_.categoryColor(b.known ? b.category : "Text");
    if (b.disabled) c = mix(c, rgb(128, 128, 128), 0.75);
    return toQColor(c);
}

BlockPainter::BlockPainter(const BlockTheme& theme, const QFont& ui, const QFont& mono) : theme_(theme), ui_(ui), mono_(mono) {
    auto statement = [] { return std::make_unique<StatementPainter>(); };
    shapes_[Shape::Statement] = statement();
    shapes_[Shape::Container] = statement();
    shapes_[Shape::Value] = std::make_unique<PillPainter>();
    shapes_[Shape::Tag] = std::make_unique<PillPainter>();
    shapes_[Shape::Note] = std::make_unique<NotePainter>();
    shapes_[Shape::Text] = std::make_unique<NotePainter>();
    shapes_[Shape::Spacer] = std::make_unique<SpacerPainter>();
    fields_[FieldKind::Text] = std::make_unique<ChipFieldPainter>();
    fields_[FieldKind::Flag] = std::make_unique<FlagFieldPainter>();
}

void BlockPainter::paintField(PaintContext& c, const FieldBox& f, const BlockBox& b, const QColor& text) {
    auto it = fields_.find(f.kind);
    if (it == fields_.end()) it = fields_.find(FieldKind::Text);          // a kind without its own painter looks like text
    if (it != fields_.end()) it->second->paint(c, f, b, text);
}

void BlockPainter::paintBlock(QPainter& p, const BlockBox& b, const BlockPaintState& st) {
    PaintContext c{p, theme_, ui_, mono_, *this};
    auto it = shapes_.find(b.shape);
    if (it == shapes_.end()) it = shapes_.find(Shape::Statement);
    if (it != shapes_.end()) it->second->paint(c, b, st);
}

void BlockPainter::paintStack(QPainter& p, const StackLayout& s, const StackPaintInfo& info) {
    p.setRenderHint(QPainter::Antialiasing, true);
    const BlockTheme& th = theme_;
    if (info.floating) {                                                      // a dashed outline: this stack is not part of the program
        QRectF r = qr(s.bounds).adjusted(-6, -3, 6, 8);
        QColor o = toQColor(th.floatingOutline);
        p.setPen(QPen(o, 1.2, Qt::DashLine));
        QColor bg = o;
        bg.setAlphaF(0.07f);
        p.setBrush(bg);
        p.drawRoundedRect(r, 8, 8);
    }
    if (s.header.kind == HeaderBox::Hat) {
        const QRectF r = qr(s.header.rect);
        QPainterPath path;
        path.addRoundedRect(r, 8, 8);
        path |= tab(r, kNotchX);
        p.setPen(QPen(toQColor(th.hatFill).darker(130), 1));
        p.setBrush(toQColor(th.hatFill));
        p.drawPath(path);
        QFont f = ui_;
        f.setBold(true);
        p.setFont(f);
        p.setPen(toQColor(th.hatText));
        p.drawText(QRectF(r.left() + 14, r.top(), p.fontMetrics().horizontalAdvance(QString::fromUtf8(s.header.text.c_str())) + 4, r.height()), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(s.header.text.c_str()));
        if (!s.header.commentLines.empty()) {
            QString all;
            for (size_t i = 0; i < s.header.commentLines.size(); i++) all += (i ? "\n" : "") + QString::fromUtf8(s.header.commentLines[i].c_str());
            QFont cf = ui_;
            cf.setItalic(true);
            p.setFont(cf);
            QColor cc = toQColor(th.hatText);
            cc.setAlphaF(0.7f);
            p.setPen(cc);
            p.drawText(qr(s.header.commentRect), Qt::AlignLeft | Qt::AlignTop, all);
        }
    } else if (s.header.kind == HeaderBox::Tag) {
        QFont f = ui_;
        f.setItalic(true);
        p.setFont(f);
        p.setPen(toQColor(th.floatingOutline));
        p.drawText(qr(s.header.rect).adjusted(2, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(s.header.text.c_str()));
    }
    for (const RegionBox& r : s.regions) {                                   // a frame around a cascade, behind its blocks
        const QRectF fr = qr(r.rect).adjusted(-5, -3, 5, 3);
        QColor line = toQColor(th.selection);
        line.setAlphaF(0.8f);
        p.setPen(QPen(line, 1.4, Qt::DashDotLine));
        QColor fill = line;
        fill.setAlphaF(0.07f);
        p.setBrush(fill);
        p.drawRoundedRect(fr, 7, 7);
        QFont f = ui_;
        f.setItalic(true);
        f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1));
        p.setFont(f);
        p.setPen(line);
        p.drawText(QPointF(fr.left() + 6, fr.top() - 3), QString::fromUtf8(r.label.c_str()));
    }
    for (const BlockBox& b : s.blocks) {
        // an effect on this block or on a block it stands in applies to it (the whole subtree wobbles together)
        double sx = 1, sy = 1;
        int anchor = 0;
        const BlockBox* owner = nullptr;
        if (info.fx)
            for (const BlockBox* c = &b;; c = &s.blocks[(size_t)c->parent]) {
                if (info.fx(c->id, sx, sy, anchor)) { owner = c; break; }
                if (c->parent < 0) break;
            }
        if (!owner) { paintBlock(p, b, info.stateOf(b.id)); continue; }
        const QPointF a = anchor == 1 ? QPointF(owner->rect.x, owner->rect.y + owner->rect.h / 2) : QPointF(owner->rect.x + owner->rect.w / 2, owner->rect.y + owner->rect.h / 2);
        p.save();
        p.translate(a);
        p.scale(sx, sy);
        p.translate(-a);
        paintBlock(p, b, info.stateOf(b.id));
        p.restore();
    }
}

}  // namespace acide
