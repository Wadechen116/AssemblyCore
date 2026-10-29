#include "isa_top_view.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "text_util.h"

namespace acide {

namespace {

const qint64 kLitMs = 700, kPulseMs = 500, kFlashMs = 800;
const double kWireWidth = 3.2, kLitWidth = 6.5;                                   // the connecting lines: thick, so that they can be followed at a glance

QColor kindColour(const QString& kind, int domain) {
    if (kind == "decoder") return domain == 1 ? QColor("#14707d") : QColor("#8a6414");
    if (kind == "code") return domain == 1 ? QColor("#2d6068") : QColor("#6b5a2a");
    if (kind == "alu") return QColor("#6f4a96");
    if (kind == "flags") return QColor("#8e4a5c");
    if (kind == "data") return QColor("#3b6a4a");
    if (kind == "timer") return QColor("#6a6a2c");
    if (kind == "shared") return QColor("#505c6c");
    return QColor("#5e4a3a");                                                // device
}
QColor domainColour(bool peri) { return peri ? QColor("#19c8e0") : QColor("#ffb300"); }

}  // namespace

// The surface the diagram is drawn on, at the zoom of its view.
class IsaCanvas : public QWidget {
public:
    explicit IsaCanvas(IsaTopView& v) : v_(v) {
        setMouseTracking(true);
        ui_ = font();
        ui_.setPixelSize(13);
        mono_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono_.setPixelSize(11);
    }
    double scale() const { return v_.zoom_; }
    const IsaBox* boxAtPoint(const QPoint& p) const {
        const double s = scale();
        const double x = p.x() / s, y = p.y() / s;
        const IsaBox* hit = nullptr;
        for (const IsaBox& b : v_.layout_.boxes) if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) hit = &b;
        return hit;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette& pal = palette();
        p.fillRect(rect(), pal.color(QPalette::Window));
        const bool dark = pal.color(QPalette::Window).lightness() < 128;
        p.scale(scale(), scale());
        const qint64 now = v_.clock_.elapsed();
        const IsaLayout& L = v_.layout_;
        // wires
        const QColor wire = dark ? QColor("#8794a6") : QColor("#6b7686");
        for (const IsaWire& w : L.wires) {
            QPainterPath path;
            for (size_t i = 0; i < w.points.size(); i++) {
                const QPointF q(w.points[i].first, w.points[i].second);
                if (i == 0) path.moveTo(q); else path.lineTo(q);
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(wire, kWireWidth));
            p.drawPath(path);
            const QPointF a(w.points[w.points.size() - 2].first, w.points[w.points.size() - 2].second), b(w.points.back().first, w.points.back().second);
            arrow(p, a, b, wire);
            if (!w.label.empty()) {
                size_t best = 0;
                double len = -1;
                for (size_t i = 0; i + 1 < w.points.size(); i++) {
                    const double l = std::abs(w.points[i + 1].first - w.points[i].first) + std::abs(w.points[i + 1].second - w.points[i].second);
                    if (l > len) { len = l; best = i; }
                }
                const bool horizontal = std::abs(w.points[best + 1].second - w.points[best].second) < 1;
                if (len < (horizontal ? 90 : 34)) continue;                                  // no room for the text between two boxes
                QFont f = ui_;
                f.setPixelSize(10);
                p.setFont(f);
                p.setPen(dark ? QColor("#b4bdc9") : QColor("#4b5563"));
                const QPointF mid((w.points[best].first + w.points[best + 1].first) / 2, (w.points[best].second + w.points[best + 1].second) / 2);
                p.drawText(QRectF(mid.x() + 3, mid.y() - 14, 120, 12), Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8(w.label.c_str()));
            }
        }
        for (const auto& kv : v_.lit_) {                                                   // the wires of the instruction that runs
            const IsaWire* w = L.wire(kv.first);
            const qint64 age = now - kv.second.first;
            if (!w || age >= kLitMs) continue;
            QColor c = domainColour(kv.second.second);
            c.setAlphaF(1.0 - (double)age / kLitMs);
            QPainterPath path;
            for (size_t i = 0; i < w->points.size(); i++) {
                const QPointF q(w->points[i].first, w->points[i].second);
                if (i == 0) path.moveTo(q); else path.lineTo(q);
            }
            QPen pen(c, kLitWidth);
            pen.setStyle(Qt::CustomDashLine);
            pen.setDashPattern({2.2, 1.4});                                                // (in pen widths)
            pen.setDashOffset(-(double)age / 60.0);                                        // flows towards the arrow
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
            arrow(p, QPointF(w->points[w->points.size() - 2].first, w->points[w->points.size() - 2].second), QPointF(w->points.back().first, w->points.back().second), c);
        }
        // boxes
        for (size_t bi = 0; bi < L.boxes.size(); bi++) {
            const IsaBox& b = L.boxes[bi];
            const QRectF r(b.x, b.y, b.w, b.h);
            const QColor fill = kindColour(QString::fromStdString(b.kind), b.domain);
            p.setPen(QPen(fill.darker(150), 1));
            p.setBrush(fill);
            p.drawRoundedRect(r, 6, 6);
            QPainterPath bar;
            bar.addRoundedRect(QRectF(b.x, b.y, b.w, 24), 6, 6);
            bar.addRect(QRectF(b.x, b.y + 12, b.w, 12));
            p.setPen(Qt::NoPen);
            p.setBrush(fill.darker(135));
            p.drawPath(bar.simplified());
            p.setPen(Qt::white);
            QFont tf = ui_;
            tf.setBold(true);
            p.setFont(tf);
            p.drawText(QRectF(b.x + 8, b.y, b.w - 16, 24), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(tf).elidedText(QString::fromUtf8(b.title.c_str()), Qt::ElideRight, (int)b.w - 16));
            // rows
            for (size_t ri = 0; ri < b.rows.size(); ri++) {
                const double y = b.y + 28 + 20 * (double)ri;
                const qint64 age = bi < v_.changedAt_.size() && ri < v_.changedAt_[bi].size() ? now - v_.changedAt_[bi][ri] : kFlashMs;
                if (age < kFlashMs) {
                    QColor f("#ffe14d");
                    f.setAlphaF(0.75 * (1.0 - (double)age / kFlashMs));
                    p.setPen(Qt::NoPen);
                    p.setBrush(f);
                    p.drawRoundedRect(QRectF(b.x + 3, y, b.w - 6, 19), 3, 3);
                }
                const QString label = QString::fromUtf8(b.rows[ri].label.c_str());
                const QString value = bi < v_.values_.size() && ri < v_.values_[bi].size() ? QString::fromUtf8(v_.values_[bi][ri].c_str()) : QString("-");
                p.setFont(ui_);
                p.setPen(QColor(255, 255, 255, 190));
                p.drawText(QRectF(b.x + 8, y, b.w - 16, 19), Qt::AlignLeft | Qt::AlignVCenter, label);
                const int lw = QFontMetrics(ui_).horizontalAdvance(label) + 10;
                p.setFont(mono_);
                p.setPen(Qt::white);
                p.drawText(QRectF(b.x + 8 + lw, y, b.w - 16 - lw, 19), Qt::AlignRight | Qt::AlignVCenter, QFontMetrics(mono_).elidedText(value, Qt::ElideLeft, (int)(b.w - 16 - lw)));
            }
            if (v_.live_ && b.kind == "decoder" && b.domain >= 0 && v_.ctx_.debugger.focus() == (b.domain == 1 ? asmcore::Domain::Peri : asmcore::Domain::Main)) {      // the one Step acts on
                QFont sf = ui_;
                sf.setPixelSize(10);
                sf.setBold(true);
                p.setFont(sf);
                const QRectF tag(b.x + b.w - 62, b.y + 4, 56, 16);
                p.setPen(QPen(Qt::white, 1.5));
                p.setBrush(domainColour(b.domain == 1));
                p.drawRoundedRect(tag, 4, 4);
                p.setPen(QColor(20, 24, 32));
                p.drawText(tag, Qt::AlignCenter, QString::fromUtf8("STEP \xE2\x96\xB6"));
            }
            const auto pu = v_.pulse_.find(b.id);                                          // a decoder that just executed
            if (pu != v_.pulse_.end() && now - pu->second.first < kPulseMs) {
                QColor c = domainColour(pu->second.second);
                c.setAlphaF(1.0 - (double)(now - pu->second.first) / kPulseMs);
                p.setPen(QPen(c, 4));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(-2, -2, 2, 2), 8, 8);
            }
            if (b.id == hover_ && (!b.target.empty())) {
                p.setPen(QPen(QColor(255, 255, 255, 220), 2, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(-1, -1, 1, 1), 7, 7);
            }
        }
        if (!v_.live_) {
            p.setFont(ui_);
            p.setPen(dark ? QColor("#e6c15a") : QColor("#8a5a00"));
            p.drawText(QRectF(20, 2, 700, 16), Qt::AlignLeft | Qt::AlignVCenter, QCoreApplication::translate("IsaTopView", "Start debugging (F5) to see the diagram live: the values below are what the simulator holds."));
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        const IsaBox* b = boxAtPoint(e->pos());
        const std::string id = b ? b->id : std::string();
        if (id != hover_) { hover_ = id; setCursor(b && !b->target.empty() ? Qt::PointingHandCursor : Qt::ArrowCursor); update(); }
    }
    void leaveEvent(QEvent*) override { if (!hover_.empty()) { hover_.clear(); update(); } }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        if (const IsaBox* b = boxAtPoint(e->pos())) v_.activate(*b);
        else if (e->type() == QEvent::MouseButtonDblClick) v_.fit();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override { if (!boxAtPoint(e->pos())) v_.fit(); }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            const auto* he = static_cast<QHelpEvent*>(e);
            const IsaBox* b = boxAtPoint(he->pos());
            if (b && !b->help.empty()) QToolTip::showText(he->globalPos(), QString::fromUtf8(b->help.c_str()), this);
            else QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }

private:
    static void arrow(QPainter& p, const QPointF& a, const QPointF& b, const QColor& c) {
        const QPointF d = b - a;
        const double len = std::hypot(d.x(), d.y());
        if (len < 1) return;
        const QPointF u(d.x() / len, d.y() / len), n(-u.y(), u.x());
        QPolygonF poly;
        poly << b << (b - u * 15 + n * 7.5) << (b - u * 15 - n * 7.5);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(poly);
    }
    friend class IsaTopView;
    IsaTopView& v_;
    QFont ui_, mono_;
    std::string hover_;
};

IsaTopView::IsaTopView(AppContext& ctx, QWidget* parent) : QScrollArea(parent), ctx_(ctx) {
    setObjectName("isaTopView");
    clock_.start();
    layout_ = parseIsaLayout(defaultIsaLayoutText());
    const std::string file = ctx.settings.get("isatop.layout");                          // another drawing, same format
    if (!file.empty()) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        IsaLayout l = parseIsaLayout(ss.str());
        if (in && l.errors.empty() && !l.boxes.empty()) layout_ = l;
        else ctx.log.fire(QString("isatop.layout %1: %2").arg(fromStd(file), l.errors.empty() ? QString("cannot be read") : fromStd(l.errors[0])));
    }
    canvas_ = new IsaCanvas(*this);
    canvas_->setObjectName("isaTopCanvas");
    setWidget(canvas_);
    setWidgetResizable(false);
    setFrameShape(QFrame::NoFrame);
    setMinimumHeight(120);
    timer_.setInterval(40);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    conns_.add(ctx.debugger.updated, [this](const SimUpdate& u) { feed(u); });
    conns_.add(ctx.debugger.focusChanged, [this](asmcore::Domain) { canvas_->update(); });
    refreshValues();
    rescale();
}

IsaTopView::~IsaTopView() = default;

QWidget* IsaTopView::canvas() const { return canvas_; }

void IsaTopView::setLayout(const IsaLayout& l) {
    layout_ = l;
    lit_.clear();
    pulse_.clear();
    values_.clear();
    changedAt_.clear();
    refreshValues();
    rescale();
}

void IsaTopView::fit() {
    fit_ = true;
    fitAll_ = true;
    rescale();
}

void IsaTopView::fitWidth() {
    fit_ = true;
    fitAll_ = false;
    rescale();
}

void IsaTopView::setZoom(double z) {
    fit_ = false;
    fitAll_ = false;
    zoom_ = std::max(0.2, std::min(3.0, z));
    rescale();
}

void IsaTopView::rescale() {
    if (fit_) {
        double z = (double)(viewport()->width() - 2) / layout_.width;
        if (fitAll_) z = std::min(z, (double)(viewport()->height() - 2) / layout_.height);
        zoom_ = std::max(0.2, std::min(1.5, z));
    }
    canvas_->resize((int)(layout_.width * zoom_), (int)(layout_.height * zoom_));
    canvas_->update();
}

void IsaTopView::resizeEvent(QResizeEvent* e) {
    QScrollArea::resizeEvent(e);
    if (fit_) rescale();
}

void IsaTopView::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::ControlModifier) {
        setZoom(zoom_ * (e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1));
        e->accept();
        return;
    }
    QScrollArea::wheelEvent(e);
}

std::string IsaTopView::instrAt(int domain, uint32_t pc) {
    SnapshotPtr prog = ctx_.debugger.session().program();
    if (prog.get() != instrsFor_) {                                                      // another program: the table is made again
        instrs_.clear();
        instrsFor_ = prog.get();
        if (prog) for (const asmcore::InstrInfo& i : prog->result.instrs) instrs_[{i.domain == asmcore::Domain::Main ? 0 : 1, i.pc}] = i.text;
    }
    const auto it = instrs_.find({domain, pc});
    return it == instrs_.end() ? std::string() : it->second;
}

void IsaTopView::refreshValues() {
    ViewPtr view = ctx_.debugger.session().view();
    live_ = view && ctx_.debugger.active();
    const qint64 now = clock_.elapsed();
    if (values_.size() != layout_.boxes.size()) {
        values_.assign(layout_.boxes.size(), {});
        changedAt_.assign(layout_.boxes.size(), {});
    }
    bool changed = false;
    for (size_t bi = 0; bi < layout_.boxes.size(); bi++) {
        const IsaBox& b = layout_.boxes[bi];
        const bool first = values_[bi].size() != b.rows.size();
        if (first) { values_[bi].assign(b.rows.size(), std::string()); changedAt_[bi].assign(b.rows.size(), -1000000); }
        for (size_t ri = 0; ri < b.rows.size(); ri++) {
            const std::string v = isaValue(b.rows[ri].src, live_ ? view.get() : nullptr, [this](int d, uint32_t pc) { return instrAt(d, pc); });
            if (v != values_[bi][ri]) {
                if (!first && !values_[bi][ri].empty() && values_[bi][ri] != "-" && v != "-") changedAt_[bi][ri] = now;           // (not the first reading, not from or to "nothing")
                values_[bi][ri] = v;
                changed = true;
            }
        }
    }
    if (changed || animating()) { canvas_->update(); if (animating() && !timer_.isActive()) timer_.start(); }
}

void IsaTopView::feed(const SimUpdate& u) {
    const qint64 now = clock_.elapsed();
    if (u.eventsReset || u.state == SimState::Empty) { lit_.clear(); pulse_.clear(); }
    const size_t from = u.events.size() > 300 ? u.events.size() - 300 : 0;
    for (size_t i = from; i < u.events.size(); i++) {
        const asmcore::sim::Event& e = u.events[i];
        if (e.type != asmcore::sim::EventType::Issue || (e.domain != 0 && e.domain != 1)) continue;
        for (const std::string& w : layout_.wiresFor(e.domain, e.what)) lit_[w] = {now, e.domain == 1};
        for (const IsaBox& b : layout_.boxes) if (b.kind == "decoder" && b.domain == e.domain) pulse_[b.id] = {now, e.domain == 1};
    }
    refreshValues();
    if (animating() && !timer_.isActive()) timer_.start();
}

bool IsaTopView::animating() const {
    const qint64 now = clock_.elapsed();
    for (const auto& kv : lit_) if (now - kv.second.first < kLitMs) return true;
    for (const auto& kv : pulse_) if (now - kv.second.first < kPulseMs) return true;
    for (const auto& row : changedAt_) for (qint64 t : row) if (now - t < kFlashMs) return true;
    return false;
}

void IsaTopView::tick() {
    canvas_->update();
    if (!animating()) timer_.stop();
}

std::vector<std::string> IsaTopView::litWires() const {
    std::vector<std::string> v;
    const qint64 now = clock_.elapsed();
    for (const auto& kv : lit_) if (now - kv.second.first < kLitMs) v.push_back(kv.first);
    return v;
}
std::vector<std::string> IsaTopView::pulsingBoxes() const {
    std::vector<std::string> v;
    const qint64 now = clock_.elapsed();
    for (const auto& kv : pulse_) if (now - kv.second.first < kPulseMs) v.push_back(kv.first);
    return v;
}
std::vector<std::string> IsaTopView::flashingRows() const {
    std::vector<std::string> v;
    const qint64 now = clock_.elapsed();
    for (size_t bi = 0; bi < changedAt_.size() && bi < layout_.boxes.size(); bi++)
        for (size_t ri = 0; ri < changedAt_[bi].size() && ri < layout_.boxes[bi].rows.size(); ri++)
            if (now - changedAt_[bi][ri] < kFlashMs) v.push_back(layout_.boxes[bi].id + "/" + layout_.boxes[bi].rows[ri].label);
    return v;
}
std::string IsaTopView::valueOf(const std::string& box, const std::string& label) const {
    for (size_t bi = 0; bi < layout_.boxes.size() && bi < values_.size(); bi++)
        if (layout_.boxes[bi].id == box)
            for (size_t ri = 0; ri < layout_.boxes[bi].rows.size() && ri < values_[bi].size(); ri++)
                if (layout_.boxes[bi].rows[ri].label == label) return values_[bi][ri];
    return "<none>";
}
QRect IsaTopView::boxRect(const std::string& id) const {
    if (const IsaBox* b = layout_.box(id)) return QRect((int)(b->x * zoom_), (int)(b->y * zoom_), (int)(b->w * zoom_), (int)(b->h * zoom_));
    return QRect();
}

void IsaTopView::activate(const IsaBox& b) {
    if (b.target.empty()) return;
    if (b.target == "code") {
        const asmcore::Domain d = b.domain == 1 ? asmcore::Domain::Peri : asmcore::Domain::Main;
        const DebugLocation loc = ctx_.debugger.location(d);
        if (!loc.valid) { ctx_.status.fire(QCoreApplication::translate("IsaTopView", "Start debugging to see where the %1 decoder is").arg(b.domain == 1 ? "Peri" : "Main")); return; }
        Location l;
        l.file = loc.file;
        l.line = loc.line;
        ctx_.showExecution.fire(l);
        return;
    }
    ctx_.showPanel.fire(QString::fromStdString(b.target));
}

QWidget* createIsaTopPanel(AppContext& ctx) {
    auto* page = new QWidget;
    page->setObjectName("isaTopPanel");
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* top = new QHBoxLayout;
    top->setContentsMargins(4, 2, 4, 2);
    auto* title = new QLabel(QCoreApplication::translate("IsaTopView", "ISA Top"));
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    top->addWidget(title);
    top->addStretch(1);
    auto* fit = new QPushButton(QCoreApplication::translate("IsaTopView", "Fit"));
    fit->setObjectName("isaTopFit");
    fit->setToolTip(QCoreApplication::translate("IsaTopView", "The whole diagram in the view (double click on the empty canvas does the same; Ctrl+wheel zooms)"));
    auto* one = new QPushButton("100%");
    one->setObjectName("isaTopActual");
    auto* wide = new QPushButton(QCoreApplication::translate("IsaTopView", "Fit width"));
    wide->setObjectName("isaTopFitWidth");
    wide->setToolTip(QCoreApplication::translate("IsaTopView", "The whole width of the diagram in the view, readable; the rest scrolls"));
    top->addWidget(wide);
    top->addWidget(fit);
    top->addWidget(one);
    lay->addLayout(top);
    auto* view = new IsaTopView(ctx);
    lay->addWidget(view, 1);
    QObject::connect(fit, &QPushButton::clicked, view, [view] { view->fit(); });
    QObject::connect(wide, &QPushButton::clicked, view, [view] { view->fitWidth(); });
    QObject::connect(one, &QPushButton::clicked, view, [view] { view->setZoom(1.0); });
    return page;
}

}  // namespace acide
