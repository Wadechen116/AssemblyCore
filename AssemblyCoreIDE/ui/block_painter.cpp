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

constexpr double kRadius = 5, kNotchX = 14, kNotchW = 14, kNotchH = 4;

QPainterPath roundedRect(const QRectF& r, double rad) {
    QPainterPath p;
    p.addRoundedRect(r, rad, rad);
    return p;
}
QPainterPath notch(const QRectF& r) {            // the dent on top of a block that the tab of the block above fills
    QPainterPath n;
    n.moveTo(r.left() + kNotchX, r.top());
    n.lineTo(r.left() + kNotchX + 3, r.top() + kNotchH);
    n.lineTo(r.left() + kNotchX + kNotchW - 3, r.top() + kNotchH);
    n.lineTo(r.left() + kNotchX + kNotchW, r.top());
    n.closeSubpath();
    return n;
}
QPainterPath tab(const QRectF& r, double left) {  // the tab under a block
    QPainterPath n;
    n.moveTo(r.left() + left, r.bottom());
    n.lineTo(r.left() + left + 3, r.bottom() + kNotchH);
    n.lineTo(r.left() + left + kNotchW - 3, r.bottom() + kNotchH);
    n.lineTo(r.left() + left + kNotchW, r.bottom());
    n.closeSubpath();
    return n;
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
        if (s.child < 0 && !s.empty) {                           // nothing plugged in, but text typed: a chip (the same look as a field)
            c.p.setPen(QPen(toQColor(c.theme.chipBorder), 1));
            c.p.setBrush(toQColor(c.theme.chip));
            c.p.drawRoundedRect(qr(s.rect), 4, 4);
            QColor tc = toQColor(c.theme.chipText);
            if (b.disabled) tc.setAlphaF(0.55f);
            drawText(c, qr(s.rect).adjusted(6, 0, -6, 0), QString::fromUtf8(s.text.c_str()), c.mono, tc, b.disabled);
        } else if (s.child < 0) {                                // an empty socket: a dent
            c.p.setPen(QPen(toQColor(c.theme.chipBorder), 1, Qt::DashLine));
            QColor dent = toQColor(c.theme.chip);
            dent.setAlphaF(0.35f);
            c.p.setBrush(dent);
            c.p.drawRoundedRect(qr(s.rect), qr(s.rect).height() / 2, qr(s.rect).height() / 2);
        }
    }
    if (!b.comment.empty() && !b.commentRect.empty()) {
        QColor cc = text;
        cc.setAlphaF(0.65f);
        drawText(c, qr(b.commentRect), QString::fromUtf8(("; " + b.comment).c_str()), c.ui, cc, false, true);
    }
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
        paintRow(c, b, toQColor(c.theme.textOn(Rgba{(uint8_t)fill.red(), (uint8_t)fill.green(), (uint8_t)fill.blue(), 255})));
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
            if (!b.comment.empty() && !b.commentRect.empty()) paintRow(c, b, text);
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
        case Shape::Value:
        case Shape::Tag: return roundedRect(r, std::min(r.height() / 2, 14.0));
        case Shape::Note:
        case Shape::Text:
        case Shape::Spacer: return roundedRect(r, 3);
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
        p.drawText(r.adjusted(14, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(s.header.text.c_str()));
    } else if (s.header.kind == HeaderBox::Tag) {
        QFont f = ui_;
        f.setItalic(true);
        p.setFont(f);
        p.setPen(toQColor(th.floatingOutline));
        p.drawText(qr(s.header.rect).adjusted(2, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(s.header.text.c_str()));
    }
    for (const BlockBox& b : s.blocks) paintBlock(p, b, info.stateOf(b.id));
}

}  // namespace acide
