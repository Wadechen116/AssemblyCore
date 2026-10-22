#pragma once
// A tab of the main window that is not the plain text editor (the Hardware Designer, the block editor ...). The main window knows only this
// interface: how to title it, which workspace files make it "modified", what to do before it closes. Which kind of tab opens a file is decided
// by TabRegistry (by extension), so adding a kind of document never touches the window code.
#include <functional>
#include <string>
#include <vector>

#include <QString>
#include <QWidget>

#include "app_context.h"

namespace acide {

class DocumentTab {
public:
    virtual ~DocumentTab() = default;
    virtual std::string tabPath() const = 0;                        // the file the tab was opened for
    virtual QString tabTitle() const = 0;                           // without the "modified" mark
    virtual std::vector<std::string> workspacePaths() const = 0;    // workspace buffers this tab edits (any of them dirty => the tab is modified)
    virtual void prepareToClose() {}                                // apply a pending edit
    virtual QWidget* widget() = 0;
};

struct TabKind {
    std::string extension;                                          // ".achw"
    std::function<QWidget*(AppContext&, const std::string& path)> create;   // the widget must implement DocumentTab
};

class TabRegistry {
public:
    static TabRegistry& instance();
    void add(const TabKind& k);
    const TabKind* find(const std::string& path) const;             // by the extension of `path`
    std::vector<std::string> extensions() const;

private:
    std::vector<TabKind> kinds_;
};

// Registers the built-in kinds (once).
void registerBuiltinTabs();

}  // namespace acide
