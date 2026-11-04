#include "block_toolbox.h"

#include <QApplication>
#include <QDrag>
#include <QHeaderView>
#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QToolButton>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QStyledItemDelegate>
#include <QLinearGradient>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <set>

#include "block_painter.h"

namespace acide {

using blocks::CatalogEntry;

static QString tr2(const char* ctx, const std::string& s) { return QCoreApplication::translate(ctx, s.c_str()); }

// "write AE_CTRL", "set bits AE_CTRL", "read DEV_IMU (several bytes)": the verb is translated, the name of the part never is.
static QString hardwareTitle(const std::string& title) {
    static const char* verbs[] = {"set bits", "clear bits", "write", "read", "publish", "load"};
    for (const char* v : verbs) {
        const std::string vs = v;
        if (title.compare(0, vs.size() + 1, vs + " ") != 0) continue;
        QString rest = QString::fromStdString(title.substr(vs.size() + 1));
        rest.replace(" (several bytes)", QCoreApplication::translate("BlockHardware", " (several bytes)"));
        return QCoreApplication::translate("BlockHardware", v) + " " + rest;
    }
    return QString::fromStdString(title);
}

// The samples drawn as blocks: the picture of the sample (smaller than on the canvas, so that a drawer shows many), the [Main] / [Peri] / star marks on it.
// A picture that is too wide for the toolbox is made smaller still (down to kMinScale), then cut off with a fade; a tall pattern is cut off at the bottom.
namespace {
constexpr double kScale = 0.8, kMinScale = 0.55;
constexpr int kMaxHeight = 150, kPad = 3;

class SampleDelegate : public QStyledItemDelegate {
public:
    SampleDelegate(const BlockToolbox* box, QObject* parent) : QStyledItemDelegate(parent), box_(box) {}
    QImage pictureOf(const QModelIndex& i) const { return i.parent().isValid() ? box_->picture(i.data(Qt::UserRole).toString()) : QImage(); }
    static QSizeF logical(const QImage& img) { return QSizeF(img.width() / img.devicePixelRatio(), img.height() / img.devicePixelRatio()); }

    // a pattern (several blocks) has its name above the picture, with the [Main] / [Peri] mark; a single block carries the mark on its corner
    static QString captionOf(const QModelIndex& i) { return i.data(Qt::UserRole + 6).toString(); }
    static QFont captionFont(const QStyleOptionViewItem& o) { QFont f = o.font; f.setBold(true); f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1)); return f; }
    static int captionHeight(const QStyleOptionViewItem& o, const QModelIndex& i) { return captionOf(i).isEmpty() ? 0 : (int)std::ceil(QFontMetricsF(captionFont(o)).height()) + 2; }

    // the scale a picture is drawn at in a row `width` wide: kScale, smaller (down to kMinScale) when the block is too wide for the toolbox
    static double scaleFor(const QSizeF& full, double width) { return std::max(kMinScale, std::min(kScale, (width - 2 - 2 * kPad) / std::max(1.0, full.width()))); }
    static double rowWidth(const QStyleOptionViewItem& o) {
        const auto* tv = qobject_cast<const QTreeView*>(o.widget);
        return tv ? tv->viewport()->width() - tv->indentation() : 200;          // a sample is one level in
    }

    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override {
        const QImage img = pictureOf(i);
        if (img.isNull()) return QStyledItemDelegate::sizeHint(o, i);
        const QSizeF full = logical(img);
        const QSizeF s = full * scaleFor(full, rowWidth(o));
        return QSize((int)std::ceil(s.width()) + 2 * kPad, std::min(kMaxHeight, (int)std::ceil(s.height())) + 2 * kPad + captionHeight(o, i));
    }
    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override {
        const QImage img = pictureOf(i);
        if (img.isNull()) { QStyledItemDelegate::paint(p, o, i); return; }
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRectF row = QRectF(o.rect).adjusted(1, 1, -1, -1);
        const int ch = captionHeight(o, i);
        QRectF r = row.adjusted(0, ch, 0, 0);
        if (o.state & (QStyle::State_Selected | QStyle::State_MouseOver)) {               // selected / under the pointer: a soft frame behind the block
            QColor h = o.palette.color(QPalette::Highlight);
            h.setAlphaF((o.state & QStyle::State_Selected) ? 0.30f : 0.12f);
            p->setPen(Qt::NoPen);
            p->setBrush(h);
            p->drawRoundedRect(row, 5, 5);
        }
        const QString mark = i.data(Qt::UserRole + 5).toString();                         // [Main] / [Peri] / the star of the user's own blocks
        if (ch) {
            const QFont cf = captionFont(o);
            p->setFont(cf);
            p->setPen(o.palette.color(QPalette::Text));
            const QRectF cr(row.left() + kPad, row.top(), row.width() - 2 * kPad, ch);
            const QString text = captionOf(i) + (mark.isEmpty() ? QString() : "  [" + mark + "]");
            p->drawText(cr, Qt::AlignLeft | Qt::AlignVCenter, QFontMetricsF(cf).elidedText(text, Qt::ElideRight, cr.width()));
        }
        const QSizeF full = logical(img);
        const double avail = r.width() - 2 * kPad;
        const double sc = scaleFor(full, row.width() + 2);
        const QSizeF s = full * sc;
        const double h = std::min<double>(s.height(), kMaxHeight);
        const QRectF clip(r.left() + kPad, r.top() + (r.height() - h) / 2, std::min(s.width(), avail), h);
        // drawn into a layer so that a cut-off edge can fade out
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        QImage layer((clip.size() * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
        if (!layer.isNull()) {
            layer.setDevicePixelRatio(dpr);
            layer.fill(Qt::transparent);
            QPainter lp(&layer);
            lp.setRenderHint(QPainter::SmoothPixmapTransform, true);
            lp.drawImage(QRectF(QPointF(0, 0), s), img);
            lp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            if (s.width() > clip.width() + 0.5) {
                QLinearGradient g(clip.width() - 24, 0, clip.width(), 0);
                g.setColorAt(0, Qt::black); g.setColorAt(1, Qt::transparent);
                lp.fillRect(QRectF(0, 0, clip.width(), clip.height()), g);
            }
            if (s.height() > clip.height() + 0.5) {
                QLinearGradient g(0, clip.height() - 24, 0, clip.height());
                g.setColorAt(0, Qt::black); g.setColorAt(1, Qt::transparent);
                lp.fillRect(QRectF(0, 0, clip.width(), clip.height()), g);
            }
            lp.end();
            p->drawImage(clip.topLeft(), layer);
        }
        if (!mark.isEmpty() && !ch) {
            QFont f = o.font;
            f.setPointSizeF(std::max(6.5, f.pointSizeF() - 2));
            f.setBold(true);
            p->setFont(f);
            const QFontMetricsF fm(f);
            const QRectF b(r.right() - fm.horizontalAdvance(mark) - 8, r.top() + 1, fm.horizontalAdvance(mark) + 6, fm.height());
            QColor bg = o.palette.color(QPalette::Base);
            bg.setAlphaF(0.85f);
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawRoundedRect(b, 3, 3);
            p->setPen(o.palette.color(QPalette::Text));
            p->drawText(b, Qt::AlignCenter, mark);
        }
        p->restore();
    }

private:
    const BlockToolbox* box_;
};
}  // namespace

void SampleTree::resizeEvent(QResizeEvent* e) {
    QTreeWidget::resizeEvent(e);
    scheduleDelayedItemsLayout();                                            // the pictures are scaled to the width: the rows change height with it
}

QMimeData* SampleTree::mimeData(const QList<QTreeWidgetItem*>& items) const {
    if (items.isEmpty() || items[0]->data(0, Qt::UserRole).toString().isEmpty()) return nullptr;
    auto* m = new QMimeData;
    m->setData(mimeType(), items[0]->data(0, Qt::UserRole).toString().toUtf8());
    m->setText(items[0]->toolTip(0));
    return m;
}

void SampleTree::startDrag(Qt::DropActions) {
    const QList<QTreeWidgetItem*> sel = selectedItems();
    QMimeData* m = mimeData(sel);
    if (!m) return;
    auto* drag = new QDrag(this);
    drag->setMimeData(m);
    // the sample under the pointer: its picture as a block if it has one, else its colour and name
    const auto* box = qobject_cast<const BlockToolbox*>(parentWidget());
    const QImage pic = box ? box->picture(sel[0]->data(0, Qt::UserRole).toString()) : QImage();
    if (!pic.isNull()) {
        QPixmap pm = QPixmap::fromImage(pic);
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(12, (int)(pm.height() / pm.devicePixelRatio() / 2)));
        drag->exec(Qt::CopyAction);
        return;
    }
    const QString title = sel[0]->text(0);
    const QFontMetrics fm(font());
    QPixmap pm(fm.horizontalAdvance(title) + 24, fm.height() + 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(sel[0]->background(0));
    p.setPen(QPen(QColor(0, 0, 0, 90), 1));
    p.drawRoundedRect(QRectF(0.5, 0.5, pm.width() - 1, pm.height() - 1), 6, 6);
    p.setPen(sel[0]->foreground(0).color());
    p.drawText(pm.rect(), Qt::AlignCenter, title);
    p.end();
    drag->setPixmap(pm);
    drag->setHotSpot(QPoint(12, pm.height() / 2));
    drag->exec(Qt::CopyAction);
}

BlockToolbox::BlockToolbox(const blocks::BlockCatalog& catalog, const blocks::BlockTheme& theme, QWidget* parent)
    : QWidget(parent), catalog_(catalog), theme_(theme) {
    setObjectName("blockToolbox");
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(4, 4, 4, 4);
    l->setSpacing(4);
    search_ = new QLineEdit(this);
    search_->setObjectName("toolboxSearch");
    search_->setPlaceholderText(tr("Search the blocks..."));
    search_->setClearButtonEnabled(true);
    l->addWidget(search_);
    tree_ = new SampleTree(this);
    tree_->setObjectName("toolboxTree");
    tree_->setHeaderHidden(true);
    tree_->setIndentation(10);
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setUniformRowHeights(false);                            // a sample is as tall as its blocks
    tree_->setItemDelegate(new SampleDelegate(this, tree_));
    tree_->setMouseTracking(true);
    tree_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tree_->setToolTipDuration(8000);
    l->addWidget(tree_, 1);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) { showMenu(at); });
    connect(search_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it) {
        const QString id = it->data(0, Qt::UserRole).toString();
        if (!id.isEmpty()) emit activated(id);
    });
    build();
}

std::vector<std::string> BlockToolbox::drawerNames() const {
    std::vector<std::string> v;
    for (int i = 0; i < tree_->topLevelItemCount(); i++) v.push_back(tree_->topLevelItem(i)->data(0, Qt::UserRole + 2).toString().toStdString());
    return v;
}

// build the drawers again (a block was added or removed), keeping which are open and what is typed in the search box
void BlockToolbox::refresh() {
    std::set<QString> open;
    for (int i = 0; i < tree_->topLevelItemCount(); i++) if (tree_->topLevelItem(i)->isExpanded()) open.insert(tree_->topLevelItem(i)->data(0, Qt::UserRole + 2).toString());
    build();
    for (int i = 0; i < tree_->topLevelItemCount(); i++) if (open.count(tree_->topLevelItem(i)->data(0, Qt::UserRole + 2).toString())) tree_->topLevelItem(i)->setExpanded(true);
    applyFilter();
}

// right button: new block in the drawer under the pointer; on one of the user's own: edit / delete
void BlockToolbox::showMenu(const QPoint& at) {
    QTreeWidgetItem* it = tree_->itemAt(at);
    if (it) tree_->setCurrentItem(it);
    const QString drawer = it ? it->data(0, Qt::UserRole + 2).toString() : QString();
    QMenu menu(this);
    menu.setObjectName("toolboxMenu");
    const QString where = drawer.isEmpty() ? QString() : tr2("BlockDrawer", drawer.toStdString());
    QAction* add = menu.addAction(drawer.isEmpty() ? tr("New block...") : tr("New block in \"%1\"...").arg(where));
    connect(add, &QAction::triggered, this, [this, drawer] { emit addRequested(drawer, false); });
    QAction* fromSel = menu.addAction(drawer.isEmpty() ? tr("New block from the selected blocks...") : tr("Save the selected blocks in \"%1\"...").arg(where));
    fromSel->setEnabled(selectionAvailable_);
    connect(fromSel, &QAction::triggered, this, [this, drawer] { emit addRequested(drawer, true); });
    if (it && it->data(0, Qt::UserRole + 3).toBool()) {
        menu.addSeparator();
        const QString id = it->data(0, Qt::UserRole).toString();
        connect(menu.addAction(tr("Edit...")), &QAction::triggered, this, [this, id] { emit editRequested(id); });
        connect(menu.addAction(tr("Delete")), &QAction::triggered, this, [this, id] { emit deleteRequested(id); });
    }
    menu.exec(tree_->viewport()->mapToGlobal(at));
}

void BlockToolbox::setTheme(const blocks::BlockTheme& t) {
    theme_ = t;
    build();
    applyFilter();
}

QImage BlockToolbox::picture(const QString& entryId) const {
    if (!previewer_ || entryId.isEmpty()) return QImage();
    auto it = pictures_.find(entryId);
    if (it != pictures_.end()) return it->second;
    const CatalogEntry* e = catalog_.find(entryId.toStdString());
    QImage img = e ? previewer_(*e) : QImage();
    if (!img.isNull() && (img.width() < 2 || img.height() < 2)) img = QImage();
    pictures_[entryId] = img;
    return img;
}

void BlockToolbox::build() {
    tree_->clear();
    pictures_.clear();                                                           // (the catalog or the theme changed: the pictures are drawn again when shown)
    for (const blocks::BlockCatalog::Drawer& d : catalog_.drawers()) {
        const bool pattern = d.cls == "pattern";
        const QColor col = pattern ? toQColor(theme_.selection) : toQColor(theme_.categoryColor(d.name));
        auto* top = new QTreeWidgetItem(tree_);
        top->setText(0, tr2("BlockDrawer", d.name));
        QFont f = top->font(0);
        f.setBold(true);
        top->setFont(0, f);
        top->setBackground(0, col);
        top->setForeground(0, toQColor(theme_.textOn(blocks::Rgba{(uint8_t)col.red(), (uint8_t)col.green(), (uint8_t)col.blue(), 255})));
        top->setFlags(Qt::ItemIsEnabled);
        top->setData(0, Qt::UserRole + 2, QString::fromStdString(d.name));
        // the header: the name, and a + that adds a block of the user's own to this drawer
        {
            auto* head = new QWidget;
            head->setAutoFillBackground(true);
            QPalette pal = head->palette();
            pal.setColor(QPalette::Window, col);
            head->setPalette(pal);
            auto* hl = new QHBoxLayout(head);
            hl->setContentsMargins(4, 0, 2, 0);
            auto* name = new QLabel(top->text(0), head);
            name->setFont(f);
            name->setAttribute(Qt::WA_TransparentForMouseEvents);
            QPalette np = name->palette();
            np.setColor(QPalette::WindowText, top->foreground(0).color());
            name->setPalette(np);
            hl->addWidget(name, 1);
            auto* add = new QToolButton(head);
            add->setText("+");
            add->setObjectName("addBlock");
            add->setProperty("drawer", QString::fromStdString(d.name));
            add->setAutoRaise(true);
            add->setCursor(Qt::PointingHandCursor);
            add->setToolTip(tr("Add a block of your own to this drawer"));
            add->setFont(f);
            QPalette bp = add->palette();
            bp.setColor(QPalette::ButtonText, top->foreground(0).color());
            add->setPalette(bp);
            const QString dname = QString::fromStdString(d.name);
            connect(add, &QToolButton::clicked, this, [this, dname] { emit addRequested(dname, false); });
            hl->addWidget(add);
            tree_->setItemWidget(top, 0, head);
        }
        for (const CatalogEntry* e : d.entries) {
            auto* it = new QTreeWidgetItem(top);
            QString title = e->hardware ? hardwareTitle(e->title) : e->custom ? QString::fromStdString(e->title) : pattern ? tr2("BlockPattern", e->title) : tr2("BlockTitle", e->title);
            if (e->domains == asmcore::blocks::kMain && !e->component()) title += "  [Main]";
            else if (e->domains == asmcore::blocks::kPeri && !e->component()) title += "  [Peri]";
            it->setText(0, title);
            it->setData(0, Qt::UserRole, QString::fromStdString(e->id));
            it->setData(0, Qt::UserRole + 1, QString::fromStdString(e->title));
            it->setData(0, Qt::UserRole + 2, QString::fromStdString(d.name));
            it->setData(0, Qt::UserRole + 3, e->custom);
            if (e->custom) {                                                         // the user's own: italic, with a star
                QFont cf = it->font(0);
                cf.setItalic(true);
                it->setFont(0, cf);
                it->setText(0, QString::fromStdString(e->title) + "  \u2605");
            }
            QColor tint = col;
            tint.setAlphaF(0.55f);
            it->setBackground(0, tint);
            it->setForeground(0, toQColor(theme_.textOn(blocks::Rgba{(uint8_t)col.red(), (uint8_t)col.green(), (uint8_t)col.blue(), 255})));
            QString tip = e->text.empty() ? QString::fromStdString(e->description) : QString::fromStdString(e->text);
            if (!e->description.empty() && !e->text.empty()) tip = tr2("BlockPattern", e->description) + "\n\n" + tip;
            it->setToolTip(0, tip);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            if (previewer_) {                                                         // drawn as its blocks (the picture when the row is shown; the text stays: search, tooltips, accessibility)
                QString mark = e->custom ? QStringLiteral("\u2605") : QString();
                if (e->domains == asmcore::blocks::kMain && !e->component()) mark = "Main";
                else if (e->domains == asmcore::blocks::kPeri && !e->component()) mark = "Peri";
                it->setData(0, Qt::UserRole + 5, mark);
                if (pattern) it->setData(0, Qt::UserRole + 6, e->custom ? QString::fromStdString(e->title) : tr2("BlockPattern", e->title));
            }
        }
        top->setExpanded(pattern || d.name == "Data" || d.name == "Control");
    }
}

int BlockToolbox::visibleSamples() const {
    int n = 0;
    for (int i = 0; i < tree_->topLevelItemCount(); i++) {
        QTreeWidgetItem* top = tree_->topLevelItem(i);
        for (int k = 0; k < top->childCount(); k++) if (!top->child(k)->isHidden()) n++;
    }
    return n;
}

void BlockToolbox::setFilter(const QString& text) { search_->setText(text); }

void BlockToolbox::applyFilter() {
    const std::string q = search_->text().toUtf8().toStdString();
    std::set<std::string> ids;
    const bool filtering = !q.empty();
    if (filtering) for (const CatalogEntry* e : catalog_.search(q, 200)) ids.insert(e->id);
    for (int i = 0; i < tree_->topLevelItemCount(); i++) {
        QTreeWidgetItem* top = tree_->topLevelItem(i);
        int shown = 0;
        for (int k = 0; k < top->childCount(); k++) {
            QTreeWidgetItem* it = top->child(k);
            const bool show = !filtering || ids.count(it->data(0, Qt::UserRole).toString().toStdString());
            it->setHidden(!show);
            if (show) shown++;
        }
        top->setHidden(shown == 0);
        if (filtering && shown) top->setExpanded(true);
    }
}

}  // namespace acide
