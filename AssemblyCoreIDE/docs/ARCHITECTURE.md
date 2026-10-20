# AssemblyCoreIDE 架構

目標：Windows／Linux／macOS 三平台；核心與服務層為標準 C++17（不含 Qt）；只有 `ui/` 使用 Qt Widgets；CMake 與 QMake 都能建置；
每一層可單獨測試、可替換。

## 分層

```
 ui/            Qt Widgets：視窗、編輯器、面板。只讀模型、呼叫服務，不含組譯／模擬邏輯
   │  (只依賴下層的介面，永不反向)
 services/      標準 C++17：Workspace、BuildService、SimSession、Settings…  → 靜態庫 acide_services，無 Qt
   │
 ../core        asmcore：assemble / Simulator / disassemble / lang / index
```

| 規則 | 目的 |
|---|---|
| 下層不認識上層；`services/` 不 `#include` 任何 Qt 標頭 | 服務層能在無視窗、無 Qt 的環境測試，也可以日後重用於 LSP／CLI 工具 |
| UI 與服務之間用 `Signal`（觀察者）與純資料結構溝通 | 換掉 UI 工具箱不動服務層 |
| 跨執行緒只經由 `IDispatcher::post()` | 背景工作緒的結果由 UI 緒取走；服務層不知道 Qt 的事件迴圈 |
| 檔案存取只經由 `IFileSystem` | 測試用記憶體檔案系統；將來可接遠端或虛擬檔案 |
| 模擬器只被 `SimSession` 擁有（單一執行緒、命令佇列） | `Simulator` 不是執行緒安全的；UI 永遠看到快照 |
| 面板是外掛：實作 `IPanel`，在 `PanelRegistry` 註冊 | 加一個面板不必改 `MainWindow` |
| 建置快照 `BuildSnapshot` 不可變、以 `shared_ptr` 傳遞 | 面板各自持有舊快照也安全；新快照到來才換 |
| 設定／專案檔是簡單的 `key = value` 文字 | 可讀、可進版本控制、可手改 |

## 服務層元件（`services/`）

| 元件 | 責任 |
|---|---|
| `Signal<...>` | 單緒觀察者，連線以 id 管理 |
| `IDispatcher` | 把函式丟回 UI 緒；`ImmediateDispatcher`（同步）、`QueueDispatcher`（測試用，可手動抽取） |
| `IFileSystem` | 讀寫、列舉；`RealFileSystem`、`MemoryFileSystem` |
| `Project` | `.acproj`：來源、include 目錄、profile、環境檔、情境檔、建置選項；解析與序列化 |
| `Workspace` | 專案＋開啟的文件緩衝區（版本號、dirty）；產生建置輸入的快照（含未存檔文字） |
| `BuildService` | 背景工作緒；最新請求優先、過期結果丟棄；產生 `BuildSnapshot`（`Result`＋`Index`＋`SourceMap`＋每檔診斷） |
| `Settings` | 使用者設定（字型、主題、快捷鍵、最近專案、介面語言） |
| `SimSession`（P3） | 模擬器的工作緒＋命令佇列＋狀態快照 |

## UI 層（`ui/`）

`MainWindow` 只負責版面與選單；功能都在面板與動作模組：`CodeEditor`（`QPlainTextEdit` 自製：行號、上色、診斷波浪線、括號配對、折疊、補全）、
`ProjectTree`、`ProblemsPanel`、`OutlinePanel`、`ListingPanel`…；每個面板收 `BuildSnapshot` 更新自己。主題用 token 名稱（`lang::tokName`）對應顏色，
可由設定檔覆寫，不寫死在程式裡。

## 介面語言

介面字串全部用 `tr()`（或 `QT_TRANSLATE_NOOP`）寫成英文，翻譯放在 `ui/translations/`（`.ts` 來源、`.qm` 內嵌）。
`ui/i18n.*` 安裝／切換翻譯器；`AppShell` 持有主視窗，語言改變時重建視窗（每個字串在建構時翻譯，重建最可靠）。
服務層與核心**不含**翻譯：它們的訊息（組譯診斷、模擬器停止原因）保持英文。

## 建置

- CMake：頂層 `ASMCORE_BUILD_IDE`（預設 ON）；找不到 Qt6 時只建服務層與其測試。
- QMake：`AssemblyCoreIDE.pro` 與 `sources.pri`（檔案清單）；`tests/check_build_files.py` 檢查兩邊的清單一致。
- 平台差異（路徑、換行、高 DPI、打包）集中在 `ui/platform/`，其餘程式碼不寫 `#ifdef _WIN32`。
