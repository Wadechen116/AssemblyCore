#include "syntax_highlighter.h"

#include "text_util.h"

namespace acide {

SyntaxHighlighter::SyntaxHighlighter(QTextDocument* doc, std::shared_ptr<const asmcore::lang::Language> language, const Theme& theme)
    : QSyntaxHighlighter(doc), lang_(std::move(language)), theme_(theme) {}

void SyntaxHighlighter::setTheme(const Theme& t) {
    theme_ = t;
    rehighlight();
}

void SyntaxHighlighter::setKnownSymbols(std::shared_ptr<const std::set<std::string>> known) {
    known_ = std::move(known);
    rehighlight();
}

void SyntaxHighlighter::highlightBlock(const QString& text) {
    using asmcore::lang::Dom;
    int prev = previousBlockState();
    Dom dom = prev == 1 ? Dom::Main : prev == 2 ? Dom::Peri : Dom::None;
    std::string line = toStd(text);
    const bool ascii = line.size() == (size_t)text.size();
    for (const asmcore::lang::Token& t : lang_->tokenizeLine(line, dom, known_.get())) {
        if (t.kind == asmcore::lang::Tok::Whitespace) continue;
        QTextCharFormat f = theme_.format(QString::fromLatin1(asmcore::lang::tokName(t.kind)));
        if (f.properties().isEmpty()) continue;
        int b = t.col, e = t.col + t.len;
        if (!ascii) { b = charFromByte(text, b); e = charFromByte(text, e); }
        setFormat(b, e - b, f);
    }
    setCurrentBlockState(dom == Dom::Main ? 1 : dom == Dom::Peri ? 2 : 0);
}

}  // namespace acide
