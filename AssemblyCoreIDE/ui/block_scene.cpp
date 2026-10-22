#include "block_scene.h"

#include <QFontMetricsF>
#include <QGraphicsSceneHoverEvent>
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

void StackItem::mousePressEvent(QGraphicsSceneMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { e->ignore(); return; }
    const BlockBox* b = boxAt(e->pos());
    if (b) scene_.blockClicked.fire(b->id, e->modifiers());
    else scene_.emptyClicked.fire(e->modifiers());
    e->accept();
}

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

QRectF BlockScene::blockRect(const std::string& id) const {
    const BlockBox* box = nullptr;
    const StackLayout* s = layout_.stackOf(id, &box);
    if (!s || !box) return QRectF();
    return QRectF(s->origin.x + box->rect.x, s->origin.y + box->rect.y, box->rect.w, box->rect.h);
}

}  // namespace acide
