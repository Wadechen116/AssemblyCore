#include "app_shell.h"

#include <QTimer>

#include "i18n.h"
#include "text_util.h"

namespace acide {

AppShell::AppShell(AppContext& ctx, PanelRegistry& panels, QObject* parent) : QObject(parent), ctx_(ctx), panels_(panels) {
    // requested from a menu item of the window that is about to be replaced: do it once that handler has returned
    conns_.add(ctx.languageRequested, [this](const std::string& setting) {
        QTimer::singleShot(0, this, [this, setting] { setLanguage(setting); });
    });
    create(std::string());
}

AppShell::~AppShell() { delete window_; }

void AppShell::create(const std::string& projectFile) {
    window_ = new MainWindow(ctx_, panels_);
    window_->setQuiet(quiet_);
    window_->show();
    if (!projectFile.empty()) window_->openProject(fromStd(projectFile));
}

void AppShell::show() { if (window_) window_->show(); }

void AppShell::setQuiet(bool q) {
    quiet_ = q;
    if (window_) window_->setQuiet(q);
}

bool AppShell::setLanguage(const std::string& setting) {
    if (setting == ctx_.settings.get("ui.language", "auto") && window_) return true;
    const std::string resolved = resolveLanguage(setting);
    if (window_ && !window_->prepareForRebuild()) {                     // the user cancelled at "save changes?"
        window_->refreshLanguageMenu();
        return false;
    }
    if (!installLanguage(resolved)) {
        if (window_) window_->refreshLanguageMenu();
        return false;
    }
    ctx_.settings.set("ui.language", setting);
    const std::string project = ctx_.workspace.hasProject() ? ctx_.workspace.project().file : std::string();
    MainWindow* old = window_;
    window_ = nullptr;
    if (old) { old->hide(); old->deleteLater(); }
    create(project);
    return true;
}

}  // namespace acide
