#include "i18n.h"

#include <memory>

#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>

// The translations are compiled into the program (translations/translations.qrc). Initialised by hand because they live in a static library.
static void initTranslationResources() { Q_INIT_RESOURCE(translations); }

namespace acide {

namespace {
std::unique_ptr<QTranslator> g_app, g_qt;
std::string g_current = "en";
}  // namespace

std::vector<LanguageInfo> availableLanguages() {
    return {{"en", QStringLiteral("English")}, {"zh_TW", QString::fromUtf8("\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87")}};     // 繁體中文
}

std::string resolveLanguage(const std::string& setting) {
    std::string s = setting;
    if (s == "auto" || s.empty()) {
        QLocale l = QLocale::system();
        bool traditional = l.language() == QLocale::Chinese &&
                           (l.script() == QLocale::TraditionalChineseScript || l.country() == QLocale::Taiwan || l.country() == QLocale::HongKong || l.country() == QLocale::Macao);
        return traditional ? "zh_TW" : "en";
    }
    for (const LanguageInfo& i : availableLanguages()) if (i.code == s) return s;
    return "en";
}

bool installLanguage(const std::string& code) {
    initTranslationResources();
    if (g_app) { QCoreApplication::removeTranslator(g_app.get()); g_app.reset(); }
    if (g_qt) { QCoreApplication::removeTranslator(g_qt.get()); g_qt.reset(); }
    g_current = "en";
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    if (code == "en") return true;
    const QString c = QString::fromStdString(code);
    auto app = std::make_unique<QTranslator>();
    if (!app->load(":/i18n/acide_" + c + ".qm")) return false;
    QCoreApplication::installTranslator(app.get());
    g_app = std::move(app);
    auto qt = std::make_unique<QTranslator>();                       // Qt's own texts (file dialog buttons, message box buttons...)
    if (qt->load(":/i18n/qtbase_" + c + ".qm")) { QCoreApplication::installTranslator(qt.get()); g_qt = std::move(qt); }
    QLocale::setDefault(QLocale(c));
    g_current = code;
    return true;
}

std::string currentLanguage() { return g_current; }

}  // namespace acide
