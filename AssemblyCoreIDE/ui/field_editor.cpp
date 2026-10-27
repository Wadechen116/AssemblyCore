#include "field_editor.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTimer>
#include <QToolTip>

namespace acide {

FieldEditor::FieldEditor(QWidget* parent) : QComboBox(parent) {
    setObjectName("fieldEditor");
    setInsertPolicy(QComboBox::NoInsert);
    setMaxVisibleItems(14);
    connect(this, QOverload<int>::of(&QComboBox::activated), this, [this] { if (open_) finish(true, 0); });      // a choice from the list is the answer
    hide();
}

void FieldEditor::open(const QRect& where, const QString& text, const QStringList& items, bool editable, const QFont& font, const QColor& bg, const QColor& fg, const QColor& border,
                       std::function<QString(const QString&)> hint) {
    hint_ = std::move(hint);
    border_ = border;
    editable_ = editable;
    open_ = true;
    clear();
    addItems(items);
    setEditable(editable);
    setFont(font);
    setStyleSheet(QString("QComboBox { background: %1; color: %2; border: 2px solid %3; border-radius: 5px; padding: 0 2px 0 4px; }"
                          "QComboBox QAbstractItemView { background: %1; color: %2; selection-background-color: %3; }")
                      .arg(bg.name(QColor::HexArgb), fg.name(), border.name()));
    const QFontMetrics fm(font);
    int w = fm.horizontalAdvance(text) + 46;
    for (const QString& it : items) w = std::max(w, fm.horizontalAdvance(it) + 46);
    setGeometry(where.x(), where.y() - 2, std::max(where.width() + 20, std::min(w, 360)), where.height() + 4);
    if (editable) {
        setCurrentIndex(-1);
        lineEdit()->setText(text);
        lineEdit()->installEventFilter(this);
        auto* c = new QCompleter(items, lineEdit());
        c->setCaseSensitivity(Qt::CaseInsensitive);
        c->setCompletionMode(QCompleter::InlineCompletion);                 // the rest of a name is filled in as you type (selected); Enter keeps what is there, the arrow lists everything
        setCompleter(c);
        connect(lineEdit(), &QLineEdit::textChanged, this, &FieldEditor::showHint, Qt::UniqueConnection);
        lineEdit()->selectAll();
    } else {
        const int i = findText(text, Qt::MatchFixedString);
        setCurrentIndex(i < 0 ? 0 : i);
        installEventFilter(this);
    }
    show();
    raise();
    setFocus();
    if (!editable || !items.isEmpty()) QTimer::singleShot(0, this, [this] { if (open_ && !editable_) showPopup(); });
    showHint(text);
}

void FieldEditor::hidePopup() {
    QComboBox::hidePopup();
    if (open_ && !editable_) QTimer::singleShot(0, this, [this] { if (open_) finish(true, 0); });       // (after `activated`, if there was a choice)
}

void FieldEditor::showHint(const QString& text) {
    if (!hint_ || !open_) return;
    const QString h = hint_(text);
    setToolTip(h);
    setStyleSheet(styleSheet().replace(QRegularExpression("border: 2px solid #[0-9a-fA-F]+"), "border: 2px solid " + (h.isEmpty() ? border_.name() : QStringLiteral("#e37400"))));
    if (!h.isEmpty()) QToolTip::showText(mapToGlobal(QPoint(0, height())), h, this);
    else QToolTip::hideText();
}

void FieldEditor::finish(bool keep, int navigate) {
    if (!open_) return;
    open_ = false;
    const QString t = text();
    hide();
    QToolTip::hideText();
    if (keep) emit committed(t, navigate); else emit cancelled();
}

bool FieldEditor::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        const bool popup = view() && view()->isVisible();
        if (k->key() == Qt::Key_Escape && !popup) { finish(false, 0); return true; }
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && !popup && !(completer() && completer()->popup() && completer()->popup()->isVisible())) { finish(true, 0); return true; }
        if (k->key() == Qt::Key_Tab) { finish(true, +1); return true; }
        if (k->key() == Qt::Key_Backtab) { finish(true, -1); return true; }
    } else if (e->type() == QEvent::FocusOut) {
        // the drop-down takes the focus while it is open: only a real loss of focus keeps the text
        QTimer::singleShot(0, this, [this] {
            const bool inside = hasFocus() || (lineEdit() && lineEdit()->hasFocus()) || (view() && view()->isVisible()) || (completer() && completer()->popup() && completer()->popup()->isVisible());
            if (open_ && !inside) finish(true, 0);
        });
    }
    return QComboBox::eventFilter(o, e);
}

}  // namespace acide
