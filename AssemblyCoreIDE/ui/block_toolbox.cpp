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
#include <QVBoxLayout>
#include <set>

#include "block_painter.h"

namespace acide {

using blocks::CatalogEntry;

static QString tr2(const char* ctx, const std::string& s) { return QCoreApplication::translate(ctx, s.c_str()); }

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
    // a small picture of the sample under the pointer: its colour and name
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
    tree_->setUniformRowHeights(true);
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

void BlockToolbox::build() {
    tree_->clear();
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
            QString title = e->custom ? QString::fromStdString(e->title) : pattern ? tr2("BlockPattern", e->title) : tr2("BlockTitle", e->title);
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
