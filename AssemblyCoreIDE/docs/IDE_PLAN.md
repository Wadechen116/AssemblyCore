# AssemblyCoreIDE 計畫與進度

決定（使用者確認）：Qt 原生（Widgets）、編輯器自製、第一版範圍 = P0–P5 全部、Windows／Linux／macOS、核心與服務層標準 C++17、
CMake 與 QMake 都能建置、低耦合高彈性、**不做**寫入板子（F4）。架構見 `ARCHITECTURE.md`。

圖例：✅ 完成並有測試　🟡 部分完成　⬜ 未開始

## 階段

| 階段 | 內容 | 狀態 |
|---|---|---|
| P0 | 核心語言服務：診斷欄位（L1）、詞彙／token（L2）、符號索引與盡力而為（L3、L4）、行↔位址（L5） | ✅ |
| P1 | 編輯與建置：服務層、專案、編輯器、面板、自動建置 | 🟡 主體完成，見下表缺項 |
| P2 | 分析視圖：表用量、WCET、呼叫模型比較、重新命名 | ⬜（重新命名已在 P1 做完） |
| P3 | 模擬與除錯（`DEBUGGING.md`） | 🟡 主體完成，見下表缺項 |
| P4 | 進階除錯：時間軸、回溯、情境腳本、剖析、注入 | ⬜（需 L7） |
| P5 | 快速修復、格式化、反組譯檢視、打包與測試 | ⬜（QMake／CMake 建置與檢查腳本已完成） |

## P1 項目

| ID | 項目 | 狀態 | 備註 |
|---|---|---|---|
| A1 | 分層（services 無 Qt、ui 用 Qt） | ✅ | 靜態庫 `acide_services`、`acide_ui` |
| A2 | 背景建置、最新優先、過期結果丟棄 | ✅ | TSan 無競態 |
| A3 | `SimSession`（模擬器專用工作緒、命令佇列、快照） | ✅ | P3；TSan／ASan 乾淨 |
| A5 | 未存檔緩衝區直接建置 | ✅ | 當機復原（自動儲存草稿）⬜ |
| B1 | 專案檔 `.acproj` | ✅ | 解析／序列化往返、未知鍵保留、行號錯誤 |
| B2 | 專案樹 | 🟡 | 檢視、開啟、未存檔標記；新增／刪除／搬移尚未 |
| B3 | 新增專案範本 | 🟡 | 一個範本（零警告）；範例專案挑選尚未 |
| B4 | 最近專案、工作階段還原 | ✅ | 歡迎頁 ⬜ |
| B4a | File → Close Project（Ctrl+Shift+W） | ✅ | 先問未存檔內容（可取消）、存工作階段、停止除錯、關閉分頁；之後再開同一專案會還原；沒有專案時停用 |
| B5 | 外部變更偵測、CRLF／BOM 保留 | ✅ | 編碼選擇 UI ⬜ |
| B6 | `.inc` 被誰含入的專案層檢查 | ⬜ | |
| C1 | 語法上色 | ✅ | 主題以 token 名稱為鍵，可由設定檔覆寫 |
| C2 | 行號、目前行、括號配對、縮排 | ✅ | 折疊、書籤 ⬜ |
| C3 | 診斷：波浪線、邊欄圖示、懸停 | ✅ | 「說明」連結 ⬜ |
| C4 | 補全 | ✅ | 依指令、運算元位置、domain 過濾；符號來自索引 |
| C5 | 參數提示與編碼資訊 | 🟡 | 懸停顯示簽名；游標跟隨的參數提示 ⬜ |
| C6 | 懸停 | ✅ | 符號值、指令簽名、規格章節 |
| C7 | 跳到定義、找參照、重新命名 | ✅ | F12／Shift+F12／F2；重新命名遇過期索引會拒絕 |
| C8 | 大綱 | 🟡 | 分類、值、呼叫點數；WCET 欄 ⬜ |
| C12 | 搜尋／取代、專案內搜尋、註解切換、複製／移動行 | ✅ | |
| D-1 | 建置（含呼叫模型、資料流快速切換） | ✅ | 選單隨專案同步 |
| D-2 | 問題面板（篩選、跳轉） | ✅ | |
| D-3 | 輸出面板、產物寫出 | ✅ | |
| D-4 | 清單檢視與原始碼雙向同步 | ✅ | |

## 介面語言（P5 的 A6）

| 項目 | 狀態 | 備註 |
|---|---|---|
| English／繁體中文切換 | ✅ | View → Language：自動（跟隨系統）、English、繁體中文；設定存於 `ui.language` |
| 切換方式 | ✅ | 重建視窗（`AppShell`）；專案、開啟的檔案、中斷點回來，執行中的除錯工作階段會結束；有未存檔內容時會先詢問 |
| Qt 內建對話框（檔案對話框、按鈕） | ✅ | 內嵌 Qt 的 `qtbase_zh_TW.qm` |
| 翻譯完整性檢查 | ✅ | `.ts` 與原始碼、`.qm` 與 `.ts` 的一致性、佔位符；`acide_update_translations` 目標更新翻譯 |
| 組譯器診斷、模擬器停止原因、詞彙說明文字 | ⬜ | 仍是英文（核心不依賴 Qt；需要訊息代碼表才能翻譯） |
| 新專案範本的註解 | ⬜ | 仍是英文 |
| 簡體中文及其他語言 | ⬜ | 加一種語言的步驟見 `ui/translations/README.md` |

## P3 項目

| ID | 項目 | 狀態 | 備註 |
|---|---|---|---|
| E1 | 環境檔編輯 | 🟡 | 純文字編輯、未存檔內容會被使用、錯誤會列出；表單模式與即時驗證 ⬜ |
| E2 | 開始、停止、重置、繼續、暫停、執行到游標、跑 N 週期、跑到運算式 | ✅ | 速度節流 ⬜ |
| E3 | 單步進入／跳過／跳出、焦點 domain | ✅ | 核心新增 `stepOut` |
| E4 | 中斷點（行、條件、命中次數、停用）與資料中斷點 | ✅ | 事件中斷點（NACK、overrun）⬜ |
| E5–E7 | 狀態、記憶體（可改）、CSR（可改） | ✅ | |
| E8 | 訊號面板 | ✅ | 邊緣偵測標示 ⬜ |
| E9 | 監看運算式（十進位、十六進位） | ✅ | 其他顯示格式 ⬜ |
| E10 | 呼叫堆疊（兩個 domain） | ✅ | |
| E11 | 反組譯／混合檢視 | 🟡 | 清單面板跟隨游標與執行位置；描述子內容 ⬜ |
| E12 | fault 顯示與清除 | ✅ | State 面板與「Clear fault」 |
| E13 | 事件記錄、篩選、跳轉、匯出文字／CSV／VCD | ✅ | |
| E21 | 失控保護 | ✅ | 週期與牆上時間上限；核心新增可取消的 `run` |

## 目前測試

| 套件 | 項數 |
|---|---|
| 核心（組譯器） | 180 |
| 核心（模擬器） | 106 |
| 服務層 | 137（ASan、TSan 乾淨） |
| UI（offscreen） | 33 |
| 建置檔案一致性（`tests/check_build_files.py`） | 1 |
| 翻譯完整性（`tests/check_translations.py`） | 1 |

## 平台驗證

| 平台 | 工具鏈 | 結果 |
|---|---|---|
| Linux（WSL Ubuntu） | GCC 13、Qt 6.5.3；CMake **與** QMake | 全部通過（含 offscreen GUI 測試） |
| Windows | MinGW 11.2、Qt 6.5.3 MinGW 版、CMake | 全部通過；GUI 測試需 `ACIDE_TEST_OUT=<目錄>` 才讀得到結果（GUI 子系統無主控台） |
| Windows（MSVC 2026） | Visual Studio Community 2026（工具集 14.51），Qt 6.5.3 `msvc2019_64`，CMake＋Ninja | 全部通過，零警告；需 `ui/msvc_stdext_compat.h`（見下）；發佈資料夾在 `dist/msvc2026/` |
| macOS | — | **尚未驗證**（此環境沒有 macOS）；程式碼未使用平台 API，檔案存取用 `std::filesystem`，預期只需 Qt 6 與 CMake／qmake |

### MSVC 2026 的注意事項

- 機器上有**系統層級環境變數**（`VSInstallDir`、`VCToolsInstallDir`、`WindowsSDK_Include_ucrt`、`VCInstallDir2017Cmty`）固定指向 VS2017，會讓新版 `vcvars64.bat` 失敗。建置腳本在呼叫 `vcvars64.bat` 前先 `set` 成空值即可，不必改系統設定。
- Qt 6.5 的 `QVarLengthArray` 需要 `stdext::make_checked_array_iterator`，VS 2026 的 STL 已移除；`ui/msvc_stdext_compat.h` 只在 `_MSC_VER >= 1950` 補上，CMake 與 qmake 都用 `/FI` 強制引入。Qt 本身沒有被修改。
- 發佈資料夾需要目標機器已安裝 VC++ 可轉散發套件（`windeployqt` 沒帶 `--compiler-runtime`）。

## 建置

```
cmake -S .. -B build -DCMAKE_PREFIX_PATH=<Qt6 路徑> && cmake --build build && ctest --test-dir build
qmake AssemblyCoreIDE.pro && make          # 另有 services_tests.pro、ui_tests.pro
python3 tests/check_build_files.py        # CMake 與 qmake 的檔案清單必須一致
```
GUI 測試用 `QT_QPA_PLATFORM=offscreen`。找不到 Qt6 時 CMake 只建服務層與其測試。
