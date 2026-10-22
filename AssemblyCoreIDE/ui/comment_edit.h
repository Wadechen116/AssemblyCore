#pragma once
// The little editor that opens on the canvas, over the end of a block, when its tail is double-clicked: the end-of-line comment as text, with as many lines
// as you like. Enter keeps it, Shift+Enter starts another line, Esc throws the change away; clicking elsewhere keeps it (a comment half typed is more
// often wanted than not).
#include <QPlainTextEdit>

namespace acide {

class CommentEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CommentEdit(QWidget* parent);
    void open(const QRect& where, const QString& text, const QFont& font, const QColor& bg, const QColor& fg, const QColor& border);
    bool isOpen() const { return open_; }

signals:
    void committed(const QString& text);                 // Enter, or the focus left
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    void fit();
    void finish(bool keep);
    bool open_ = false;
    QRect anchor_;
};

}  // namespace acide
