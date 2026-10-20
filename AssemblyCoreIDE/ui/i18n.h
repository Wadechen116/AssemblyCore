#pragma once
// User-interface language. English is the language of the source strings; other languages are Qt translations (translations/*.ts, compiled
// into translations/*.qm and embedded as resources). The assembler's own diagnostics and the simulator's messages are not part of this: they
// stay in English.
#include <string>
#include <vector>

#include <QString>

namespace acide {

struct LanguageInfo {
    std::string code;                 // "en", "zh_TW"
    QString nativeName;               // shown in the language menu, in that language
};

std::vector<LanguageInfo> availableLanguages();

// The setting is "auto" (follow the system) or one of the codes. Returns a code from availableLanguages(): an unknown setting, and a system
// language that has no translation, give "en".
std::string resolveLanguage(const std::string& setting);

// Installs the Qt translators of `code` (and Qt's own, for the standard dialogs), replacing the ones installed before. "en" installs none.
// Returns false if the translation of a non-English language cannot be loaded (the interface then stays English).
bool installLanguage(const std::string& code);
std::string currentLanguage();

}  // namespace acide
