# AssemblyCoreIDE 架構

更新日期：2026-11-20（對照程式碼核對）。

目標：Windows／Linux／macOS 三平台；核心與服務層為標準 C++17（不含 Qt）；只有 `ui/` 使用 Qt Widgets；CMake 與 QMake 都能建置；
每一層可單獨測試、可替換。

## 分層

```
 ui/            Qt Widgets：視窗、編輯器、面板、分頁。只讀模型、呼叫服務，不含組譯／模擬邏輯
   │  (只依賴下層的介面，永不反向)
 services/      標準 C++17：Workspace、BuildService、SimSession、Debugger、Settings…  → 靜態庫 acide_services，無 Qt
   │  services/blocks/   積木畫布的幾何、拖放、目錄、主題（同樣無 Qt）
   │
 ../core        asmcore：assemble / Simulator / disassemble / lang / index / blocks / hardware / eeprom / sim_params
```

| 規則 | 目的 |
|---|---|
| 下層不認識上層；`services/` 不 `#include` 任何 Qt 標頭 | 服務層能在無視窗、無 Qt 的環境測試，也可以日後重用於 LSP／CLI 工具 |
| UI 與服務之間用 `Signal`（觀察者）與純資料結構溝通；UI 元件之間只經由 `AppContext` 上的訊號，不直接互相呼叫 | 換掉 UI 工具箱不動服務層；任何一個元件都可以單獨拿掉 |
| 跨執行緒只經由 `IDispatcher::post()` | 背景工作緒的結果由 UI 緒取走；服務層不知道 Qt 的事件迴圈 |
| 檔案存取只經由 `IFileSystem` | 測試用記憶體檔案系統；將來可接遠端或虛擬檔案 |
| 模擬器只被 `SimSession` 擁有（單一工作緒、命令佇列）；UI 經由 `Debugger` 操作 | `Simulator` 不是執行緒安全的；UI 永遠看到快照 |
| 面板是外掛：一個 `PanelSpec`（id、標題、停靠位置、建立函式）加進 `PanelRegistry` | 加一個面板不必改 `MainWindow` |
| 非純文字的分頁是外掛：實作 `DocumentTab`，以副檔名在 `TabRegistry` 註冊（`TabKind`） | 加一種文件（`.achw`、`.acblk`…）不必改視窗程式 |
| 建置快照 `BuildSnapshot` 不可變、以 `shared_ptr` 傳遞 | 面板各自持有舊快照也安全；新快照到來才換 |
| 設定／專案檔／參數檔是簡單的 `key = value` 文字；不認得的鍵保留 | 可讀、可進版本控制、可手改；舊版 IDE 開新檔不會弄丟內容 |
| 新的模擬行為做成參數，預設不改變既有結果 | 相容是硬性要求 |

## 服務層元件（`services/`）

基礎：

| 元件 | 責任 |
|---|---|
| `Signal<...>`、`ConnectionBag`（`signal.h`） | 單緒觀察者，連線以 id 管理；`ConnectionBag` 在解構時斷開。訊號的發送方法叫 `fire`（`emit` 是 Qt 的巨集） |
| `IDispatcher`（`dispatcher.h`） | 把函式丟回 UI 緒；`ImmediateDispatcher`（同步）、`QueueDispatcher`（測試用，可手動抽取）；Qt 版在 `ui/qt_dispatcher.*` |
| `IFileSystem`（`fs.h`） | 讀寫、列舉；`RealFileSystem`、`MemoryFileSystem`。IDE 內部路徑一律用 `/` |
| `Settings` | 使用者設定：扁平的鍵值（字型、主題、最近專案、介面語言、`blocks.*`、`isatop.layout`…），鍵由使用者自己擁有 |
| `Project` | `.acproj`：`source`、`include_dir`、`profile`、`environment`、`scenario`、`hardware`、`blocks`（舊名 `block` 仍可讀）、`simparams`、`debug_eeprom`、輸出位置（`output_dir`、`output_prefix`）、建置選項（`call_model`、`dataflow`、`suppress`、`werror`、`wcet`） |
| `Workspace` | 專案＋開啟的文件緩衝區（版本號、dirty）；產生建置輸入的快照（含未存檔文字，以及建置要用的硬體參數 `setBuildParams`；週期模型的參數改變時發 `buildParamsChanged`，版本號不動） |

建置與編輯：

| 元件 | 責任 |
|---|---|
| `BuildService`（`build.*`） | 背景工作緒；最新請求優先、過期結果丟棄；產生 `BuildSnapshot`（`Result`＋`Index`＋`SourceMap`＋每檔診斷）。組譯前把硬體參數中的週期模型蓋在 profile 上（與模擬器同一順序），`cycleOverrides` 記下被取代的數字；以核心的 `sim::assembleWithFetch` 組譯，讓 WCET 計入取指時間（`fetchModel` 記下算法） |
| `artifacts.*` | 把建置結果寫成檔案（`.bin`／`.hex`／`.lst`／`.map`／`.dbg.json`…，檔名與 `asmc` 相同） |
| `editing.*` | 補全、懸停、重新命名：`(文字, 語言, 索引)` 的純函式 |
| `auto_fix.*` | 自動修復：依診斷代碼套用「只有一種最可能修法」的修補，重建，直到乾淨或無法再修；可限定檔案／行／代碼 |
| `hardware_link.*` | 硬體設計器與工作區的橋接：`.achw` 與它產生的兩個檔案 |
| `block_link.*` | 積木程式（`.acblk`）與工作區的橋接：積木是事實來源，產生的組合語言檔是唯讀的 |

模擬與除錯：

| 元件 | 責任 |
|---|---|
| `SimSession` | 模擬器的工作緒＋命令佇列＋狀態快照；即時執行（依 `sim.speed` 切片）、取消、倒回；更新有回壓，UI 忙時不堆積 |
| `Debugger` | 使用者眼中的除錯：原始碼行、焦點 domain、執行／單步／繼續、中斷點、目前位置；在它與 `SimSession` 之間轉換。擁有硬體參數，值改變時同時送給執行中的模擬器與 `Workspace`（給建置） |
| `SimParamsStore` | IDE 持有的硬體參數：工作中的值、所屬的 `.simparams` 檔、與預設的差異 |
| `instr_params.*` | 一道指令牽涉哪些參數（積木與架構圖方塊的右鍵選單用） |
| `TraceRing` | 除錯工作階段的近期歷史（兩個 sequencer 的指令與其間的事件），供時間軸與倒回 |
| `sim_compare.*` | 在獨立的模擬器裡跑一段、留下軌跡，並比較兩份軌跡（基準線、標記、連結、統計） |
| `exec_values.*` | 一道指令剛做了什麼的簡短文字（積木上的執行值） |
| `auto_debug.*` | 自動除錯：在獨立的模擬器全速跑一段，找出 fault、overrun、空轉、I2C 無回應等 |
| `isa_layout.*` | 架構圖（ISA Top）的**資料**：方塊、方塊內的列、連線、每種指令點亮哪些線與方塊（`touch`）；預設內建，可由設定 `isatop.layout` 換成檔案 |

積木（`services/blocks/`，不含 Qt、不繪圖）：

| 元件 | 責任 |
|---|---|
| `block_session` | 一份編輯中的積木程式：文件、編輯歷史、產生的文字、選取（畫布與模型之間的 presenter） |
| `block_layout` | 幾何：每個積木、欄位、插槽、接點的位置；文字寬度經由 `ITextMetrics` 取得 |
| `block_shapes`、`block_theme` | 各種積木的外形與顏色表（純數字） |
| `block_drop` | 拖曳中的積木可以放在哪裡（以試做的方式問編輯規則） |
| `block_catalog` | 工具箱的抽屜、樣本、Patterns、自訂積木與搜尋 |
| `block_tidy`、`block_spring` | 整理版面（不改變程式）；彈簧動畫的數學 |

## UI 層（`ui/`）

`MainWindow` 只負責版面、選單、分頁與建置迴圈；功能都在面板、分頁與編輯器裡。每個元件由 `AppContext` 取得服務與共用訊號。

| 部分 | 內容 |
|---|---|
| `AppShell`、`main.cpp` | `AppShell` 持有主視窗，語言改變時重建；`main.cpp` 註冊面板（`registerBuiltinPanels`）；分頁種類在 `document_tab.cpp` 註冊 |
| `CodeEditor` | `QPlainTextEdit` 自製：行號、上色（`SyntaxHighlighter`）、診斷波浪線與邊欄圖示、括號配對、補全、中斷點與執行位置、Main／Peri 游標、右鍵 Fix、產生檔的唯讀標示。**沒有**折疊與書籤 |
| 停靠面板（`panels.cpp`） | `project`、`outline`、`problems`、`output`、`references`、`listing` |
| 除錯面板（`debug_panels.cpp`） | `state`、`memory`、`signals`、`csrs`、`watch`、`callstack`、`breakpoints`、`events`、`decoder`、`sram`、`autodebug` |
| 分頁（`DocumentTab`） | 硬體設計器（`hardware_designer.*`）、積木編輯器（`block_editor.*`）、模擬器頁面（`sim_panels.*` 的 `SimulatorTab`：架構圖 `isa_top_view.*`＋執行時間軸） |
| 積木畫布 | `block_scene`（畫布與拖放）、`block_view`（縮放、平移）、`block_painter`（繪圖）、`block_toolbox`、`typeblock_box`、`field_editor`、`comment_edit`、`generated_code_pane`、`custom_block_dialog` |
| 其他 | 狀態列的就緒等級標示與說明視窗（`MainWindow::showReadiness`，資料來自 `BuildSnapshot::readiness`）、`cursor_bar`（Main／Peri 雙游標工具列）、`eeprom_tool`（EEPROM 視窗）、`find_bar`、`theme`、`templates`、`i18n` |

主題用 token 名稱（`lang::tokName`）對應顏色，可由設定檔覆寫，不寫死在程式裡。

## 擴充點

| 想加的東西 | 做法 |
|---|---|
| 一個停靠面板 | 寫一個 `QWidget`，在 `registerBuiltinPanels`／`registerDebugPanels` 加一個 `PanelSpec`；標題用 `QT_TRANSLATE_NOOP` 的原文，建立停靠視窗時才翻譯 |
| 一種文件分頁 | 實作 `DocumentTab`，在 `ui/document_tab.cpp` 以副檔名向 `TabRegistry` 加一個 `TabKind`（目前有 `.achw`、`.acblk`）。模擬器頁面不對應檔案，由 `MainWindow::openSimulator` 開啟 |
| 架構圖的方塊、連線、哪道指令點亮什麼 | 改 `services/isa_layout.cpp` 的內建佈局文字，或用設定 `isatop.layout` 指到自己的檔案；不必改繪圖程式 |
| 一條自動修復規則 | `services/auto_fix.cpp`，以診斷代碼為鍵 |
| 一項自動除錯的發現 | `services/auto_debug.cpp` |
| 一個硬體參數 | 在核心的 `sim_params.cpp` 加（見 `../../docs/EXTENDING.md`）；IDE 的參數介面由目錄產生。要讓它出現在某個方塊或某道指令的右鍵選單，再改佈局的 `params=` 或 `instr_params.cpp` |
| 一種積木 | 在核心的積木定義加（見 `../../docs/EXTENDING.md`）；外形在 `block_shapes`，譯名在 `ui/block_texts.cpp` |
| 一種介面語言 | `ui/translations/README.md` |

## 介面語言

介面字串全部用 `tr()`（或 `QT_TRANSLATE_NOOP`）寫成英文，翻譯放在 `ui/translations/`（`.ts` 來源、`.qm` 內嵌）。
`ui/i18n.*` 安裝／切換翻譯器；`AppShell` 持有主視窗，語言改變時重建視窗（每個字串在建構時翻譯，重建最可靠）。
服務層與核心**不含**翻譯：它們的訊息（組譯診斷、模擬器停止原因）保持英文。

## 建置

- CMake：頂層 `ASMCORE_BUILD_IDE`（預設 ON）；找不到 Qt6 時只建服務層與其測試。
- QMake：`AssemblyCoreIDE.pro` 與 `sources.pri`（檔案清單）；`tests/check_build_files.py` 檢查兩邊的清單一致。
- 平台差異：`services/` 與 `ui/` 沒有任何 `#ifdef _WIN32`／`Q_OS_*`；唯一的編譯器條件是 `ui/msvc_stdext_compat.h`（VS 2026 的 STL 配 Qt 6.5 所需）。原先規劃的 `ui/platform/` 目錄並不存在，目前也不需要。
- 工具：`tools/md2pdf.cpp`（Markdown 轉 PDF，只用 Qt）。
- 測試：`tests/services_tests.cpp`、`tests/blocks_tests.cpp`（無 Qt）、`tests/ui_tests.cpp`（Qt Test，offscreen）。
