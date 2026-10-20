#pragma once
#include <memory>
#include <set>

#include <QSyntaxHighlighter>

#include "asmcore/lang.h"
#include "theme.h"

namespace acide {

// Colours a document with the language service. The domain (.main / .peri) is carried from block to block in the block state.
class SyntaxHighlighter : public QSyntaxHighlighter {
public:
    SyntaxHighlighter(QTextDocument* doc, std::shared_ptr<const asmcore::lang::Language> language, const Theme& theme);
    void setTheme(const Theme& t);
    // Words that are defined somewhere; any other plain word is marked unknown. Null: do not mark.
    void setKnownSymbols(std::shared_ptr<const std::set<std::string>> known);

protected:
    void highlightBlock(const QString& text) override;

private:
    std::shared_ptr<const asmcore::lang::Language> lang_;
    Theme theme_;
    std::shared_ptr<const std::set<std::string>> known_;
};

}  // namespace acide
