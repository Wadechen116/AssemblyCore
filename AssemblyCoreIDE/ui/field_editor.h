#pragma once
// The editor that opens over a part of a block (a field chip, an operand, a number): one line of text, with a drop-down of what fits (the choices of the
// field, the names the definitions define) and completion as you type. Enter keeps it, Esc cancels, Tab / Shift+Tab keep it and go on to the next / previous part.
#include <functional>

#include <QComboBox>
#include <QElapsedTimer>

namespace acide {

class FieldEditor : public QComboBox {
    Q_OBJECT
public:
    explicit FieldEditor(QWidget* parent);
    // where: the part of the block, in the coordinates of the parent. editable false: only the items can be chosen.
    void open(const QRect& where, const QString& text, const QStringList& items, bool editable, const QFont& font, const QColor& bg, const QColor& fg, const QColor& border,
              std::function<QString(const QString&)> hint);
    bool isOpen() const { return open_; }
    void commit() { finish(true, 0); }                        // keep what is typed and close (a click elsewhere)
    void hidePopup() override;                                // a closed list (no typing) is finished when its drop-down closes, chosen or not
    QString text() const { return editable_ ? currentText() : itemText(currentIndex()); }

signals:
    void committed(const QString& text, int navigate);       // navigate: 0 stay, +1 next part, -1 previous part
    void cancelled();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void finish(bool keep, int navigate);
    void showPopupWhenReleased(int tries);
    bool strayClose();
    bool shown_ = false;
    QElapsedTimer shownClock_;
    void showHint(const QString& text);
    bool open_ = false, editable_ = true;
    std::function<QString(const QString&)> hint_;
    QColor border_;
};

}  // namespace acide
