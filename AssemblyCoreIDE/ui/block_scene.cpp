#include "block_scene.h"

#include <QFontMetricsF>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsRectItem>
#include <QGraphicsView>
#include <QGraphicsSceneMouseEvent>
#include <algorithm>

#include <QDateTime>
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
    return QRectF(b.x - 40, b.y - 40, b.w + 80, b.h + 84);               // room for the badges, the selection outline, the tab and a block that swells while it wobbles
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
    info.fx = [this](const std::string& id, double& sx, double& sy, int& anchor) {
        auto it = scene_.fx_.find(id);
        if (it == scene_.fx_.end()) return false;
        sx = it->second.sx.x; sy = it->second.sy.x; anchor = it->second.anchor;
        return true;
    };
    scene_.painter_.paintStack(*p, s, info);
}

const BlockBox* StackItem::boxAt(const QPointF& pos) const { return scene_.layout_.stacks[index_].blockAt({pos.x(), pos.y()}); }

void StackItem::hoverMoveEvent(QGraphicsSceneHoverEvent* e) {
    const BlockBox* b = boxAt(e->pos());
    scene_.setHovered(b ? b->id : std::string());
    {
        const StackLayout& s = scene_.layout_.stacks[index_];
        if (!b && s.header.kind == HeaderBox::Hat && s.header.rect.contains({e->pos().x(), e->pos().y()})) {
            setToolTip(s.header.order > 0
                           ? QCoreApplication::translate("BlockCanvas", "Part %1 of the program: the parts are written top to bottom (and left to right at the same height)\nDrag it to change the order; double-click the end of it to write a comment").arg(s.header.order)
                           : QCoreApplication::translate("BlockCanvas", "A part of the program"));
            return;
        }
    }
    if (!b) { setToolTip(QString()); return; }
    QString tip;
    if (b->shape != Shape::Value && b->shape != Shape::Note && b->shape != Shape::Spacer && b->tailRect.contains({e->pos().x(), e->pos().y()}))
        tip += QCoreApplication::translate("BlockCanvas", "Double-click to write or edit the comment at the end of this block") + "\n\n";
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
    fxTimer_.setInterval(16);
    connect(&fxTimer_, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const double dt = fxLast_ ? std::min(0.05, (now - fxLast_) / 1000.0) : 0.016;
        fxLast_ = now;
        advanceEffects(dt);
    });
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
    {
        EditTarget ft;                                                             // a click on a flag chip switches it
        if (editTargetAt(pressPos_, ft) && ft.flag) {
            const std::string now = ft.text == "1" ? std::string() : std::string("1");
            session_.apply([&](asmcore::blocks::Editor& ed) { return ed.setField(ft.blockId, ft.part, now); });
            popEffect(ft.blockId);
            session_.select({ft.blockId});
            mode_ = Mode::Idle;
            e->accept();
            return;
        }
    }
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

// ---- soft effects --------------------------------------------------------------------------------------------------------------------
void BlockScene::setAnimationsEnabled(bool on) {
    animate_ = on;
    if (!on) { fx_.clear(); tiltActive_ = false; fxTimer_.stop(); update(); }
}

void BlockScene::effectScale(const std::string& id, double& sx, double& sy) const {
    sx = sy = 1;
    auto it = fx_.find(id);
    if (it != fx_.end()) { sx = it->second.sx.x; sy = it->second.sy.x; }
}

void BlockScene::startFxTimer() {
    if (!fxTimer_.isActive()) { fxLast_ = 0; fxTimer_.start(); }
}

// A block pops: it starts squashed and swells past its size, then wobbles to rest -- like something soft being pressed in or let go.
void BlockScene::popEffect(const std::string& id) {
    if (!animate_ || id.empty()) return;
    Fx f;
    f.sx.kick(0.82);
    f.sy.kick(1.14);                                                     // squash and stretch: narrower while taller; the volume stays about the same
    f.sx.stiffness = f.sy.stiffness = 420;
    f.sx.damping = f.sy.damping = 12;
    f.anchor = 0;
    fx_[id] = f;
    startFxTimer();
    update();
}

// A row that was wider (a block was taken out of it) or narrower (a block was put into its hole) than it is now springs to its size with a wobble.
void BlockScene::stretchEffect(const std::string& id, double from) {
    if (!animate_ || id.empty() || std::fabs(from - 1) < 0.01) return;
    Fx f;
    f.sx.kick(from);
    f.sy.kick(1 + (1 - from) * 0.35);                                    // wider -> a little flatter, narrower -> a little taller
    f.sx.stiffness = f.sy.stiffness = 340;
    f.sx.damping = f.sy.damping = 10;
    f.anchor = 1;
    fx_[id] = f;
    startFxTimer();
    update();
}

void BlockScene::advanceEffects(double dt) {
    for (auto it = fx_.begin(); it != fx_.end();) {
        it->second.sx.step(dt);
        it->second.sy.step(dt);
        if (it->second.sx.settled() && it->second.sy.settled()) it = fx_.erase(it);
        else ++it;
    }
    if (tiltActive_) {
        if (mode_ == Mode::DragPayload) tilt_.target *= std::pow(0.03, dt);        // the lean relaxes while the pointer rests
        tilt_.step(dt);
        applyTilt();
        if (mode_ != Mode::DragPayload && tilt_.settled(0.05)) tiltActive_ = false;
    }
    if (fx_.empty() && !tiltActive_) fxTimer_.stop();
    update();
}

// the stack being dragged leans with the pointer's speed (around the point it is held by) and swings back when the pointer stops
void BlockScene::applyTilt() {
    if (payload_ >= layout_.stacks.size()) return;
    for (QGraphicsItem* it : items())
        if (auto* si = dynamic_cast<StackItem*>(it))
            if (si->index() == payload_) {
                const blocks::BlockBox& first = layout_.stacks[payload_].blocks[(size_t)layout_.stacks[payload_].top[0]];
                si->setTransformOriginPoint(grab_ + QPointF(first.rect.x, first.rect.y));
                si->setRotation(tilt_.x);
            }
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
    std::string hostId;
    double hostWidthBefore = 0;
    if (box->parent >= 0) {                                                           // a component in a socket of its parent
        const blocks::BlockBox& parent = st->blocks[(size_t)box->parent];
        for (const blocks::SocketBox& sb : parent.sockets) if (sb.child == (int)index) nested = true;
        if (nested) { hostId = parent.id; hostWidthBefore = parent.rect.w; }
        if (nested) o = session_.apply([&](asmcore::blocks::Editor& ed) { return ed.unplugValue(parent.id, box->parentSlot, origin.x(), origin.y()); });
    }
    if (!nested) o = session_.apply([&](asmcore::blocks::Editor& ed) { return ed.liftBlocks(pressedId_, !single, origin.x(), origin.y()); });
    if (!o.ok) { session_.endGroup(); return false; }
    payload_ = o.section;
    if (payload_ >= layout_.stacks.size() || layout_.stacks[payload_].top.empty()) { session_.endGroup(); return false; }
    lifted_ = true;
    // soft effects: the block pops out, the row it left springs shut around the hole, and the stack leans with the pointer
    popEffect(pressedId_);
    if (!hostId.empty()) {
        const blocks::BlockBox* hb = nullptr;
        if (layout_.stackOf(hostId, &hb) && hb && hb->rect.w > 0) stretchEffect(hostId, hostWidthBefore / hb->rect.w);
    }
    if (animate_) { tilt_ = blocks::Spring(); tilt_.x = 0; tilt_.target = 0; tilt_.stiffness = 200; tilt_.damping = 9; tiltActive_ = true; lastPointer_ = pressPos_; startFxTimer(); }
    (void)scenePos;
    grab_ = pressPos_ - at;                                                         // where the block was held when the pointer went down
    return true;
}

void BlockScene::updatePayloadPreview(const QPointF& scenePos, const QPoint& screenPos) {
    if (payload_ >= layout_.stacks.size()) return;
    const QPointF o = payloadOrigin(scenePos);
    if (tiltActive_) {                                                  // lean into the direction of travel
        tilt_.target = std::max(-12.0, std::min(12.0, (scenePos.x() - lastPointer_.x()) * 1.4));
        lastPointer_ = scenePos;
    }
    for (QGraphicsItem* it : items())
        if (auto* si = dynamic_cast<StackItem*>(it))
            if (si->index() == payload_) { si->setPos(o); si->setZValue(500); }
    applyTilt();
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
    tiltActive_ = false;
    palette_ = false;
    std::string poppedId;
    if (payload < layout_.stacks.size() && !layout_.stacks[payload].top.empty()) poppedId = layout_.stacks[payload].blocks[(size_t)layout_.stacks[payload].top[0]].id;
    double hostBefore = 0;
    if (target.fits() && target.kind == blocks::DropTarget::Socket) {
        const blocks::BlockBox* hb = nullptr;
        if (layout_.stackOf(target.hostId, &hb) && hb) hostBefore = hb->rect.w;
    }
    if (drop) {
        if (target.fits()) {
            blocks::performDrop(session_, payload, target);
            // soft effects: what was dropped squashes into place; the row with the hole springs open to take it
            popEffect(poppedId);
            if (target.kind == blocks::DropTarget::Socket && hostBefore > 0) {
                const blocks::BlockBox* hb = nullptr;
                if (layout_.stackOf(target.hostId, &hb) && hb && hb->rect.w > 0) stretchEffect(target.hostId, hostBefore / hb->rect.w);
            }
        }
        else session_.apply([&](asmcore::blocks::Editor& ed) { return ed.moveSection(payload, where.x(), where.y()); });          // stays where it was put, floating
    }
    session_.endGroup();
    lifted_ = false;
    mode_ = Mode::Idle;
    rebuild();
}

bool BlockScene::beginPaletteDrag(std::vector<asmcore::blocks::Block> blocks, const QPointF& scenePos) {
    if (mode_ != Mode::Idle || blocks.empty()) return false;
    session_.beginGroup("palette:" + std::to_string(++dragSerial_));
    const asmcore::blocks::Outcome o = session_.apply([&](asmcore::blocks::Editor& ed) { return ed.newFloating(std::move(blocks), scenePos.x(), scenePos.y()); });
    if (!o.ok || o.section >= layout_.stacks.size() || layout_.stacks[o.section].top.empty()) { session_.endGroup(); return false; }
    payload_ = o.section;
    lifted_ = true;
    palette_ = true;
    grab_ = QPointF(16, 12);                                              // the pointer holds it by its top left corner area
    mode_ = Mode::DragPayload;
    const std::string first = layout_.stacks[payload_].blocks[(size_t)layout_.stacks[payload_].top[0]].id;
    session_.select({first});
    popEffect(first);
    return true;
}

void BlockScene::updatePaletteDrag(const QPointF& scenePos, const QPoint& screenPos) {
    if (palette_ && mode_ == Mode::DragPayload) updatePayloadPreview(scenePos, screenPos);
}

void BlockScene::endPaletteDrag(bool drop) {
    if (!palette_ || mode_ != Mode::DragPayload) return;
    if (drop) endPayloadDrag(true);
    else cancelDrag();
    palette_ = false;
}

void BlockScene::cancelDrag() {
    if (mode_ == Mode::DragPayload) {
        palette_ = false;
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

// The end of a block (or of a hat): the comment if there is one, else the empty zone reserved for it. Only blocks that are lines of the program carry a
// comment -- not components, not comment blocks.
std::vector<BlockScene::EditTarget> BlockScene::editTargets() const {
    std::vector<EditTarget> all;
    for (const blocks::StackLayout& s : layout_.stacks) {
        // the nearest block that is a line of the program (not a component) is the "row" of a block: its parts are read left to right
        std::vector<int> root(s.blocks.size(), 0);
        for (size_t i = 0; i < s.blocks.size(); i++) {
            int r = (int)i;
            while (s.blocks[(size_t)r].shape == blocks::Shape::Value && s.blocks[(size_t)r].parent >= 0) r = s.blocks[(size_t)r].parent;
            root[i] = r;
        }
        std::map<int, std::vector<EditTarget>> rows;
        std::vector<int> order;
        for (size_t i = 0; i < s.blocks.size(); i++) {
            const blocks::BlockBox& b = s.blocks[i];
            if (b.shape == blocks::Shape::Spacer) continue;
            const asmcore::blocks::BlockDef* def = session_.registry().find(b.type);
            auto fieldDef = [&](const std::string& name) -> const asmcore::blocks::FieldDef* {
                if (def) for (const asmcore::blocks::FieldDef& f : def->fields) if (f.name == name) return &f;
                return nullptr;
            };
            std::vector<EditTarget> mine;
            for (const blocks::FieldBox& f : b.fields) {
                if (f.name.empty()) continue;
                EditTarget t;
                t.blockId = b.id; t.part = f.name; t.flag = f.kind == asmcore::blocks::FieldKind::Flag; t.kind = f.kind;
                t.rect = QRectF(s.origin.x + f.rect.x, s.origin.y + f.rect.y, f.rect.w, f.rect.h);
                t.text = f.fullText;
                if (const asmcore::blocks::FieldDef* fd = fieldDef(f.name)) { t.options = fd->options; t.open = fd->open; }
                mine.push_back(t);
            }
            for (const blocks::SocketBox& sb : b.sockets) {
                if (sb.child >= 0) continue;                               // something is plugged in: it is edited itself
                EditTarget t;
                t.blockId = b.id; t.part = sb.name; t.socket = true;
                t.rect = QRectF(s.origin.x + sb.rect.x - sb.plugDepth, s.origin.y + sb.rect.y, sb.rect.w + sb.plugDepth, sb.rect.h);
                t.text = sb.fullText;
                t.accepts = sb.types;
                if (const asmcore::blocks::FieldDef* fd = fieldDef(sb.name)) { t.kind = fd->kind; t.options = fd->options; t.open = true; }
                mine.push_back(t);
            }
            std::sort(mine.begin(), mine.end(), [](const EditTarget& a, const EditTarget& c) { return a.rect.x() < c.rect.x(); });
            const int r = root[i];
            if (!rows.count(r)) order.push_back(r);
            for (const EditTarget& t : mine) rows[r].push_back(t);
        }
        for (int r : order) {
            auto& v = rows[r];
            std::stable_sort(v.begin(), v.end(), [](const EditTarget& a, const EditTarget& c) { return a.rect.x() < c.rect.x(); });
            for (const EditTarget& t : v) all.push_back(t);
        }
    }
    return all;
}

bool BlockScene::editTargetAt(const QPointF& p, EditTarget& out) const {
    const std::vector<EditTarget> all = editTargets();
    for (size_t i = all.size(); i-- > 0;) if (all[i].rect.contains(p)) { out = all[i]; return true; }
    return false;
}

bool BlockScene::commentTargetAt(const QPointF& p, CommentTarget& out) const {
    for (size_t i = layout_.stacks.size(); i-- > 0;) {
        const blocks::StackLayout& s = layout_.stacks[i];
        const blocks::Pt local{p.x() - s.origin.x, p.y() - s.origin.y};
        if (s.header.kind == blocks::HeaderBox::Hat && s.header.tailRect.contains(local)) {
            out.section = true;
            out.stack = i;
            out.blockId.clear();
            out.rect = QRectF(s.origin.x + s.header.tailRect.x, s.origin.y + s.header.tailRect.y, s.header.tailRect.w, s.header.tailRect.h);
            out.text = session_.document().sections[i].comment;
            return true;
        }
        for (size_t k = s.blocks.size(); k-- > 0;) {
            const blocks::BlockBox& b = s.blocks[k];
            if (b.shape == blocks::Shape::Value || b.shape == blocks::Shape::Note || b.shape == blocks::Shape::Spacer || b.tailRect.empty()) continue;
            if (!b.tailRect.contains(local)) continue;
            out.section = false;
            out.stack = i;
            out.blockId = b.id;
            out.rect = QRectF(s.origin.x + b.tailRect.x, s.origin.y + b.tailRect.y, b.tailRect.w, b.tailRect.h);
            out.text = b.comment;
            return true;
        }
    }
    return false;
}

bool BlockScene::commentTargetOf(const std::string& id, CommentTarget& out) const {
    const blocks::BlockBox* b = nullptr;
    const blocks::StackLayout* s = layout_.stackOf(id, &b);
    if (!s || !b || b->shape == blocks::Shape::Value || b->shape == blocks::Shape::Note || b->shape == blocks::Shape::Spacer || b->tailRect.empty()) return false;
    out.section = false;
    out.stack = s->section;
    out.blockId = id;
    out.rect = QRectF(s->origin.x + b->tailRect.x, s->origin.y + b->tailRect.y, b->tailRect.w, b->tailRect.h);
    out.text = b->comment;
    return true;
}

void BlockScene::contextMenuEvent(QGraphicsSceneContextMenuEvent* e) {
    contextRequested.fire(e->scenePos(), e->screenPos(), blockAt(e->scenePos()));
    e->accept();
}

void BlockScene::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* e) {
    CommentTarget t;
    EditTarget et;
    if (e->button() == Qt::LeftButton && editTargetAt(e->scenePos(), et) && !et.flag) { fieldEdit.fire(et); e->accept(); return; }          // a part of a block: edit it
    if (e->button() == Qt::LeftButton && commentTargetAt(e->scenePos(), t)) { commentEdit.fire(t); e->accept(); return; }
    QGraphicsScene::mouseDoubleClickEvent(e);
}

bool BlockScene::navigate(int dx, int dy) {
    if (session_.selection().size() != 1) {
        if (!session_.selection().empty()) return false;
        // nothing selected: start at the first block of the first stack
        for (const blocks::StackLayout& s : layout_.stacks) if (!s.top.empty()) { session_.select({s.blocks[(size_t)s.top[0]].id}); return true; }
        return false;
    }
    const std::string id = *session_.selection().begin();
    const blocks::BlockBox* box = nullptr;
    const blocks::StackLayout* st = layout_.stackOf(id, &box);
    if (!st || !box) return false;
    const size_t index = (size_t)(box - &st->blocks[0]);
    auto isStatement = [&](const blocks::BlockBox& b) { return b.shape != blocks::Shape::Value && b.shape != blocks::Shape::Spacer; };
    if (dy != 0) {
        // the instructions of the stack in reading order; from a component, start at the instruction it belongs to
        size_t from = index;
        while (st->blocks[from].shape == blocks::Shape::Value && st->blocks[from].parent >= 0) from = (size_t)st->blocks[from].parent;
        std::vector<size_t> order;
        for (size_t i = 0; i < st->blocks.size(); i++) if (isStatement(st->blocks[i])) order.push_back(i);
        auto at = std::find(order.begin(), order.end(), from);
        if (at == order.end()) return false;
        if (dy > 0 && at + 1 != order.end()) { session_.select({st->blocks[*(at + 1)].id}); return true; }
        if (dy < 0 && at != order.begin()) { session_.select({st->blocks[*(at - 1)].id}); return true; }
        // beyond the end / beginning of the stack: the neighbouring stack (in the order of the sections)
        const size_t n = layout_.stacks.size();
        for (size_t step = 1; step < n; step++) {
            const size_t k = dy > 0 ? (st->section + step) % n : (st->section + n - step) % n;
            const blocks::StackLayout& other = layout_.stacks[k];
            if (other.top.empty()) continue;
            const blocks::BlockBox* pick = nullptr;
            for (size_t i = 0; i < other.blocks.size(); i++) if (isStatement(other.blocks[i])) { if (dy > 0) { pick = &other.blocks[i]; break; } pick = &other.blocks[i]; }
            if (pick) { session_.select({pick->id}); return true; }
        }
        return false;
    }
    if (dx < 0) {                                                           // out: to what holds it
        if (box->parent >= 0) { session_.select({st->blocks[(size_t)box->parent].id}); return true; }
        return false;
    }
    if (dx > 0) {                                                           // in: the first component, else the first block of the first slot
        for (const blocks::SocketBox& sb : box->sockets) if (sb.child >= 0) { session_.select({st->blocks[(size_t)sb.child].id}); return true; }
        for (const blocks::SlotBox& sl : box->slotBoxes) if (!sl.items.empty()) { session_.select({st->blocks[(size_t)sl.items[0]].id}); return true; }
        return false;
    }
    return false;
}

void BlockScene::keyPressEvent(QKeyEvent* e) {
    // a printable key on the canvas: Typeblock
    if (mode_ == Mode::Idle && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !e->text().isEmpty() && e->text()[0].isPrint() && !e->text()[0].isSpace() &&
        e->key() != Qt::Key_Delete && e->key() != Qt::Key_Escape) {
        typeRequested.fire(e->text().toUtf8().toStdString());
        e->accept();
        return;
    }
    if (mode_ == Mode::Idle && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier))) {
        int dx = 0, dy = 0;
        if (e->key() == Qt::Key_Up) dy = -1; else if (e->key() == Qt::Key_Down) dy = 1; else if (e->key() == Qt::Key_Left) dx = -1; else if (e->key() == Qt::Key_Right) dx = 1;
        if ((dx || dy) && navigate(dx, dy)) {
            if (!session_.selection().empty()) { const QRectF r = blockRect(*session_.selection().begin()); for (QGraphicsView* v : views()) v->ensureVisible(r, 30, 30); }
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_F2) {         // type into the first part of the selected block
            if (session_.selection().size() == 1) {
                const std::string id = *session_.selection().begin();
                for (const EditTarget& t : editTargets()) if (t.blockId == id && !t.flag) { fieldEdit.fire(t); e->accept(); return; }
            }
        }
    }
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
