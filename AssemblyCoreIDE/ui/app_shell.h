#pragma once
// Owns the main window and recreates it when the language changes (every string of the interface is translated when it is built, so the
// simplest reliable way to switch is to build the window again). The project, the open files and the breakpoints come back; a running
// debug session ends.
#include <memory>

#include <QObject>

#include "main_window.h"

namespace acide {

class AppShell : public QObject {
    Q_OBJECT
public:
    AppShell(AppContext& ctx, PanelRegistry& panels, QObject* parent = nullptr);
    ~AppShell() override;

    MainWindow* window() const { return window_; }
    void show();
    void setQuiet(bool q);
    // `setting` is "auto" or a language code. Saves it, installs the translation and rebuilds the window. False if the user cancelled
    // (unsaved changes) or the translation could not be loaded.
    bool setLanguage(const std::string& setting);

private:
    void create(const std::string& projectFile);
    AppContext& ctx_;
    PanelRegistry& panels_;
    MainWindow* window_ = nullptr;
    bool quiet_ = false;
    ConnectionBag conns_;
};

}  // namespace acide
