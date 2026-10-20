#pragma once
// Panels are plug-ins: a PanelSpec says how to make one, and the main window docks whatever is registered. Adding a panel means
// adding one spec; MainWindow does not change. A panel wires itself to the AppContext signals it cares about.
#include <functional>
#include <vector>

#include <Qt>
#include <QString>

#include "app_context.h"

class QWidget;

namespace acide {

struct PanelSpec {
    QString id;                                       // stable key (settings, "View" menu)
    const char* title = "";                           // the source text (QT_TRANSLATE_NOOP("QObject", ...)): translated when the dock is created,
                                                      // so the registry can outlive a change of language
    Qt::DockWidgetArea area = Qt::BottomDockWidgetArea;
    bool visible = true;                              // shown on first start
    std::function<QWidget*(AppContext&)> create;
};

class PanelRegistry {
public:
    void add(const PanelSpec& s) { specs_.push_back(s); }
    const std::vector<PanelSpec>& specs() const { return specs_; }
private:
    std::vector<PanelSpec> specs_;
};

// The panels that ship with the IDE.
void registerBuiltinPanels(PanelRegistry& r);     // also registers the debugging panels
void registerDebugPanels(PanelRegistry& r);       // state, memory, signals, CSRs, watch, call stack, breakpoints, events (debug_panels.cpp)

}  // namespace acide
