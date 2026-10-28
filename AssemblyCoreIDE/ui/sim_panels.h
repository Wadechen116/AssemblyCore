#pragma once
// The simulator page: the parameters, the execution (and, next, the ISA Top diagram) side by side in ONE document tab, so a parameter can be changed
// and its effect seen at once; and the quick access to the parameters from anywhere (a search box and the pinned ones, in a popup).
#include <functional>
#include <string>

#include <QWidget>

#include "app_context.h"

namespace acide {

// The simulator tab (it implements DocumentTab). One per window; its path is "simulator:".
QWidget* createSimulatorTab(AppContext& ctx);
// Brings a parameter into view in the simulator tab, selects its row and starts editing its value.
void showParameterInTab(QWidget* simulatorTab, const std::string& key);
// Edit > Find while the simulator page is shown: the cursor goes to the parameter search box.
void focusParameterSearch(QWidget* simulatorTab);
// The popup under `anchor`: a search box over every parameter and the pinned ones, each with its editor. `showInTab` is called with the key of the
// parameter when its arrow is pressed (the caller opens the simulator tab and shows it).
void showQuickParams(AppContext& ctx, QWidget* anchor, std::function<void(const std::string&)> showInTab);
// The text for "N changed" in the status bar and the toolbar.
QString simParamsSummary(AppContext& ctx);

}  // namespace acide
