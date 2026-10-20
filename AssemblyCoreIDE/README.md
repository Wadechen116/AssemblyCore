# AssemblyCoreIDE

AssemblyCore 組譯器的整合開發環境（Qt Widgets）。IDE 只呼叫 `asmcore`（`assemble`、`Index`、`SourceMap`、日後的 `Simulator`），不重複任何組譯邏輯。

- 架構與分層規則：`docs/ARCHITECTURE.md`
- 計畫、進度與缺項：`docs/IDE_PLAN.md`
- 模擬與除錯的使用方式：`docs/DEBUGGING.md`
- 介面語言（English／繁體中文）與如何更新翻譯：`ui/translations/README.md`
- 服務層（`services/`，標準 C++17、無 Qt）與 UI 層（`ui/`，Qt Widgets）；CMake 與 QMake 皆可建置
