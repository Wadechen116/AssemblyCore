# sample — 用來測試 AssemblyCoreIDE 功能是否齊全的範例專案

每個子資料夾是一個可以直接用 IDE 開啟的專案（`File → Open Project…` 選該資料夾裡的 `.acproj`）。
它們同時也是 **AssemblyCore 的整合測試**：`check_samples.py` 會用同一個引擎的命令列工具（`asmc` / `asmsim` / `asmdis`）把每個專案建置、
跑情境腳本、再反組譯比對，結果與 IDE 內看到的一致。

```
python3 sample/check_samples.py                 # 預設使用 build/ 底下的工具
python3 sample/check_samples.py --bin-dir D:\swork\AssemblyCore\dist\msvc2026
ctest -R sample_projects                         # 在 CMake 建置目錄中（已納入測試）
```

## 專案一覽

| 專案 | 主題 | 預期建置結果 |
|---|---|---|
| `01_hello_csr` | 最小的 Main 程式：CSR 寫入、ALU 迴圈、`WRITE_SP`、單步與中斷點 | 成功，零警告 |
| `02_imu_gyro` | Main + Peri、服務契約（`.service … big`）、位元組交換、Poll／Timer、環境檔、情境腳本、VCD | 成功，零警告 |
| `03_call_models` | `.proc` / `CALL` / `RET`，**inline 與 softret** 兩種呼叫模型比較、WCET 查詢、呼叫堆疊 | 成功（兩種模型） |
| `04_alu_cascade` | 全部 ALU 運算、常數池（`@名稱`）、`CASCADE`、一次 `WRITE_SP` 寫 16 位元組 | 成功，零警告 |
| `05_events` | 硬體事件：`.vector`、`safe` 分支、handler、`BR.ERET`、事件注入、事件記錄 | 成功，零警告 |
| `06_diagnostics` | **會建置成功但有 31 個警告**，涵蓋 W401–W517 幾乎全部碼 | 成功，有警告 |
| `07a…07d_errors_*` | **故意寫錯**（共 4 個專案，依錯誤出現的階段分開）：測試波浪線、問題面板、錯誤說明 | 失敗，並列出預期的錯誤碼 |
| `08_runtime_faults` | 建置成功但**執行時 fault**：fault 顯示、`Clear fault`、`csr_buf_valid` | 成功（有 2 個警告），執行時 fault |
| `09_profile_artifacts` | 非預設 **目標 profile**（SRAM 配置、對齊、DeviceMap 排列、ALU 碼重映射）與產物 | 成功（兩種模型） |
| `11_hardware_designer` | **硬體設計器**：CSR／裝置／服務／常數都由 `board.achw` 以表單定義，程式裡沒有任何定義；即時驗證；接手既有專案 | 成功，零警告 |
| `12_block_files` | **積木 B0**：`.acblk` 檔（程式的積木模型）、`asmblk` 轉換、文字⇄積木往返保證（圖形編輯器在 B1） | 成功，零警告 |
| `10_multi_file` | 專案含多個來源檔（`shared.inc` + Main 檔 + Peri 檔）：專案樹、跨檔重新命名／找參照／跳轉 | 成功，零警告 |

`common/defs.inc` 是共用的定義檔（CSR 位址是**範例值**，規格只給名稱，不給編號），各專案用 `include_dir = ../common` 引入。

`.acproj` 內的 `sample_expect`、`sample_errors`、`sample_models` 只給 `check_samples.py` 讀；IDE 會原樣保留這些未知的鍵。

## IDE 功能 ↔ 在哪個範例測試

| IDE 功能 | 範例 | 怎麼試 |
|---|---|---|
| 開啟專案、專案樹、最近專案 | 全部、`10` | File → Open Project；專案樹列出 sources、scenarios、環境檔 |
| 語法上色、行號、括號配對 | 全部 | 打開任何 `.asm` |
| 補全（指令、運算元位置、符號、`safe`） | `01`、`05` | 在行首、`MOV ` 後、`BR.COND …, ` 後輸入，Ctrl+Space |
| 懸停說明（符號值、指令簽名、規格章節、診斷） | `01`、`06` | 滑鼠移到指令、`.csr` 名稱、波浪線上 |
| 跳到定義 F12、找參照 Shift+F12、重新命名 F2 | `02`、`10` | 在 `shared.inc` 重新命名 `SVC_TEMP`，兩個 domain 的檔案同步改 |
| 大綱（符號分類、值、呼叫點數） | `03`、`10` | 大綱面板 |
| 搜尋／取代、專案內搜尋 Ctrl+Shift+F、註解切換 Ctrl+/、複製／移動行 | 任一 | Edit 選單 |
| 自動建置、問題面板（篩選、跳轉）、輸出面板 | `06`、`07*` | 修改後即時重建；雙擊問題跳到該行 |
| 診斷（波浪線、邊欄圖示） | `06`、`07*` | 對照原始碼註解裡標的碼 |
| 呼叫模型切換（Build → Call model） | `03`、`09` | 切換後比較 `.map` 與 `.lst` 的大小與 WCET |
| 資料流檢查切換（Build → Data-flow check：off／warn／error） | `06` | 切成 error，警告變成 `E5xx` 錯誤（建置失敗） |
| 清單檢視與原始碼雙向同步 | `01`、`03` | 打開 `.lst`，點一行看對應位址 |
| 產物寫出（`.bin`、`.layout.json`、`.sram.hex`、`.boot.json`…） | `09` | F7 建置後看輸出面板列出的 `wrote …`；`out/` 資料夾 |
| Profile（專案 `profile =`） | `08`、`09` | 改 `toy.profile` 的鍵，看 E311／E312／E314 等 |
| 除錯：開始 F5、暫停 F6、停止 Shift+F5、重新開始 | `01`、`02`、`05` | Debug 選單 |
| 單步 F11／F10／Shift+F11、執行到游標 Ctrl+F10、焦點 domain | `01`、`03` | `03` 的 `CALL` 是一步，進入程序後才逐行 |
| 中斷點（行、條件、命中次數）、資料中斷點 | `01`、`04` | 例如條件 `scratch[0..1] == 3` |
| 狀態／記憶體（可改）／CSR（可改）／訊號／監看／呼叫堆疊面板 | `01`–`05` | View 選單；改 CSR 值會觸發它的動作 |
| 事件記錄（篩選、跳轉、匯出文字／CSV／VCD） | `02`、`05` | Events 面板；匯出 VCD 用波形檢視器開 |
| 環境檔編輯（Debug → Edit Environment File…） | `02`、`08` | 改 `[device]` 的暫存器值、NACK 注入 |
| fault 顯示與清除 | `08` | 見該專案 README |
| 失控保護（週期／牆上時間上限） | `01` | 把 `done: JMP done` 改成真的無窮迴圈（例如 `JMP start` 且不碰 halt） |
| 硬體設計器（元件樹、屬性表單、問題清單、產生檔預覽、Adopt） | `11` | 見該專案 README |
| 中英文介面切換（View → Language） | 任一 | 切換後專案與中斷點保留 |
| 事件（Peri／Main handler）的單步與 `main.in_handler` 訊號 | `05` | 在 `on_frame` 設中斷點，Debug 時注入事件 |

## 目前 IDE 還沒做的（測試時會發現，不是範例的問題）

- **情境腳本（`.scn`）在 IDE 內沒有「執行」按鈕**：專案樹只列出它們（P4）。請用 `asmsim … --scenario` 或 `check_samples.py`；範例內每個 `.scn` 都附了說明。
- 事件中斷點（NACK、overrun 時停止）、回溯／時間軸、剖析、環境檔表單模式。
- 組譯器診斷文字與 `asmc --explain` 的內容目前只有英文。
- 「快速修復」（例如把 v24 的 `WRITE_BUF` 自動改成 `WRITE_SP`）、格式化、事件時間（`events.json` 的回應時間）的專屬檢視：尚未做，內容在輸出面板與 `.map`。

這些項目在 `AssemblyCoreIDE/docs/IDE_PLAN.md` 有編號與狀態。
