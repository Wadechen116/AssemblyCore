#pragma once
// The assembler text a block program generates, as a read-only page with the usual syntax colours. The lines of the selected blocks are
// marked; clicking a line reports it so the block editor can select the block that produced it.
#include <memory>
#include <vector>

#include <QPlainTextEdit>

#include "asmcore/lang.h"
#include "syntax_highlighter.h"
#include "theme.h"

namespace acide {

class GeneratedCodePane : public QPlainTextEdit {
    Q_OBJECT
public:
    GeneratedCodePane(std::shared_ptr<const asmcore::lang::Language> lang, const Theme& theme, const QFont& font, QWidget* parent = nullptr);
    void setCode(const QString& text);                         // keeps the scroll position
    void setMarkedLines(const std::vector<int>& lines, bool scrollToFirst);   // 1-based
    void setTheme(const Theme& t);
    int lineAtCursor() const { return textCursor().blockNumber() + 1; }

signals:
    void lineClicked(int line);                                // 1-based

protected:
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    void applyMarks();
    SyntaxHighlighter* hl_;
    Theme theme_;
    std::vector<int> marked_;
};

}  // namespace acide
