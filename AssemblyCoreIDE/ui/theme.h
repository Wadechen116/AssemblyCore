#pragma once
// Colours and fonts, keyed by the token names of the language service (lang::tokName), so a theme is just a table that can come from a
// settings file. Nothing in the editor hard-codes a colour.
#include <QColor>
#include <QFont>
#include <QMap>
#include <QString>
#include <QTextCharFormat>

#include "settings.h"

namespace acide {

struct Theme {
    QString name;
    QColor background, foreground, currentLine, lineNumberBg, lineNumberFg, selection, error, warning, bracketMatch;
    QColor execMain, execPeri, breakpoint;           // the line a sequencer is about to execute; a breakpoint mark
    QMap<QString, QTextCharFormat> tokens;           // by lang::tokName
    QTextCharFormat format(const QString& tok) const { return tokens.value(tok); }

    static Theme dark();
    static Theme light();
    // Starts from the named base theme and applies "theme.token.<name> = #rrggbb[,bold][,italic]" and "theme.<colour> = #rrggbb" overrides.
    static Theme fromSettings(const Settings& s);
};

QFont editorFont(const Settings& s);

}  // namespace acide
