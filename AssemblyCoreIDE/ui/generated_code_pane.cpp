#include "generated_code_pane.h"

#include <QMouseEvent>
#include <QScrollBar>
#include <QTextBlock>

namespace acide {

GeneratedCodePane::GeneratedCodePane(std::shared_ptr<const asmcore::lang::Language> lang, const Theme& theme, const QFont& font, QWidget* parent)
    : QPlainTextEdit(parent), theme_(theme) {
    setReadOnly(true);
    setFont(font);
    setLineWrapMode(NoWrap);
    setObjectName("generatedCode");
    hl_ = new SyntaxHighlighter(document(), std::move(lang), theme);
    setTheme(theme);
}

void GeneratedCodePane::setTheme(const Theme& t) {
    theme_ = t;
    QPalette p = palette();
    p.setColor(QPalette::Base, t.background);
    p.setColor(QPalette::Text, t.foreground);
    setPalette(p);
    hl_->setTheme(t);
    applyMarks();
}

void GeneratedCodePane::setCode(const QString& text) {
    if (toPlainText() == text) return;
    const int v = verticalScrollBar()->value();
    setPlainText(text);
    verticalScrollBar()->setValue(v);
    applyMarks();
}

void GeneratedCodePane::setMarkedLines(const std::vector<int>& lines, bool scrollToFirst) {
    marked_ = lines;
    applyMarks();
    if (scrollToFirst && !lines.empty()) {
        QTextCursor c(document()->findBlockByNumber(lines.front() - 1));
        if (c.isNull()) return;
        const QTextCursor keep = textCursor();
        setTextCursor(c);
        centerCursor();
        (void)keep;
    }
}

void GeneratedCodePane::applyMarks() {
    QList<QTextEdit::ExtraSelection> sel;
    QColor c = theme_.selection;
    c.setAlpha(90);
    for (int line : marked_) {
        QTextBlock b = document()->findBlockByNumber(line - 1);
        if (!b.isValid()) continue;
        QTextEdit::ExtraSelection s;
        s.format.setBackground(c);
        s.format.setProperty(QTextFormat::FullWidthSelection, true);
        s.cursor = QTextCursor(b);
        sel.append(s);
    }
    setExtraSelections(sel);
}

void GeneratedCodePane::mouseReleaseEvent(QMouseEvent* e) {
    QPlainTextEdit::mouseReleaseEvent(e);
    if (e->button() == Qt::LeftButton && !textCursor().hasSelection()) emit lineClicked(lineAtCursor());
}

}  // namespace acide
