#include "theme.h"

#include <QFontDatabase>
#include <QStringList>

namespace acide {

namespace {

QTextCharFormat fmt(const char* color, bool bold = false, bool italic = false) {
    QTextCharFormat f;
    f.setForeground(QColor(color));
    if (bold) f.setFontWeight(QFont::Bold);
    f.setFontItalic(italic);
    return f;
}

}  // namespace

Theme Theme::dark() {
    Theme t;
    t.name = "dark";
    t.background = QColor("#1e1e1e"); t.foreground = QColor("#d4d4d4"); t.currentLine = QColor("#2a2d2e");
    t.lineNumberBg = QColor("#252526"); t.lineNumberFg = QColor("#858585"); t.selection = QColor("#264f78");
    t.error = QColor("#f14c4c"); t.warning = QColor("#cca700"); t.bracketMatch = QColor("#515c6a");
    t.execMain = QColor("#4d4a1c"); t.execPeri = QColor("#1c4a4d"); t.breakpoint = QColor("#e51400");
    t.tokens["comment"] = fmt("#6a9955", false, true);
    t.tokens["label"] = fmt("#dcdcaa", true);
    t.tokens["directive"] = fmt("#c586c0");
    t.tokens["mnemonic"] = fmt("#569cd6", true);
    t.tokens["subop"] = fmt("#4ec9b0");
    t.tokens["region"] = fmt("#9cdcfe");
    t.tokens["condition"] = fmt("#ce9178");
    t.tokens["argname"] = fmt("#4fc1ff");
    t.tokens["number"] = fmt("#b5cea8");
    t.tokens["string"] = fmt("#ce9178");
    t.tokens["symbol"] = fmt("#d4d4d4");
    t.tokens["operator"] = fmt("#d4d4d4");
    t.tokens["unknown"] = fmt("#f48771");
    return t;
}

Theme Theme::light() {
    Theme t;
    t.name = "light";
    t.background = QColor("#ffffff"); t.foreground = QColor("#1f1f1f"); t.currentLine = QColor("#f3f3f3");
    t.lineNumberBg = QColor("#f0f0f0"); t.lineNumberFg = QColor("#6e7681"); t.selection = QColor("#add6ff");
    t.error = QColor("#e51400"); t.warning = QColor("#bf8803"); t.bracketMatch = QColor("#c8dcf0");
    t.execMain = QColor("#fff1a8"); t.execPeri = QColor("#bfeff2"); t.breakpoint = QColor("#e51400");
    t.tokens["comment"] = fmt("#008000", false, true);
    t.tokens["label"] = fmt("#795e26", true);
    t.tokens["directive"] = fmt("#af00db");
    t.tokens["mnemonic"] = fmt("#0000ff", true);
    t.tokens["subop"] = fmt("#267f99");
    t.tokens["region"] = fmt("#001080");
    t.tokens["condition"] = fmt("#a31515");
    t.tokens["argname"] = fmt("#0070c1");
    t.tokens["number"] = fmt("#098658");
    t.tokens["string"] = fmt("#a31515");
    t.tokens["symbol"] = fmt("#1f1f1f");
    t.tokens["operator"] = fmt("#1f1f1f");
    t.tokens["unknown"] = fmt("#e51400");
    return t;
}

Theme Theme::fromSettings(const Settings& s) {
    Theme t = s.get("theme", "dark") == "light" ? light() : dark();
    auto color = [&](const char* key, QColor& c) {
        std::string v = s.get(std::string("theme.") + key);
        if (!v.empty()) c = QColor(QString::fromStdString(v));
    };
    color("background", t.background); color("foreground", t.foreground); color("currentLine", t.currentLine);
    color("lineNumberBg", t.lineNumberBg); color("lineNumberFg", t.lineNumberFg); color("selection", t.selection);
    color("error", t.error); color("warning", t.warning); color("bracketMatch", t.bracketMatch);
    color("execMain", t.execMain); color("execPeri", t.execPeri); color("breakpoint", t.breakpoint);
    for (auto it = t.tokens.begin(); it != t.tokens.end(); ++it) {
        std::string v = s.get("theme.token." + it.key().toStdString());
        if (v.empty()) continue;
        QStringList parts = QString::fromStdString(v).split(',', Qt::SkipEmptyParts);
        QTextCharFormat f;
        f.setForeground(QColor(parts.value(0).trimmed()));
        f.setFontWeight(parts.contains("bold") ? QFont::Bold : QFont::Normal);
        f.setFontItalic(parts.contains("italic"));
        it.value() = f;
    }
    return t;
}

QFont editorFont(const Settings& s) {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    std::string family = s.get("editor.font");
    if (!family.empty()) f.setFamily(QString::fromStdString(family));
    f.setPointSize(s.getInt("editor.fontSize", 11));
    f.setStyleHint(QFont::Monospace);
    return f;
}

}  // namespace acide
