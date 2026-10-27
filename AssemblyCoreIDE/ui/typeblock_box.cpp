#include "typeblock_box.h"

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace acide {

using blocks::CatalogEntry;

namespace {
const int kEntry = Qt::UserRole, kKind = Qt::UserRole + 1;           // kind: 0 a catalog entry, 1 a line of assembler
}

TypeBlockBox::TypeBlockBox(const blocks::BlockCatalog& catalog, QWidget* parent) : QFrame(parent), catalog_(catalog) {
    setObjectName("typeblockBox");
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(6, 6, 6, 6);
    l->setSpacing(4);
    edit_ = new QLineEdit(this);
    edit_->setObjectName("typeblockEdit");
    edit_->setPlaceholderText(tr("Type a block name or a line of assembler, Enter to add it"));
    list_ = new QListWidget(this);
    list_->setObjectName("typeblockList");
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setUniformItemSizes(true);
    l->addWidget(edit_);
    l->addWidget(list_);
    edit_->installEventFilter(this);
    connect(edit_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(list_, &QListWidget::itemActivated, this, [this] { finish(true); });
    connect(list_, &QListWidget::itemClicked, this, [this] { finish(true); });
    hide();
}

void TypeBlockBox::open(const QRect& within, const QString& initialText) {
    within_ = within;
    open_ = true;
    const int w = std::min(520, std::max(280, within.width() - 40));
    setGeometry(within.x() + (within.width() - w) / 2, within.bottom() - 250, w, 240);
    edit_->setText(initialText);
    refresh();
    show();
    raise();
    edit_->setFocus();
    edit_->setCursorPosition(edit_->text().size());
}

int TypeBlockBox::resultCount() const { return list_->count(); }

void TypeBlockBox::refresh() {
    list_->clear();
    const std::string text = edit_->text().toUtf8().toStdString();
    if (text.empty()) return;
    if (catalog_.looksLikeAssembler(text)) {
        auto* it = new QListWidgetItem(tr("Insert as assembler:  %1").arg(edit_->text()), list_);
        it->setData(kKind, 1);
    }
    for (const CatalogEntry* e : catalog_.search(text, 8)) {
        QString title = e->pattern() ? QCoreApplication::translate("BlockPattern", e->title.c_str()) : QCoreApplication::translate("BlockTitle", e->title.c_str());
        QString where = QCoreApplication::translate("BlockDrawer", e->category.c_str());
        if (e->domains == asmcore::blocks::kMain && !e->component()) where += " \xC2\xB7 Main";
        else if (e->domains == asmcore::blocks::kPeri && !e->component()) where += " \xC2\xB7 Peri";
        auto* it = new QListWidgetItem(title + "    \xE2\x80\x94  " + where, list_);
        it->setData(kEntry, QString::fromStdString(e->id));
        it->setData(kKind, 0);
        it->setToolTip(e->text.empty() ? QString::fromStdString(e->description) : QString::fromStdString(e->text));
    }
    if (list_->count()) list_->setCurrentRow(0);
}

void TypeBlockBox::finish(bool take) {
    if (!open_) return;
    QListWidgetItem* it = list_->currentItem();
    const QString text = edit_->text();
    open_ = false;
    hide();
    if (take && it) {
        if (it->data(kKind).toInt() == 1) emit chosenAssembler(text);
        else emit chosenEntry(it->data(kEntry).toString());
    } else if (take && !text.trimmed().isEmpty()) {
        emit chosenAssembler(text);                                    // nothing matched: what was typed is taken as assembler text (kept as text if not understood)
    }
    emit closed();
}

bool TypeBlockBox::eventFilter(QObject* o, QEvent* e) {
    if (o == edit_ && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        switch (k->key()) {
            case Qt::Key_Escape: finish(false); return true;
            case Qt::Key_Return: case Qt::Key_Enter: finish(true); return true;
            case Qt::Key_Down: if (list_->count()) list_->setCurrentRow(std::min(list_->count() - 1, list_->currentRow() + 1)); return true;
            case Qt::Key_Up: if (list_->count()) list_->setCurrentRow(std::max(0, list_->currentRow() - 1)); return true;
            case Qt::Key_Tab: if (list_->count()) list_->setCurrentRow((list_->currentRow() + 1) % list_->count()); return true;
            default: break;
        }
    } else if (o == edit_ && e->type() == QEvent::FocusOut && open_) {
        // the list does not take the focus; losing it elsewhere closes the box without adding anything
        finish(false);
    }
    return QFrame::eventFilter(o, e);
}

}  // namespace acide
