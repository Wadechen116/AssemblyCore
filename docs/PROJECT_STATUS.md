# AssemblyCore 專案進度與後續計畫

更新日期：2026-11-02 ・ 最新提交：`799ba30`（master）・ 儲存庫：<https://github.com/Wadechen116/AssemblyCore>（Apache-2.0）

AssemblyCore 是 Sensor Micro-Sequencer ISA v1.6 r10（規格 v31）的組譯器、模擬器、反組譯器，附 Qt Widgets IDE、積木編輯器與範例專案。核心是標準 C++17、不依賴 Qt。

---

## 1. 目前狀態一覽

| 項目 | 狀態 |
|---|---|
| 核心：組譯器、模擬器、反組譯器、硬體描述 | 完成 |
| CLI 工具：`asmc`、`asmsim`、`asmdis`、`asmhw`、`asmblk` | 完成 |
| IDE：編輯器、語言服務、建置、除錯 | P0–P1、P3 完成；P2、P4、P5 待做（見 `AssemblyCoreIDE/docs/IDE_PLAN.md`） |
| 積木編輯器 | M1–M6 完成；B4 進行中 |
| 模擬器（硬體參數、Main/Peri 同跑、即時執行） | S1–S7 完成；S8 待做 |
| 自動除錯（找出並修復） | 完成，規則持續增強 |
| Linux 測試 | `ctest` 13/13 通過 |
| Windows（MSVC）建置 | 0 個 `warning C`；偵錯 26、積木 60、翻譯 7 項 UI 測試、服務層 402 項全過 |
| `dist\msvc2026` | 已更新到 S7（`799ba30`） |

測試數量（Linux）：核心模擬器 217 項、IDE 服務層 402 項，UI 測試全過。

---

## 2. 已完成的階段

### 2.1 模擬器（S 系列）

| 階段 | 內容 | 提交 |
|---|---|---|
| S1 | 硬體參數資料庫（約 70 個參數、`SimParams`、運行中可改） | `51425bb` |
| S2 | 參數面板、目標檔、執行檢視 | `3001530` |
| — | 「模擬器」分頁：參數與執行並排 | `c350052` |
| — | 即時執行（直到中斷點或暫停），由 `sim.speed` 調速；參數運行中立即生效 | `40cbfe8` |
| — | 範例 `14_main_peri_sim`（Main＋Peri 同跑，附參數檔） | `1f9d3c6` |
| — | 速度控制：偵錯工具列下拉選單、`Ctrl+Alt+↑/↓` 加減速 | `9d6dc5c` |
| S4 | ISA Top 架構圖（資料驅動、即時、連接線加粗） | `33c5fd4`、`7c1fe4a` |
| S5 | 雙游標（Main／Peri 各一個，各有自己的單步：`Ctrl+F11`、`Ctrl+Shift+F11`） | `8fc6805` |
| S3 | 軌跡環形緩衝：時間軸是偵錯的即時歷史，可倒回 | `463e11f` |
| S6 | 時間軸連結：把相關的事件連起來，可查詢「為什麼」 | `511dd79` |
| S7 | 搶資源建模（見 2.2） | `b69523f`、`799ba30` |

### 2.2 S7 搶資源建模（詳見 `docs/S7_SHARED_RESOURCES.md` / `.pdf`）

八個參數，**預設全部關閉＝行為和以前完全相同**（測試以「全部寫成初始值，逐週期結果相同」證明）：

* `csr.arbiter_policy`、`csr.arbiter_penalty`：Main／Peri 的 `EXEC_CSR` 共用仲裁器，同週期誰先、交接延遲。
* `datapath.contention`、`datapath.contention_cycles`：Peri 的 `EXEC_PUB` 與 Main 從結果槽的 `MOV` 共用 Data Xfer。
* `slot.copy_extra`、`slot.valid_delay`：結果槽複製變長、結果晚可見。
* `flag.sample_delay_main`、`flag.sample_delay_peri`：BR 看到的是幾個週期以前的旗標。

關鍵規則：等待發生在指令**執行之前**；資源有「占用者／空閒時刻／下一位／輪次時刻」（先前的兩個設計各造成餓死與互相等待，已修並有回歸測試）；狀態放在快照 `State`，倒回自動一致。

顯示：時間軸橘色「等共用資源」標記與統計行、ISA Top 仲裁器與 Data Xfer 方塊、計算信號 `arb.busy`、`xfer.busy`、`arb.stall_main`、`arb.stall_peri`。

**重要提醒**：這些是**模型**，尚未對照 RTL／FPGA 驗證（假設：占用時間約等於延遲減 1、仲裁器只管 `EXEC_CSR`、先到先得）。

### 2.3 自動除錯（詳見 `docs/AUTO_DEBUG.md`）

一鍵「找出並修復」或「只看不改」。修復規則：E101／E103／E104／E105／E201／E202／E203／E204／E205／E301／E303、W410／W501／W502／W504／W517；修好後在獨立模擬器全速跑一段，找出 Fault、結果槽 overrun、迴圈空轉、I2C 無回應等問題。積木程式的修復做在積木本身，整批是一個復原步驟。

### 2.4 積木編輯器

M1–M6（畫布、拖放、欄位編輯、工具箱、專案整合、效能與驗收）、B2（硬體描述抽屜）、B4 第一批（if／while／wait until、變數與運算式、級聯積木）。偵錯時積木上顯示執行狀態（值、閃爍與流向、熱度與次數），按停止後回復靜止。

### 2.5 工具

`AssemblyCoreIDE/tools/md2pdf.cpp`：只用 Qt 把 Markdown 轉成 A4 PDF（支援中文與表格）。用法 `md2pdf INPUT.md OUTPUT.pdf [--title 標題]`。MSVC 上需要專案已有的 `ui/msvc_stdext_compat.h`（已加進 CMake）。

---

## 3. 後續計畫（依使用者指定的順序）

### 3.1 S8：校準套件與 FPGA 擷取比對 —— 需要使用者回答

目標：把 FPGA 量到的資料匯入，和模擬的時間軸疊起來比，找出該打開哪個 S7 參數、填多少。

**阻塞的問題**（需要使用者決定）：
1. 量測來源：ILA／邏輯分析儀／CSR 週期計數器？
2. 匯出格式：CSV？VCD？欄位長什麼樣？
3. 時間基準：量測時脈和 ISA 週期的對應。

### 3.2 S7 的硬體規則確認 —— 需要使用者回答

模型已實作成可選項，但預設值要由硬體事實決定：
* CSR Arbiter 與 Data Xfer 在硬體上是否真的存在競爭？規則（先到先得／固定優先／輪流）？
* 一次存取占用多久？交接有沒有額外延遲？
* 結果槽的 `valid` 在複製完成前能不能被 Main 看到？
* 條件旗標到 BR 之間有幾個週期的延遲？
* Main 觸發語意：Frame Timing WCET＋一個 Timer？

### 3.3 全部實作完成之後：討論 Main／Peri 切換與狀態交換

**先討論、決定，再實作；在使用者同意之前不要動手。** 要談的是模擬器如何在 Main 與 Peri 兩個組合語言程式之間切換、交換執行狀態。

### 3.4 一次性修復「其他下拉選單」的問題

使用者要求把其他下拉選單有問題的地方留到最後一次修。屆時先問清楚是哪個欄位仍有異常（已修過的：封閉選項改用選單、下拉清單等滑鼠放開才關閉、選完即結束編輯）。

### 3.5 最後：`timer.shared` 實驗

使用者指定放在**最後**。內容：
* 新參數 `timer.shared`（預設關＝現在行為）；開啟時 `main_timer.*` 對應到 `timer.*`，`MAIN_TIMER_DONE` 與 `IF_TIMER_DONE` 共用同一個 done 旗標。
* 自動除錯新增發現：「計時器被另一邊搶走」。
* ISA Top 增加共用計時器的版本。
* 用執行面板的基準線比較「共用」與「各自一個」。
* 需要使用者的硬體答案：誰可以啟動它、完成時是否通知兩邊、同時啟動怎麼辦。

### 3.6 其他待辦（B4 與 IDE 計畫）

* B4：多段常數、程序（procedure）的圖形介面。
* IDE 組譯器的 WCET 估算跟隨延遲參數。
* IDE 計畫的 P2、P4、P5（見 `AssemblyCoreIDE/docs/IDE_PLAN.md`）。

---

## 4. 工作約定（持續有效）

* **回覆一律用繁體中文。**
* 每完成一個階段，更新 `D:\swork\AssemblyCore\dist\msvc2026`；若 `AssemblyCoreIDE.exe` 被占用（IDE 開著），不要強制結束，請使用者關閉。
* **相容是硬性要求**：舊檔案與舊 IDE 必須能照常運作；新的模擬行為預設關閉。
* 大階段開始前先列出工作項目請使用者確認；錯誤訊息要能讓人知道怎麼修。
* 下載或安裝任何東西前先說明理由並詢問；不要把新東西放 C 槽；注意 D 槽剩餘空間（用 Windows 端的數字）。
* GUI 程序只能用 PID 結束，禁止 `pkill -f`；不要從 WSL 開真實視窗做冒煙測試（用 offscreen／QTest）；會跳對話框的測試要用 `MainWindow::setQuiet(true)`。
* 大量或正規表達式修改前先備份；優先用精確字串替換並加斷言，替換後必須重新檢查。
* Windows 建置必須 **0 個 `warning C`**。
* Git：提交訊息以 `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` 結尾；可以推送；不要把 GitHub token 寫進檔案或日誌；三份廠商規格（`docs/MicroISA_Spec_v31.pdf`、`docs/MicroISA_v31_assembly_compiler_handoff.md`、`docs/isa_seq_v1.6_r10_detailed_spec_v24_zhTW.docx`）已在 `.gitignore`，**不可公開**；不要擅自改使用者本機的修改（例如 `sample/common/defs.inc`）。

---

## 5. 常用指令

```bash
# Linux 建置與測試（WSL）
cd /mnt/d/swork/AssemblyCore/build && cmake .. && ulimit -v 6000000 && timeout 580 make -j2
ctest                                     # 13 項
./asmcore_sim_tests                       # 核心模擬器
./AssemblyCoreIDE/acide_services_tests    # IDE 服務層

# UI 測試（offscreen）
QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH=$HOME/Qt/6.5.3/gcc_64/lib:$HOME/sysroot/usr/lib/x86_64-linux-gnu \
  ACIDE_TEST_OUT=/tmp ./AssemblyCoreIDE/ui/acide_ui_tests

# 模擬實驗（例）
./asmsim prog.asm --param latency.csr_access=6 --param csr.arbiter_policy=main_first --cycles 4000 --events ev.csv --dump

# Markdown → PDF
./AssemblyCoreIDE/md2pdf docs/FOO.md docs/FOO.pdf --title "標題"

# 重新產生參數文件
./gen_sim_params_doc > ../docs/SIM_PARAMS.md
```

Windows 建置在 `/mnt/d/WSL/acbuild_msvc`（`build.bat`、`uitest.bat`），警告檢查：`iconv -f cp950 -t utf-8 -c build.log | grep -c "warning C"`。

---

## 6. 文件索引

| 文件 | 內容 |
|---|---|
| `docs/PROJECT_STATUS.md` | 本文：進度與後續計畫 |
| `docs/S7_SHARED_RESOURCES.md` / `.pdf` | S7 功能與建置邏輯 |
| `docs/SIM_VISUAL_DESIGN.md` | S3–S6 視覺化設計 |
| `docs/SIM_PARAMS.md` | 硬體參數目錄（自動產生） |
| `docs/SIM.md` | 模擬器 |
| `docs/AUTO_DEBUG.md` | 自動除錯 |
| `docs/BLOCKS_EDITOR.md`、`docs/BLOCKS.md` | 積木編輯器 |
| `docs/ASM_SYNTAX.md`、`docs/HARDWARE.md`、`docs/DEBUG_INFO.md`、`docs/DISASM.md` | 語法、硬體描述、除錯資訊、反組譯 |
| `AssemblyCoreIDE/docs/IDE_PLAN.md` | IDE 各階段計畫與狀態 |
| `AssemblyCoreIDE/docs/DEBUGGING.md`、`ARCHITECTURE.md` | 除錯與架構 |
| `sample/README.md` | 範例專案（01–14） |

---

## 7. 待使用者回答的問題（三處）

回答之前，對應的工作只做到「機制可選、預設關閉」，不會把任何猜測當成預設。回答後請直接填在「答覆」欄，我再據此實作或調整預設。

### 7.1 S8：FPGA 量測資料的格式（阻塞 S8）

| # | 問題 | 選項／說明 | 答覆 |
|---|---|---|---|
| A1 | 量測來源是什麼？ | ILA、邏輯分析儀、CSR 週期計數器（可多選） | |
| A2 | 匯出格式？欄位長什麼樣？ | CSV（欄位：時間、信號名、值？）或 VCD；請給一小段範例檔 | |
| A3 | 量測時脈和 ISA 週期怎麼對應？ | 例：1 ISA 週期＝N 個取樣點；有沒有固定偏移 | |
| A4 | 要比對哪些信號？ | 例：兩個 decoder 的 pc、CSR 寫入、`PERI_RESULT_VALID`、計時器 done | |

影響：S8 的匯入器、時間軸疊圖、以及「差在哪一類就建議打開哪個 S7 參數」的判斷。

### 7.2 S7：硬體規則（決定預設值與模型形狀）

| # | 問題 | 目前模型的假設 | 答覆 |
|---|---|---|---|
| B1 | CSR Arbiter 在硬體上是否真的存在 Main／Peri 競爭？ | 是（可選，預設關） | |
| B2 | 裁決規則：先到先得、固定優先（誰優先）、還是輪流？ | 先到先得；同週期撞上才用 `csr.arbiter_policy` | |
| B3 | 一次 `EXEC_CSR` 占用仲裁器多久？ | `max(1, 延遲 − 1)` 週期（只含存取段） | |
| B4 | 交接有沒有額外延遲？幾週期？ | 0（`csr.arbiter_penalty`） | |
| B5 | `EXEC_XFER` 的 I2C 路徑是否也要經過 Arbiter？ | 否 | |
| B6 | Data Xfer 的使用者還有哪些？ | 只有 Peri `EXEC_PUB` 和 Main 從結果槽的 `MOV` | |
| B7 | 結果槽的 `valid` 在複製完成前 Main 能否看到？複製要幾週期？ | 發佈當下立即可見（`slot.copy_extra`、`slot.valid_delay` 為 0） | |
| B8 | 條件旗標到 BR 之間有幾個週期延遲？ALU 旗標也有嗎？ | 0；不含 ALU 旗標（`flag.sample_delay_*`） | |
| B9 | Main 的觸發語意：Frame Timing WCET＋一個 Timer？ | 尚未定 | |

### 7.3 `timer.shared` 實驗的硬體問題（實驗排在最後）

| # | 問題 | 說明 | 答覆 |
|---|---|---|---|
| C1 | 誰可以啟動這個共用計時器？ | 只有 Main？只有 Peri？兩邊都可以？ | |
| C2 | 完成時是否同時通知兩邊？ | 即 `MAIN_TIMER_DONE` 與 `IF_TIMER_DONE` 是否同一個 done 旗標 | |
| C3 | 兩邊同時啟動怎麼辦？ | 後者覆蓋、前者優先、或視為錯誤？ | |

實驗內容（待問題回答後才做）：新參數 `timer.shared`（預設關）、`main_timer.*` 對應到 `timer.*`、自動除錯新增「計時器被另一邊搶走」、ISA Top 共用計時器版本、用執行面板基準線比較「共用」與「各自一個」。
