#pragma once
// The two cursors of the debugger, side by side in one bar: where MAIN is (file, line, the instruction it is about to execute) and where PERI is, each with its
// own Step buttons, the colours used everywhere else (Main amber, Peri cyan). The one Step acts on has a thick frame and "STEP". A click on a cursor makes it the
// one Step acts on and shows its line. The bar is only there while something is debugged.
#include <QToolBar>

#include "app_context.h"

namespace acide {

QToolBar* createCursorBar(AppContext& ctx, QWidget* parent);

}  // namespace acide
