#include "block_scene.h"

#include <QFontMetricsF>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsRectItem>
#include <QGraphicsSceneMouseEvent>
#include <QToolTip>
#include <QPainter>

namespace acide {

using namespace blocks;

static QString trw(const char* ctx, const std::string& s) { return QCoreApplication::translate(ctx, s.c_str()); }
static std::string stdTr(const char* ctx, const std::string& s) { return trw(ctx, s).toStdString(); }

double QtTextMetrics::width(const std::string& s, bool mono) const { return QFontMetricsF(mono ? mono_ : ui_).horizontalAdvance(QString::fromUtf8(s.c_str())); }
double QtTextMetrics::lineHeight(bool mono) const { return QFontMetricsF(mono ? mono_ : ui_).height(); }

std::string QtTextProvider::blockTitle(const asmcore::blocks::BlockDef& d) const { return stdTr("BlockTitle", d.title); }
std::string QtTextProvider::fieldLabel(const asmcore::blocks::BlockDef&, const asmcore::blocks::FieldDef& f) const { return f.label.empty() ? f.label : stdTr("BlockField", f.label); }
std::string QtTextProvider::inputLabel(const asmcore::blocks::BlockDef&, const asmcore::blocks::InputDef& i) const { return i.label.empty() ? i.label : stdTr("BlockField", i.label); }
std::string QtTextProvider::hatTitle(const std::string& domain) const {
    if (domain == "main") return QCoreApplication::translate("BlockCanvas", "Main").toStdString();
    if (domain == "peri") return QCoreApplication::translate("BlockCanvas", "Peri").toStdString();
    return QCoreApplication::translate("BlockCanvas", "Definitions").toStdString();
}
std::string QtTextProvider::floatingTag() const { return QCoreApplication::translate("BlockCanvas", "not part of the program").toStdString(); }
std::string QtTextProvider::collapsedSummary(size_t n) const { return QCoreApplication::translate("BlockCanvas", "%n block(s)", nullptr, (int)n).toStdString(); }
std::string QtTextProvider::emptyValue() const { return "..."; }

// ---- StackItem
StackItem::StackItem(BlockScene& scene, size_t index) : scene_(scene), index_(index) {
    setAcceptHoverEvents(true);
    relayout();
}

void StackItem::relayout() {
    prepareGeometryChange();
    const StackLayout& s = scene_.layout_.stacks[index_];
    setPos(s.origin.x, s.origin.y);
    setZValue(s.domain == "floating" ? 1 : 0);
    update();
}

QRectF StackItem::boundingRect() const {
    const Rect& b = scene_.layout_.stacks[index_].bounds;
    return QRectF(b.x - 12, b.y - 12, b.w + 24, b.h + 28);               // room for the badges, the selection outline and the tab
}

void StackItem::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) {
    const StackLayout& s = scene_.layout_.stacks[index_];
    StackPaintInfo info;
    info.selected = &scene_.session_.selection();
    info.hovered = scene_.hovered_;
    info.hatched = &scene_.hatched_;
    info.errors = &scene_.errorIds_;
    info.warnings = &scene_.warningIds_;
    info.floating = s.domain == "floating";
    scene_.painter_.paintStack(*p, s, info);
}

const BlockBox* StackItem::boxAt(const QPointF& pos) const { return scene_.layout_.stacks[index_].blockAt({pos.x(), pos.y()}); }

void StackItem::hoverMoveEvent(QGraphicsSceneHoverEvent* e) {
    const BlockBox* b = boxAt(e->pos());
    scene_.setHovered(b ? b->id : std::string());
    if (!b) { setToolTip(QString()); return; }
    QString tip;
    auto it = scene_.problems_.find(b->id);
    if (it != scene_.problems_.end()) tip += QString::fromUtf8(it->second.c_str()) + "\n\n";
    tip += QString::fromUtf8(b->title.empty() ? b->type.c_str() : b->title.c_str());
    for (const FieldBox& f : b->fields)
        if (f.elided || f.kind != asmcore::blocks::FieldKind::Flag) tip += "\n" + QString::fromUtf8(f.label.c_str()) + (f.label.empty() ? "" : ": ") + QString::fromUtf8(f.fullText.c_str());
    if (!b->comment.empty()) tip += "\n; " + QString::fromUtf8(b->comment.c_str());
    setToolTip(tip.trimmed());
}

void StackItem::hoverLeaveEvent(QGraphicsSceneHoverEvent*) { scene_.setHovered(std::string()); }


// ---- BlockScene
BlockScene::BlockScene(BlockSession& session, const BlockTheme& theme, const QFont& ui, const QFont& mono, QObject* parent)
    : QGraphicsScene(parent), session_(session), metrics_(ui, mono), painter_(theme, ui, mono) {
    conns_.add(session_.changed, [this] { rebuild(); });
    conns_.add(session_.selectionChanged, [this] { update(); });
    rebuild();
}

BlockScene::~BlockScene() = default;

void BlockScene::setTheme(const BlockTheme& t) {
    painter_.setTheme(t);
    update();
}

void BlockScene::setLayoutStyle(const LayoutStyle& s) {
    style_ = s;
    rebuild();
}

void BlockScene::setHatched(const std::set<std::string>& ids) {
    hatched_ = ids;
    update();
}

void BlockScene::setHovered(const std::string& id) {
    if (hovered_ == id) return;
    hovered_ = id;
    update();
}

void BlockScene::updateProblems() {
    problems_.clear();
    errorIds_.clear();
    warningIds_.clear();
    for (const asmcore::blocks::BlockProblem& p : session_.problems()) {
        if (p.blockId.empty()) continue;
        (p.error ? errorIds_ : warningIds_).insert(p.blockId);
        std::string& m = problems_[p.blockId];
        if (!m.empty()) m += "\n";
        m += p.message;
    }
}

void BlockScene::rebuild() {
    if (rebuilding_) return;
    rebuilding_ = true;
    hovered_.clear();
    band_ = nullptr;                                              // deleted with the other items below
    marker_ = nullptr;
    if (mode_ == Mode::Band || mode_ == Mode::PressedBlock) mode_ = Mode::Idle;      // (a drag of a lifted stack goes on across the rebuild its own lift causes)
    const auto items = this->items();
    for (QGraphicsItem* it : items) { removeItem(it); delete it; }
    LayoutEngine eng(metrics_, style_, session_.registry(), ShapeRegistry::defaults(), &text_);
    layout_ = eng.layoutDocument(session_.document());
    updateProblems();
    for (size_t i = 0; i < layout_.stacks.size(); i++) addItem(new StackItem(*this, i));
    const Rect& b = layout_.bounds;
    const double m = 2000;                                        // a wide margin: the canvas is unbounded in practice
    setSceneRect(QRectF(b.x - m, b.y - m, b.w + 2 * m, b.h + 2 * m));
    rebuilding_ = false;
    rebuilt.fire();
}

std::string BlockScene::blockAt(const QPointF& p) const {
    for (size_t i = layout_.stacks.size(); i-- > 0;) {                      // the stacks later in the list are drawn on top
        const StackLayout& s = layout_.stacks[i];
        if (const BlockBox* b = s.blockAt({p.x() - s.origin.x, p.y() - s.origin.y})) return b->id;
    }
    return std::string();
}

std::vector<std::string> BlockScene::blocksIn(const QRectF& r) const {
    std::vector<std::string> out;
    for (const StackLayout& s : layout_.stacks) {
        const Rect local{r.x() - s.origin.x, r.y() - s.origin.y, r.width(), r.height()};
        std::vector<char> in(s.blocks.size(), 0);
        for (size_t i = 0; i < s.blocks.size(); i++) {
            const BlockBox& b = s.blocks[i];
            if (b.shape == Shape::Spacer) continue;
            const bool hit = b.rect.intersects(local);
            const bool parentIn = b.parent >= 0 && in[(size_t)b.parent];
            if (hit && !parentIn) { in[i] = 1; out.push_back(b.id); }
            else if (parentIn) in[i] = 1;                                    // covered by its parent
        }
    }
    return out;
}

void BlockScene::selectAll() {
    std::vector<std::string> ids;
    for (const StackLayout& s : layout_.stacks) for (int i : s.top) ids.push_back(s.blocks[(size_t)i].id);
    session_.select(ids);
}

std::set<size_t> BlockScene::selectedStacks() const {
    std::set<size_t> out;
    for (const std::string& id : session_.selection()) {
        const StackLayout* s = layout_.stackOf(id);
        if (s) out.insert(s->section);
    }
    return out;
}

// The part of the canvas a stack may be moved in: the area around the stacks that the scene reserves (the sceneRect), not beyond.
QRectF BlockScene::canvasArea() const { return sceneRect(); }

void BlockScene::moveSelectedStacksBy(const QPointF& delta, bool commit) {
    if (dragOrigin_.empty()) for (size_t i : (movingStacks_.empty() ? selectedStacks() : movingStacks_)) dragOrigin_[i] = QPointF(layout_.stacks[i].origin.x, layout_.stacks[i].origin.y);
    const QRectF area = canvasArea();
    // one common offset for all the stacks, limited so that none of them leaves the area
    QPointF d = delta;
    for (auto& kv : dragOrigin_) {
        const Rect& b = layout_.stacks[kv.first].bounds;
        const QRectF r(kv.second.x() + b.x, kv.second.y() + b.y, b.w, b.h);
        d.setX(std::max(area.left() - r.left(), std::min(area.right() - r.right(), d.x())));
        d.setY(std::max(area.top() - r.top(), std::min(area.bottom() - r.bottom(), d.y())));
    }
    if (!commit) {                                                           // only show it: the items move, the document does not change yet
        for (QGraphicsItem* it : items())
            if (auto* si = dynamic_cast<StackItem*>(it)) {
                auto o = dragOrigin_.find(si->index());
                if (o != dragOrigin_.end()) si->setPos(o->second + d);
            }
        return;
    }
    const auto origin = dragOrigin_;
    dragOrigin_.clear();
    if (d.isNull()) { for (QGraphicsItem* it : items()) if (auto* si = dynamic_cast<StackItem*>(it)) { auto o = origin.find(si->index()); if (o != origin.end()) si->setPos(o->second); } return; }
    for (auto& kv : origin) {
        const QPointF to = kv.second + d;
        session_.apply([&](asmcore::blocks::Editor& ed) { return ed.moveSection(kv.first, to.x(), to.y()); });
    }
}

void BlockScene::mousePressEvent(QGraphicsSceneMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QGraphicsScene::mousePressEvent(e); return; }
    pressPos_ = e->scenePos();
    pressMods_ = e->modifiers();
    pressedId_ = blockAt(pressPos_);
    dragOrigin_.clear();
    movingStacks_.clear();
    // the hat of a section / the tag strip of a floating stack: it moves the whole stack
    if (pressedId_.empty()) {
        for (size_t i = layout_.stacks.size(); i-- > 0;) {
            const blocks::StackLayout& s = layout_.stacks[i];
            const blocks::Rect& hr = s.header.rect;
            if (s.header.kind != blocks::HeaderBox::None && hr.contains({pressPos_.x() - s.origin.x, pressPos_.y() - s.origin.y})) {
                mode_ = Mode::PressedHeader;
                headerStack_ = i;
                e->accept();
                return;
            }
        }
    }
    if (pressedId_.empty()) {
        mode_ = Mode::Band;
        bandBase_ = (pressMods_ & Qt::ControlModifier) ? session_.selection() : std::set<std::string>();
        if (!(pressMods_ & Qt::ControlModifier)) emptyClicked.fire(pressMods_);
        band_ = addRect(QRectF(pressPos_, QSizeF(0, 0)));
        QColor c = toQColor(theme().selection);
        band_->setPen(QPen(c, 1, Qt::DashLine));
        c.setAlpha(40);
        band_->setBrush(c);
        band_->setZValue(1000);
    } else {
        mode_ = Mode::PressedBlock;
        pressedWasSelected_ = session_.isSelected(pressedId_);
        if (!pressedWasSelected_ || (pressMods_ & Qt::ControlModifier)) blockClicked.fire(pressedId_, pressMods_);
    }
    e->accept();
}

// ---- the marker that shows where a drop would go
namespace {
class DropMarkerItem : public QGraphicsItem {
public:
    DropMarkerItem(const blocks::DropTarget& t, double width, const blocks::BlockTheme& th) : t_(t), width_(width), th_(th) { setZValue(900); }
    QRectF boundingRect() const override {
        if (t_.kind == blocks::DropTarget::Socket) return QRectF(t_.socketRect.x - 16, t_.socketRect.y - 8, t_.socketRect.w + 32, t_.socketRect.h + 16);
        return QRectF(t_.marker.x - 12, t_.marker.y - 12, width_ + 24, 24);
    }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
        p->setRenderHint(QPainter::Antialiasing, true);
        const bool no = t_.fit == asmcore::blocks::Fit::No, warn = t_.fit == asmcore::blocks::Fit::Warn;
        const QColor col = no ? toQColor(th_.error) : warn ? toQColor(th_.warning) : QColor(46, 204, 113);
        if (t_.kind == blocks::DropTarget::Socket) {
            const QRectF r(t_.socketRect.x - 8, t_.socketRect.y - 3, t_.socketRect.w + 11, t_.socketRect.h + 6);
            QPainterPath path;
            path.addRoundedRect(r, 5, 5);
            if (no) {
                p->save();
                p->setClipPath(path);
                QColor h = toQColor(th_.hatch);
                h.setAlphaF(0.75f);
                p->setPen(Qt::NoPen);
                p->setBrush(QBrush(h, Qt::BDiagPattern));
                p->drawPath(path);
                p->restore();
            }
            p->setPen(QPen(col, 2.5));
            p->setBrush(Qt::NoBrush);
            p->drawPath(path);
        } else {
            const QRectF bar(t_.marker.x, t_.marker.y - 2, width_, 4);
            if (no) {
                QColor h = toQColor(th_.hatch);
                p->setPen(QPen(col, 1));
                p->setBrush(QBrush(h, Qt::BDiagPattern));
            } else {
                p->setPen(Qt::NoPen);
                p->setBrush(col);
            }
            p->drawRoundedRect(bar, 2, 2);
            p->setPen(Qt::NoPen);
            p->setBrush(col);
            p->drawEllipse(QPointF(t_.marker.x, t_.marker.y), 4.5, 4.5);
        }
    }

private:
    blocks::DropTarget t_;
    double width_;
    blocks::BlockTheme th_;
};
}  // namespace

void BlockScene::showMarker() {
    if (marker_) { removeItem(marker_); delete marker_; marker_ = nullptr; }
    if (dropTarget_.kind == blocks::DropTarget::None) return;
    const double w = payload_ < layout_.stacks.size() ? layout_.stacks[payload_].bounds.w : 100;
    marker_ = new DropMarkerItem(dropTarget_, w, theme());
    addItem(marker_);
}

// where the stack being dragged is drawn: the pointer holds it where it grabbed it
QPointF BlockScene::payloadOrigin(const QPointF& scenePos) const {
    const StackLayout& ps = layout_.stacks[payload_];
    const BlockBox& first = ps.blocks[(size_t)ps.top[0]];
    QPointF o = scenePos - grab_ - QPointF(first.rect.x, first.rect.y);
    const QRectF area = canvasArea();
    o.setX(std::max(area.left(), std::min(area.right() - ps.bounds.w, o.x())));
    o.setY(std::max(area.top(), std::min(area.bottom() - ps.bounds.h, o.y())));
    return o;
}

// The pointer has dragged a block away from where it stands: lift it out (a component out of its socket, instructions out of their stack; with
// `single`, only that instruction) so it floats, and go on dragging that. Returns false if there is nothing to lift.
bool BlockScene::beginPayloadDrag(const QPointF& scenePos, bool single) {
    const blocks::BlockBox* box = nullptr;
    const blocks::StackLayout* st = layout_.stackOf(pressedId_, &box);
    if (!st || !box) return false;
    const QPointF at(st->origin.x + box->rect.x, st->origin.y + box->rect.y);
    const QPointF origin(at.x(), at.y() - style_.tagHeight);                       // a floating stack has a tag strip above its first block
    const size_t index = (size_t)(box - &st->blocks[0]);
    session_.beginGroup("drag:" + std::to_string(++dragSerial_));       // one undo step per drag (a different key for each)
    asmcore::blocks::Outcome o;
    bool nested = false;
    if (box->parent >= 0) {                                                           // a component in a socket of its parent
        const blocks::BlockBox& parent = st->blocks[(size_t)box->parent];
        for (const blocks::SocketBox& sb : parent.sockets) if (sb.child == (int)index) nested = true;
        if (nested) o = session_.apply([&](asmcore::blocks::Editor& ed) { return ed.unplugValue(parent.id, box->parentSlot, origin.x(), origin.y()); });
    }
    if (!nested) o = session_.apply([&](asmcore::blocks::Editor& ed) { return ed.liftBlocks(pressedId_, !single, origin.x(), origin.y()); });
    if (!o.ok) { session_.endGroup(); return false; }
    payload_ = o.section;
    if (payload_ >= layout_.stacks.size() || layout_.stacks[payload_].top.empty()) { session_.endGroup(); return false; }
    lifted_ = true;
    (void)scenePos;
    grab_ = pressPos_ - at;                                                         // where the block was held when the pointer went down
    return true;
}

void BlockScene::updatePayloadPreview(const QPointF& scenePos, const QPoint& screenPos) {
    if (payload_ >= layout_.stacks.size()) return;
    const QPointF o = payloadOrigin(scenePos);
    for (QGraphicsItem* it : items())
        if (auto* si = dynamic_cast<StackItem*>(it))
            if (si->index() == payload_) { si->setPos(o); si->setZValue(500); }
    blocks::DropOptions opt;
    dropTarget_ = blocks::findDropTarget(session_, layout_, payload_, {o.x(), o.y()}, opt);
    showMarker();
    if (dropTarget_.kind != blocks::DropTarget::None && !dropTarget_.reason.empty()) QToolTip::showText(screenPos, QString::fromUtf8(dropTarget_.reason.c_str()));
    else QToolTip::hideText();
}

void BlockScene::endPayloadDrag(bool drop) {
    if (marker_) { removeItem(marker_); marker_ = nullptr; }                      // (deleted with the rest below)
    QToolTip::hideText();
    const blocks::DropTarget target = dropTarget_;
    dropTarget_ = blocks::DropTarget();
    const size_t payload = payload_;
    QPointF where;
    for (QGraphicsItem* it : items()) if (auto* si = dynamic_cast<StackItem*>(it)) if (si->index() == payload) where = si->pos();
    if (drop) {
        if (target.fits()) blocks::performDrop(session_, payload, target);
        else session_.apply([&](asmcore::blocks::Editor& ed) { return ed.moveSection(payload, where.x(), where.y()); });          // stays where it was put, floating
    }
    session_.endGroup();
    lifted_ = false;
    mode_ = Mode::Idle;
    rebuild();
}

void BlockScene::cancelDrag() {
    if (mode_ == Mode::DragPayload) {
        dropTarget_ = blocks::DropTarget();
        QToolTip::hideText();
        session_.endGroup();
        mode_ = Mode::Idle;
        if (lifted_) { lifted_ = false; session_.undo(); }                          // the lift is the only edit so far: undoing it puts everything back
        else rebuild();
    } else if (mode_ == Mode::MoveStacks) {
        dragOrigin_.clear();
        movingStacks_.clear();
        mode_ = Mode::Idle;
        rebuild();
    }
}

void BlockScene::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && (mode_ == Mode::DragPayload || mode_ == Mode::MoveStacks)) { cancelDrag(); e->accept(); return; }
    QGraphicsScene::keyPressEvent(e);
}

void BlockScene::mouseMoveEvent(QGraphicsSceneMouseEvent* e) {
    if (mode_ == Mode::PressedBlock && (e->scenePos() - pressPos_).manhattanLength() > 4 && session_.isSelected(pressedId_)) {
        if (selectedStacks().size() > 1) { movingStacks_ = selectedStacks(); mode_ = Mode::MoveStacks; }          // several stacks selected: they move together
        else if (beginPayloadDrag(e->scenePos(), (pressMods_ | e->modifiers()) & Qt::AltModifier)) mode_ = Mode::DragPayload;
        else { movingStacks_ = selectedStacks(); mode_ = Mode::MoveStacks; }
        hovered_.clear();
    }
    if (mode_ == Mode::PressedHeader && (e->scenePos() - pressPos_).manhattanLength() > 4) {
        movingStacks_ = {headerStack_};
        mode_ = Mode::MoveStacks;
        hovered_.clear();
    }
    if (mode_ == Mode::DragPayload) {
        updatePayloadPreview(e->scenePos(), e->screenPos());
        e->accept();
        return;
    }
    if (mode_ == Mode::MoveStacks) {
        moveSelectedStacksBy(e->scenePos() - pressPos_, false);
        e->accept();
        return;
    }
    if (mode_ == Mode::Band && band_) {
        const QRectF r = QRectF(pressPos_, e->scenePos()).normalized();
        band_->setRect(r);
        std::vector<std::string> ids = blocksIn(r);
        std::set<std::string> next = bandBase_;
        next.insert(ids.begin(), ids.end());
        session_.select(std::vector<std::string>(next.begin(), next.end()));
        e->accept();
        return;
    }
    QGraphicsScene::mouseMoveEvent(e);
}

void BlockScene::mouseReleaseEvent(QGraphicsSceneMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QGraphicsScene::mouseReleaseEvent(e); return; }
    if (mode_ == Mode::DragPayload) { endPayloadDrag(true); e->accept(); return; }
    if (mode_ == Mode::MoveStacks) { moveSelectedStacksBy(e->scenePos() - pressPos_, true); movingStacks_.clear(); }
    else if (mode_ == Mode::PressedHeader && headerStack_ < layout_.stacks.size()) {                // a click on a hat: select the blocks of that stack
        std::vector<std::string> ids;
        const blocks::StackLayout& s = layout_.stacks[headerStack_];
        for (int i : s.top) ids.push_back(s.blocks[(size_t)i].id);
        session_.select(ids);
    }
    else if (mode_ == Mode::PressedBlock && pressedWasSelected_ && !(pressMods_ & Qt::ControlModifier)) blockClicked.fire(pressedId_, pressMods_);   // a click, not a drag: select just this one
    if (band_) { removeItem(band_); delete band_; band_ = nullptr; }
    mode_ = Mode::Idle;
    e->accept();
}

QRectF BlockScene::blockRect(const std::string& id) const {
    const BlockBox* box = nullptr;
    const StackLayout* s = layout_.stackOf(id, &box);
    if (!s || !box) return QRectF();
    return QRectF(s->origin.x + box->rect.x, s->origin.y + box->rect.y, box->rect.w, box->rect.h);
}

}  // namespace acide
