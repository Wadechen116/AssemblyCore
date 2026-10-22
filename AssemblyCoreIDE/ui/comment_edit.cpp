#include "comment_edit.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QTextDocument>

namespace acide {

CommentEdit::CommentEdit(QWidget* parent) : QPlainTextEdit(parent) {
    setObjectName("commentEdit");
    setFrameShape(QFrame::NoFrame);
    setLineWrapMode(NoWrap);
    setTabChangesFocus(true);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setToolTip(tr("Enter keeps the comment, Shift+Enter starts a new line, Esc cancels"));
    hide();
    connect(this, &QPlainTextEdit::textChanged, this, [this] { if (open_) fit(); });
}

void CommentEdit::open(const QRect& where, const QString& text, const QFont& font, const QColor& bg, const QColor& fg, const QColor& border) {
    anchor_ = where;
    setFont(font);
    setStyleSheet(QString("QPlainTextEdit { background: %1; color: %2; border: 2px solid %3; border-radius: 6px; padding: 2px 4px; selection-background-color: %3; }")
                      .arg(bg.name(QColor::HexArgb), fg.name(), border.name()));
    open_ = true;
    setPlainText(text);
    fit();
    show();
    raise();
    setFocus();
    selectAll();
}

// as wide and as high as the text needs, from the end of the block it comments
void CommentEdit::fit() {
    const QFontMetrics fm(font());
    int w = 0;
    int lines = 1;
    for (const QString& l : toPlainText().split('\n')) { w = std::max(w, fm.horizontalAdvance(l)); }
    lines = std::max(1, (int)toPlainText().count('\n') + 1);
    const int width = std::max(anchor_.width(), w + 28), height = lines * fm.lineSpacing() + 14;
    setGeometry(anchor_.x(), anchor_.y() - std::max(0, (height - anchor_.height()) / 2), width, height);
}

void CommentEdit::finish(bool keep) {
    if (!open_) return;
    open_ = false;
    const QString t = toPlainText();
    hide();
    if (keep) emit committed(t); else emit cancelled();
}

void CommentEdit::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) { finish(false); e->accept(); return; }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) { finish(true); e->accept(); return; }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) { insertPlainText("\n"); e->accept(); return; }
    QPlainTextEdit::keyPressEvent(e);
}

void CommentEdit::focusOutEvent(QFocusEvent* e) {
    QPlainTextEdit::focusOutEvent(e);
    finish(true);
}

}  // namespace acide
