#pragma once
// The simulator page: the ISA Top diagram above the time line of the run in ONE document tab. The parameters are set where they belong: the right button
// on a box of the diagram (its hardware parameters), on a block (its instruction's), all of them in the "All parameters" window; and the quick access from
// anywhere (a search box and the pinned ones, in a popup).
#include <functional>
#include <string>
#include <vector>

#include <QPoint>

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
// A small window with some parameters (the right button on a box of the ISA Top diagram, on a block): `own` under `ownLabel`, then `related` under
// `relatedLabel` (either label may be empty). It edits the debugger's parameters at once. Shown near `globalPos`; it closes itself.
QWidget* showParamSet(AppContext& ctx, QWidget* parent, const QString& title, const std::vector<std::string>& own, const std::vector<std::string>& related,
                      const QString& ownLabel, const QString& relatedLabel, const QPoint& globalPos);
// The window with every parameter (search, the parameter file, status and note, restart): one per AppContext, made on first use. With `key`: that row is
// shown and edited; `focusSearch`: the cursor goes to the search box.
QWidget* showAllParams(AppContext& ctx, QWidget* parent, const std::string& key = std::string(), bool focusSearch = false);
QWidget* allParamsWindow(AppContext& ctx);                     // the window if it was made (tests)
// The text for "N changed" in the status bar and the toolbar.
QString simParamsSummary(AppContext& ctx);

}  // namespace acide
