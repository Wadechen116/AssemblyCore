#include "block_scene.h"

#include <QFontMetricsF>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsRectItem>
#include <QGraphicsSceneMouseEvent>
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
    mode_ = Mode::Idle;
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
    if (dragOrigin_.empty()) for (size_t i : selectedStacks()) dragOrigin_[i] = QPointF(layout_.stacks[i].origin.x, layout_.stacks[i].origin.y);
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

void BlockScene::mouseMoveEvent(QGraphicsSceneMouseEvent* e) {
    if (mode_ == Mode::PressedBlock && (e->scenePos() - pressPos_).manhattanLength() > 4 && session_.isSelected(pressedId_)) {
        mode_ = Mode::MoveStacks;
        hovered_.clear();
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
    if (mode_ == Mode::MoveStacks) moveSelectedStacksBy(e->scenePos() - pressPos_, true);
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
