# AssemblyCore 專案進度與後續計畫

更新日期：2026-11-20 ・ 最新程式提交：`a32cc44`（分支 `blocks-toolbox-single-pieces`，已推送）・ 儲存庫：<https://github.com/Wadechen116/AssemblyCore>（Apache-2.0）

AssemblyCore 是 Sensor Micro-Sequencer ISA v1.6 r10（規格 v31）的組譯器、模擬器、反組譯器，附 Qt Widgets IDE、積木編輯器與範例專案。核心是標準 C++17、不依賴 Qt。

分支關係：`master`（`df4ab1e`，2026-11-02）← `sim-timer-flag-toolbox-longrun`（`93c8125`，2026-11-10）← `blocks-toolbox-single-pieces`（目前工作分支）。三者尚未合併；依使用者決定（2026-11-18），功能穩定之前不開 PR，持續在工作分支提交並推送。

---

## 1. 目前狀態一覽

| 項目 | 狀態 |
|---|---|
| 核心：組譯器、模擬器、反組譯器、硬體描述、EEPROM 映像 | 完成（v31 遷移階段 1–5 完成；階段 6、7 見 `docs/V31_MIGRATION.md`） |
| CLI 工具：`asmc`、`asmsim`、`asmdis`、`asmhw`、`asmblk` | 完成 |
| IDE：編輯器、語言服務、建置、除錯 | P0–P1、P3 完成；P4、P5 部分；P2 待做（見 `AssemblyCoreIDE/docs/IDE_PLAN.md`） |
| 積木編輯器 | B0、B1（M1–M6）、B2 完成；B4 部分（缺多段常數、程序介面）；工具箱改為單片積木 |
| 模擬器（硬體參數、Main／Peri 同跑、即時執行） | S1–S7 完成；EEPROM 取指、共用 I2C、Frontend 七狀態模型完成；S8 待做 |
| 自動除錯（找出並修復） | 完成；編輯器的波浪線可按右鍵修復 |
| EEPROM 流程（建置、驗證、反組譯、從映像除錯） | 完成 |
| Linux 測試 | `ctest` 15/15 通過（2026-11-20 重跑） |
| Windows（MSVC 2026）建置 | 0 個 `warning C`；組譯器 344、模擬器 336、服務層 456 項通過（2026-11-20）；**UI 測試自 2026-11-18 起未在 Windows 重跑** |
| `dist\msvc2026` | exe 更新到「WCET 含取指時間」（2026-11-20），範例含 17 |
| macOS | 尚未驗證 |

測試數量（Linux，2026-11-20）：

| 套件 | 項數 |
|---|---|
| 核心：組譯器 | 344 |
| 核心：模擬器 | 336 |
| 核心：反組譯器 | 63 |
| 核心：解碼交叉比對（反組譯器對模擬器） | 6（0 個不一致） |
| 核心：硬體描述 | 83 |
| 核心：積木（模型 125、元件 200、編輯 125） | 450 |
| IDE 服務層：積木 | 267 |
| IDE 服務層：其餘 | 456 |
| IDE 介面（offscreen，六個測試類別） | 136 通過、1 略過（長時間浸泡測試，需 `ACIDE_SOAK_SECONDS` 才跑） |
| 範例專案、`asmc` 參數選項（9 項檢查）、建置檔一致性、翻譯完整性 | 各 1 項，通過 |

**使用前要知道的限制**：模擬器與 WCET 的許多數值是規劃值或假設，尚未對照 RTL／FPGA（見第 7 節）。在硬體答覆之前，模擬結果可用來理解行為與比較方案，不能當作時序簽核的依據。

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

### 2.5 工具（md2pdf）

`AssemblyCoreIDE/tools/md2pdf.cpp`：只用 Qt 把 Markdown 轉成 A4 PDF（支援中文與表格）。用法 `md2pdf INPUT.md OUTPUT.pdf [--title 標題]`。MSVC 上需要專案已有的 `ui/msvc_stdext_compat.h`（已加進 CMake）。

### 2.6 計時器、Status Flag 與長時間執行（2026-11-04，`2bb63fa`、`f0ee607`）

* 兩個計時器共用 `clock.timer_hz`（13.5 MHz＝27 MHz／2，使用者確認）；`timer.prescaler`、`timer.count`、`timer.phase` 是待 RTL 確認的規劃值。
* Status Flag 是 Main／Peri 的 BR 共用的一個單元（使用者確認）：`flag.arbiter_policy`、`flag.access_cycles`、`flag.arbiter_penalty`，預設 `main_first`。
* CSR 仲裁器：先到先得，同週期 Main 先（使用者確認）→ `csr.arbiter_policy` 預設 `main_first`。這兩個預設是使用者明確指定的例外，不受「預設不改變舊結果」限制。
* 長時間執行的記憶體與回應問題已修（軌跡標記上限、更新回壓、快照共用事件紀錄）；浸泡測試 `DebugTests::aLongDebugRunStaysBoundedAndResponsive`（`ACIDE_SOAK_SECONDS=60` 才跑）。

### 2.7 參數就地設定（2026-11-05，`9a58396`）

模擬器頁面不再有參數表：架構圖的方塊按右鍵設定該方塊的參數，積木按右鍵設定該指令的參數，完整表格在「所有參數」對話框。

### 2.8 EEPROM 流程（2026-11-05～06，詳見 `docs/EEPROM.md`）

硬體事實（使用者說明）：程式碼、描述子表、常數池、DeviceMap 存在 I2C EEPROM（0x50，與 Peri 的 `EXEC_XFER` 共用匯流排），解碼器每道指令都去讀；各區的起始位址是暫存器。

* `asmc`／`asmdis`／`asmsim` 的 `--eeprom`；IDE 建置寫出 `.eeprom.*` 與 `PREFIX.eeprom.sym`（名稱）。
* 模擬器預設從 EEPROM 取指（`fetch.source = eeprom`）；`fetch.read_mode` 為 sequential（預設）／per_byte／instant；解碼器與 `EXEC_XFER` 共用 I2C 匯流排，先到先得，同週期 Main 先，不插隊（使用者確認）。
* 驗證燒錄檔：`asmsim --verify-eeprom`；反組譯燒錄檔：`asmdis --eeprom`（`--check`、`--strict`）。
* IDE：Build 選單的「EEPROM」視窗（建置／驗證／反組譯三頁）、「Debug from an EEPROM Image」、Decoder 面板。
* 範例 `15_eeprom_images`（正確與故意寫壞的映像）。
* 常數池的名稱在反組譯結果中以註解呈現，不還原成 `.const` 定義（使用者決定，2026-11-09）。

### 2.9 解碼問題分級與硬體偵測表（2026-11-09～10，`2bcf11b`、`a3daf44`）

* 分三類：A 不合法的編碼、B 編碼合法但違反規格規則、C 執行期狀態造成。反組譯器與模擬器的判定必須完全一致（`tests/crosscheck_tests.cpp`）。
* IDE 在問題面板（Kind 欄）、編輯器波浪線、Decoder 面板同時呈現；範例 `16_error_showcase`。
* `docs/HW_FAULT_DETECTION.md`（由 `docs/gen_hw_fault_table.py` 產生）：55 列，請硬體團隊填「哪一級偵測」。共享頁面：<https://claude.ai/artifact/2pKLgKyzHGWpXQE5iK1itR>。
* 原則（使用者）：規格文件才是依據，模擬器不是解碼器的規格。

### 2.10 積木工具箱與畫布（2026-11-18～19）

* 工具箱每個項目都是一片獨立的積木，資料插槽是空洞，選項插槽（運算種類、有無號、分支條件）保留預設值；Patterns 與 Hardware 抽屜移到最後、預設收合。
* 參照類積木以用途命名（NAME、label、procedure、CSR_NAME…），不再有多個同名的「name」。
* 接點磁吸：能接就吸過去，不能接就推開並彈回。
* 工具箱的樣本放大並以實際大小繪製；畫布預設 66%；Main／Peri 兩欄靠近；右鍵在空白處拖曳可平移畫面。

### 2.11 編輯器修復選單與即時清單（2026-11-18）

* 組合語言編輯器的波浪線按右鍵 →「Fix E###」與「Fix all」；修復時顯示進行中與結果（編輯器內提示、狀態列、輸出面板）。
* Listing 面板在除錯時顯示實際執行的程式：Main／Peri 位置、執行次數、中斷點、fault、與取指內容不同的列。

### 2.12 解碼階段、Program SRAM 與 Frontend 模型（2026-11-19，`20c0272`、`479f965`）

* Decoder 面板顯示一道指令如何形成：第一階段位元組（opcode＋index）→ 查描述子表 → 第二階段的完整指令。
* Program SRAM 面板：開機時由 ROM 載入的共用 32-bit SRAM 內容。
* Frontend 模型（選用，`fetch.source = frontend`；預設仍是 `eeprom`）：開機把 ROM 載入 SRAM，之後每道指令走規格 v31 的七個狀態；Decoder 面板可勾選、可單步一個週期。`boot.load`、`frontend.*` 參數全部是規劃值，待 RTL 提供。

### 2.13 架構圖的單元活動與範例 17（2026-11-19，`437e3a4`、`a32cc44`）

* 架構圖上指令用到的單元會一起閃（佈局檔的 `touch` 行）；ALU 回報它寫入的 out 與 status。
* 範例 `17_alu_busy`：不會結束的 MOV／ALU 迴圈，用來在模擬器頁面觀察 ALU。

### 2.14 WCET 跟隨模擬器的硬體參數，並計入取指時間（2026-11-20）

* 以前 IDE 建置時組譯器只看 profile 的延遲，模擬器另外套用硬體參數，所以清單的週期欄、WCET 與實際執行可能用不同的數字。
* 現在建置會把參數中屬於週期模型的值（`latency.*`、`clock.cpu_hz`、`clock.i2c_hz`）蓋在 profile 之上再組譯，順序與模擬器相同（profile → 參數）。映像不受影響，只有計數改變。
* 在模擬器頁面改這類參數會自動重新建置；改其他參數（速度、仲裁規則…）不會。
* 按 F7 建置時，輸出面板會列出哪些數字來自參數，例如 `latency.alu = 6 (profile: 1)`。
* **WCET 計入取指時間**（使用者決定：跟著 `fetch.source` 走；搶匯流排的等待先不納入）。每道指令除了執行週期，再加上硬體讀出它所花的週期；`.wcet` 的結果分開報「執行＋取指」，例如 `33443 cycles (10 executing + 33433 fetching the instructions)`。程序與呼叫點的 WCET、事件延遲也一併計入。清單的週期欄仍是執行週期。改 `fetch.*`、`frontend.*`、`clock.i2c_hz` 等參數會重新建置。
* 驗證：同一個程式在模擬器單獨執行到終點的週期數，與 WCET 完全相同。
* 限制：(1) **不含**等另一個 sequencer 的時間（共用 I2C 匯流排、CSR 仲裁器、Status Flag、Data Xfer），那取決於另一邊當時在做什麼，所以兩邊同時執行時實際時間仍可能比 WCET 長；(2) 只跟隨參數（`.simparams` 與模擬器頁面設定的值），環境檔裡的 `[latency]`、`[bus i2c]`、`[params]` 區段不會進到建置；(3) 命令列的 `asmc` 要加 `--params FILE`、`--param KEY=VALUE` 或 `--fetch-time` 才用同一種算法（數字與 IDE 相同）；不加時維持原本只計執行週期的行為。`sample/build_samples.py` 已照 IDE 的方式傳入（專案有 `.simparams` 用 `--params`，否則 `--fetch-time`），範例輸出資料夾的 `.map` 與 IDE 一致；(4) 取指的數值本身是算出來的規劃值（見下），尚未對照硬體。

**取指時間怎麼算**（沒有硬體時的估算依據）：不是量測，而是依協定計算，與模擬器的解碼器、Decoder 面板用同一份程式。

* 從 I2C EEPROM 取指（預設）：一次讀取 n 個位元組要傳 `2 + 位址位元組數 + n` 個位元組階段（裝置位址＋寫、記憶體位址、重新起始後的裝置位址＋讀、資料），每個階段 9 個 SCL 脈衝（8 位元＋ACK）。週期數＝`⌈9 × 階段數 × clock.cpu_hz ÷ clock.i2c_hz⌉ + 階段數 × (i2c.ack_delay + i2c.stretch) + fetch.transaction_overhead`。27 MHz／400 kHz、2 個位址位元組時：讀 1 個位元組 3,040 週期、2 個 3,647、4 個 4,862。一道指令要讀它的程式碼位元組、描述子（EXEC_XFER 另加 DeviceMap 項目）、常數，各一次讀取（`fetch.read_mode = per_byte` 時每個位元組一次）。例：`ALU ADD` 讀 1＋1 個位元組＝6,080 週期；`MOV`（4 位元組描述子）＝7,902。
* 從程式 SRAM 取指（`fetch.source = frontend`）：`frontend.*` 各狀態的週期數相加，每道指令只有幾個週期。
* `fetch.source = sram` 或 `fetch.read_mode = instant`：不計取指時間。
* 確定的部分：協定的位元數、兩個時脈（27 MHz 為使用者確認、400 kHz 為規格 14.4）。**假設的部分**：解碼器是否每個區域一次讀完、EEPROM 位址位元組數（profile `eeprom_address_bytes`）、每次交易的額外週期（預設 2）、ACK 延遲與時脈延展（預設 0）、Frontend 各狀態的長度。這些都是參數，硬體數值出來後直接改。

---

## 3. 後續計畫與未完成項目

### 3.1 等硬體答覆（阻塞模擬結果的可信度）

依影響大小排列，問題表在第 7 節：

1. **編碼假設**（7.4）：ALU 子運算碼的數值、移位方向位元（D6，兩份規格說法不一致）、描述子位元組序、DeviceMap 位元排列、ALU_STATUS 旗標位置。這是唯一會讓燒錄映像本身出錯的一類。
2. **硬體錯誤偵測表**（2.9）：55 列，共享頁面至 2026-11-20 為止 0 筆答覆。答覆後調整模擬器的 fault 時機（B5／R4 分支目標、B6 指令中間的目標）。
3. **S7 的硬體規則**（7.2 的 B3–B9）。
4. **Frontend 與計時器的 RTL 數值**（7.5）。
5. **範例 14 的提交視窗**（7.6）：400 kHz 共用匯流排下無法保證落在消隱視窗內，等硬體方向。

### 3.2 S8：校準套件與 FPGA 擷取比對 —— 需要使用者回答（7.1）

目標：把 FPGA 量到的資料匯入，和模擬的時間軸疊起來比，找出該調哪個參數、填多少。尚未開始。

### 3.3 尚未實作的功能

| 範圍 | 項目 |
|---|---|
| 模擬器 | SPI；FIFO 結果槽（非 I2C 的 `bus_type` 目前直接 fault）；Listing 的實測週期欄；SRAM 面板標示已讀／未讀；Frontend 的 `CMD_EXEC` 細分 |
| v31 遷移 | 階段 6 的就緒等級 L0／L1／L2；`asmc --upgrade-v24` 與 IDE 的「升級到 v31」；易用性 U5–U8、U11、U12（見 `docs/V31_MIGRATION.md`） |
| IDE | P2 分析視圖（表用量、WCET、呼叫模型比較）；情境腳本介面與剖析；格式化；歡迎頁與新專案精靈；當機復原；事件中斷點；環境檔表單編輯；折疊與書籤；專案樹的新增／刪除／搬移 |
| WCET | 不含等另一個 sequencer 的時間（共用匯流排、仲裁器）；不跟隨環境檔的 `[latency]`／`[params]`；`asmc` 不加選項時只計執行週期（見 2.14） |
| 積木 | B4 的多段常數（64-bit／double）、程序（procedure）圖形介面 |
| 反組譯器 | 把 softret 序列還原成 `CALL`／`RET` |
| 語言 | 組譯器診斷、模擬器停止原因、範本註解仍是英文 |
| 發佈 | `dist` 未附 VC++ 執行階段，沒有安裝程式；macOS 未驗證 |

### 3.4 尚未在實機上確認的介面改動

以下都有自動測試，但沒有人實際開 IDE 用滑鼠試過；數值是初估：

* 接點磁吸的距離與彈簧常數、被拒絕時的落點。
* 右鍵拖曳平移（Windows 實際滑鼠行為）。
* 單片工具箱、放大的樣本、66% 的畫布。
* 架構圖方塊的位置（依使用者對圖的描述排的）與閃爍節奏（範例 17 的 `sim.speed = 40`）。
* Windows 上的 UI 測試自 2026-11-18 起未重跑。

### 3.5 使用者指定順序、留到後面的三件事

1. **討論 Main／Peri 切換與狀態交換**：先討論、決定，再實作；在使用者同意之前不要動手。
2. **一次性修復「其他下拉選單」的問題**：屆時先問清楚是哪個欄位仍有異常（已修過的：封閉選項改用選單、下拉清單等滑鼠放開才關閉、選完即結束編輯）。
3. **最後：`timer.shared` 實驗**（7.3）：新參數 `timer.shared`（預設關）；開啟時 `main_timer.*` 對應到 `timer.*`，`MAIN_TIMER_DONE` 與 `IF_TIMER_DONE` 共用同一個 done 旗標；自動除錯新增「計時器被另一邊搶走」；架構圖增加共用計時器的版本；用執行面板的基準線比較。

---

## 4. 工作約定（持續有效）

* **回覆一律用繁體中文。**
* **每一次實作都要搭配一次文件更新**（使用者要求，2026-11-20；起因是多份文件落後程式碼二到四週）。程式與受影響的文件在同一個提交裡更新，提交訊息的「修改項目」寫明更新了哪些文件；確實不需要更新時也要寫明。至少檢查：本文（狀態表、第 2、3、7 節、測試數量）、`AssemblyCoreIDE/docs/IDE_PLAN.md`、`docs/V31_MIGRATION.md` 的勾選、`docs/EXTENDING.md`（檔案、鍵、擴充點有搬動時）、`docs/DEBUG_INFO.md` 與 `docs/LANG_SERVICE.md`（介面有變時）、`README.md` 的現況摘要、`sample/README.md`（新增範例時）、該功能自己的說明文件。
* 每完成一個階段，更新 `D:\swork\AssemblyCore\dist\msvc2026`；若 `AssemblyCoreIDE.exe` 被占用（IDE 開著），不要強制結束，請使用者關閉。
* **相容是硬性要求**：舊檔案與舊 IDE 必須能照常運作；新的模擬行為預設關閉。
* 大階段開始前先列出工作項目請使用者確認；錯誤訊息要能讓人知道怎麼修。
* 下載或安裝任何東西前先說明理由並詢問；不要把新東西放 C 槽；注意 D 槽剩餘空間（用 Windows 端的數字）。
* GUI 程序只能用 PID 結束，禁止 `pkill -f`；不要從 WSL 開真實視窗做冒煙測試（用 offscreen／QTest）；會跳對話框的測試要用 `MainWindow::setQuiet(true)`。
* 大量或正規表達式修改前先備份；優先用精確字串替換並加斷言，替換後必須重新檢查。
* Windows 建置必須 **0 個 `warning C`**。
* Git：每個提交訊息都要有日期（年月日）、修改人、作者、修改項目、剩餘問題（沒有就留空）、驗證，並以當時模型的 `Co-Authored-By` 行結尾；在工作分支提交並推送，功能穩定前不開 PR；合併過的分支不刪；不要把 GitHub token 寫進檔案或日誌；三份廠商規格（`docs/MicroISA_Spec_v31.pdf`、`docs/MicroISA_v31_assembly_compiler_handoff.md`、`docs/isa_seq_v1.6_r10_detailed_spec_v24_zhTW.docx`）已在 `.gitignore`，**不可公開**；不要擅自改使用者本機的修改（例如 `sample/common/defs.inc`）。

---

## 5. 常用指令

```bash
# Linux 建置與測試（WSL）
cd /mnt/d/swork/AssemblyCore/build && cmake .. && ulimit -v 6000000 && timeout 580 make -j2
ctest                                     # 15 項
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
| `AssemblyCoreIDE/docs/DEBUGGING.md`、`ARCHITECTURE.md` | 除錯；架構（分層、服務層與 UI 元件、擴充點；2026-11-20 更新） |
| `docs/EEPROM.md` | EEPROM 映像：建置、驗證、反組譯、從映像除錯 |
| `docs/HW_FAULT_DETECTION.md` | 給硬體團隊的錯誤偵測對照表（由 `gen_hw_fault_table.py` 產生） |
| `docs/V31_MIGRATION.md`、`docs/SPEC_V24_VS_V31.md` | v31 遷移檢查清單、v24 與 v31 的差異 |
| `docs/EXTENDING.md`、`docs/LANG_SERVICE.md` | 規格變動時改哪裡、語言服務層 |
| `sample/README.md` | 範例專案（01–17） |

---

## 7. 待使用者／硬體團隊回答的問題

回答之前，對應的工作只做到「機制可選」，預設值不採用猜測（使用者已明確指定的除外，見 2.6）。回答後請直接填在「答覆」欄，我再據此實作或調整預設。

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
| B1 | CSR Arbiter 在硬體上是否真的存在 Main／Peri 競爭？ | 是（可選，預設關） | 是（2026-11-05）：預設改為打開 |
| B2 | 裁決規則：先到先得、固定優先（誰優先）、還是輪流？ | 先到先得；同週期撞上才用 `csr.arbiter_policy` | 先到先得；同時抵達 Main 先、Peri 等（2026-11-05）→ 預設 `main_first` |
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

### 7.4 編碼假設（影響燒錄映像是否正確）

規格只列名稱或寬度、沒有給數值或排列；目前的值在 profile 中，`asmc --profile-gaps` 會列出未確認的群組。

| # | 問題 | 目前的假設 | 答覆 |
|---|---|---|---|
| D1 | ALU 子運算碼的數值（arith／cmp／logic） | 依規格列出名稱的順序：ADD=0、SUB=1、MUL=2、INC=3、DEC=4；EQ=0、NE=1、LT=2、GE=3；AND=0、OR=1、XOR=2、NOT=3。可由 profile 的 `alu_*` 鍵改，不必改程式 | |
| D2 | 多位元組描述子在映像中的位元組序 | 小端 | |
| D3 | DeviceMap 每筆的位元排列 | 1 位元組：bit 7＝bus_type、bits 6:0＝I2C 位址 | |
| D4 | ALU_STATUS 各旗標的位元位置 | bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 | |
| D5 | negative 旗標取哪一位 | 低 16 位元的符號位 | |
| D6 | 移位方向位元 `shift_dir`（SHIFT 描述子的 bit 4）：0 是 SHL 還是 SHR？ | SHL=0、SHR=1（依名稱列出的順序）。**寫死在 `core/src/isa.h`，沒有 profile 鍵**，數值不同時要改程式 | |

D6 單獨列出的原因：兩份規格的說法不一致（2026-11-20 查證）。

* 交接文件（`MicroISA_v31_assembly_compiler_handoff.md` 第 556 行）點名缺數值的只有 ARITH／CMP／LOGIC，並寫「SHIFT 欄位與規則已明確，可依定義 encode」。v31 遷移只把那三組做成 profile 鍵，依據就是這句。
* 規格 PDF（`MicroISA_Spec_v31.pdf` 6.3、6.7 節，第 28、31 頁）給了欄位位置 `[4]=shift_dir`，6.7 的表格以 `SHL`／`SHR` 配 `signed_mode` 0／1 說明語意；從 PDF 抽出的文字裡找不到「SHL 是 0 還是 1」的數值（表格轉文字可能有遺漏，請以原文件為準）。

處理方式：先問 RTL 負責人。答 SHL=0 → 不必改，只把「假設」改成「已確認」；答相反 → 改 `isa.h` 的 `ShiftDir` 一行；答「依目標而定」→ 才做成 profile 鍵。

### 7.5 Frontend、開機載入與計時器的 RTL 數值

| # | 問題 | 目前的規劃值 | 答覆 |
|---|---|---|---|
| E1 | SRAM 讀取延遲、兩個 Frontend 如何共用一個讀取埠 | `frontend.sram_latency = 1`、`frontend.sram_policy = shared`（先到先得，同時 Main 先） | |
| E2 | `CMD_HOLD`、`CMD_DEC`、`CMD_FIRE` 各幾個週期 | 0、1、1 | |
| E3 | 開機載入 ROM→SRAM 的方式與時間 | `boot.load = i2c` | |
| E4 | PC 何時前進；`CMD_EXEC` 是否要再細分 | 執行開始時前進；不細分 | |
| E5 | 計時器的預除器是否自由運轉、計數是 n 還是 n+1、相位 | `timer.prescaler`、`timer.count`、`timer.phase` 的預設值 | |

### 7.6 範例 14：提交能否保證落在消隱視窗內

量測結果（`10b5fda`）：計算已移到視窗之前，視窗內只剩一道 `WRITE_SP`。在 400 kHz 共用匯流排下，檢查之後的最壞情況是 17,634 週期（等 Peri 的 XFER 取指 10,334＋`WRITE_SP` 取指 7,294＋6），大於視窗的 14,362 週期，**無法保證**；1 MHz 時 WCET 7,063，可落在視窗內。

| # | 問題 | 選項 | 答覆 |
|---|---|---|---|
| F1 | 硬體方向 | 匯流排提高到 1 MHz／解碼器用自己的匯流排／取指改走 SRAM（Frontend 模型）／其他 | |

