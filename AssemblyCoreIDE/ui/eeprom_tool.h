#pragma once
// The EEPROM tool: the program store of the two decoders as a window with three pages.
//   Build      the EEPROM image of the newest build: where its six regions go, written as PREFIX.eeprom.bin / .hex / .json (what is burned)
//   Verify     a burned image against the program: every byte of the six regions, then every instruction as the decoders read it
//   Disassemble a burned image back to assembler source (both domains), shown, saved as .asm and opened in the editor
// One window per AppContext; the menu actions open it at a page.
#include <QWidget>

#include "app_context.h"

namespace acide {

enum class EepromPage { Build = 0, Verify = 1, Disassemble = 2 };
QWidget* showEepromTool(AppContext& ctx, QWidget* parent, EepromPage page);
QWidget* eepromToolWindow(AppContext& ctx);                  // the window if it was made (tests)
// The disassembly page's text written to `path` and opened in the editor (what "Save as .asm and open it..." does after its file dialog)
bool saveEepromDisassembly(AppContext& ctx, const std::string& path);

}  // namespace acide
