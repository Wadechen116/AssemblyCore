# 視覺化模擬器與 FPGA 校正 — 設計（草案，待確認）

> 狀態：**S1、S2、S4 已完成**（持續運行、模擬器分頁、積木上色、自動除錯也已完成）（S1 參數資料庫：目錄、`[params]`、命令列；S2 目標檔 `.simparams`、IDE 參數面板、執行狀態面板〔雙軌時間軸、基準疊圖、位移折線、差異表〕；清單與用法見 `docs/SIM_PARAMS.md`），S3、S5…尚未實作（S3 的軌跡記錄已有簡版：`sim_compare`）。依據使用者提供的 ISA Top 架構圖（`ISA_Top.png`）。
> 目標：①把 Main／Peri 兩個 sequencer 以「ISA Top 架構圖」即時視覺化；②所有影響時間與行為的硬體條件都是**可調參數**；
> ③上 FPGA 後，用實測值回饋校正這些參數，讓模擬器與 FPGA 的行為相近、可互相比對。

## 1. 架構圖與模擬器的對應

| 模擬器的概念 | 圖上的元件 | 說明 |
|---|---|---|
| **Main** sequencer | **ISA Main Code**（組合語言／描述子表／常數池）＋ **Main-instruction Decoder** | 執行 MOV、ALU、EXEC_CSR、BR；是唯一能用 ALU、scratchpad 的一側 |
| **Peri** sequencer | **ISA Peri Code**（組合語言／描述子表）＋ **Peri-instruction Decoder** | 執行 EXEC_XFER、EXEC_PUB、EXEC_CSR、BR；沒有 MOV、ALU、scratchpad |
| Main 的「觸發」 | **Frame Timing WCET** ＋ **一個 Timer** → Main 的 Status Flag | 決定 Main 何時能放行：`PGL／AE／LINE_COMMIT_ELIGIBLE`、`FRAME_END_EVT`、`MAIN_TIMER_DONE`、`VBLANK_OPEN`… |
| Peri 的「觸發」 | **一個 Timer**（由 EXEC_CSR 設定與觸發）→ Peri 的 Status Flag | `TIMER_DONE`、Poll（輪詢 I2C 裝置狀態）、`peri.done`／`peri.error` |
| Peri → Main 的資料 | rx_buf →（EXEC_PUB 複製）→ **result_slot** →「to Main」 | `PERI_RESULT_VALID／OVERRUN`；Main 用 MOV 取走 |
| 共用資源 | **Main/Peri CSR Arbiter**、**Data Xfer**、**CSR**、**i2ch**、**i2cm_top** | 兩個 decoder 共用的路徑；目前模擬器**沒有**建模搶資源（見 §4） |

**「觸發」的語意**（依規格與現有模擬器）：兩個 sequencer 平時連續執行（polling 型），由 Timer 與 Frame Timing 產生的**條件旗標**決定分支與動作能否放行；
Timer 是**硬體計時 ＋ BR polling**，由 EXEC_CSR 啟動（`timer_start[:週期]`），不是自行啟動。模擬器已照此運作；本設計要補的是**把這些時間條件做成可調、可觀察、可校正**。

## 2. 現有的可調參數（已存在，要整理成統一介面）

| 群組 | 現在的來源 | 內容 |
|---|---|---|
| 指令延遲 | `Latency`（profile 或環境檔 `[latency]`） | `brCond`、`cascade`、`alu`、`mul`、`movSetup`、`movByte`、`slotConsumeSetup`、`csrAccess`、`csrByte`、`csrDone`、`pubSetup`、`pubByte`、`eventDispatch`、`eret`、`xferIssue` |
| 時脈 | `Latency.cpuHz`、`i2cHz`、環境檔 `[bus i2c]` | ISA 時脈 27 MHz；I2C 400 kHz（67.5 ISA 週期／SCL） |
| 影像時序 | 環境檔 `[timing]` | `frame_cycles`、`vblank_start`／`vblank_len`、`line_cycles`、`hblank_start`／`hblank_len`；`wcet_pgl／ae／line` 訊號 |
| 計時器 | CSR action `timer_start:週期`、`main_timer_start:週期` | 週期由程式啟動時給定 |
| 排程 | 環境檔 `[sim]` | `tie_order`、`unmapped_csr`、`fault_stops` |
| 裝置 | `[device]`、`[csr]`、`[signal]`、`[condition]`、`[inject]` | I2C 裝置腳本、CSR 行為、自訂條件、事件注入 |
| 程式記憶體與表深度 | `Profile` | 描述子表深度、SRAM 配置、PC 寬度、事件數、`csrBufValid`… |
| **可信度分組** | `Profile.confirmed`、`profileGaps()` | 「`confirmed = alu, placement`」＝這些群組已對硬體確認，其餘是**規劃值**並會被列為缺口 |

## 3. 需要新增的參數（為了貼近 FPGA）

下表是我認為「真的會讓模擬與 FPGA 時間不同」的硬體條件。**每一項都做成參數，預設值＝目前行為（不改現有結果）**；實測後再覆寫。
「量法」是在 FPGA 上怎麼測它（見 §6 的校正套件）。

| # | 參數（草案名） | 單位 | 預設 | 意義 | FPGA 上的量法 |
|---|---|---|---|---|---|
| 1 | `main.fetch_cycles`、`peri.fetch_cycles` | ISA 週期 | 0 | 兩個 decoder 取描述子／解碼額外花的週期（SRAM 時脈是 ISA 的 2 倍，取 32 位元字） | 連續 N 條 NOP 型指令的總週期 ÷ N |
| 2 | `csr.arbiter_policy` | 列舉 | `none`（不建模） | CSR Arbiter 的裁決：`main_first`／`peri_first`／`round_robin`／`none` | 兩邊同時 EXEC_CSR 的先後 |
| 3 | `csr.arbiter_penalty` | ISA 週期 | 0 | 被擋的一方多等的週期 | 同上，量等待差 |
| 4 | `csr.access_latency_main`、`_peri` | ISA 週期 | = `csrAccess` | 兩側各自的 CSR 讀寫延遲（可能不同） | 單獨連續 EXEC_CSR |
| 5 | `datapath.contention` | 開關＋週期 | 關 | Data Xfer 被 MOV 與 result_slot 讀取／EXEC_PUB 共用時是否互相等待 | Main 讀 slot 同時 Peri PUB |
| 6 | `slot.copy_cycles`、`slot.valid_delay` | ISA 週期 | = `pubSetup/pubByte` | EXEC_PUB 複製 rx_buf→result_slot 與 `valid` 變 1 的延遲 | 看 Main 何時見到 `PERI_RESULT_VALID` |
| 7 | `flag.sample_delay_main`、`_peri` | ISA 週期 | 0 | 條件來源變化到 BR 看得到之間的延遲（Status Flag 取樣） | 條件翻轉後第幾個週期的 BR 才生效 |
| 8 | `timer.main.clock_div`、`timer.peri.clock_div` | 除數 | 1 | Timer 的計數時脈相對 ISA 時脈 | 設週期 N，量 done 的時間 |
| 9 | `timer.start_latency`、`timer.done_latency` | ISA 週期 | 0 | 寫啟動位元到計時開始、計滿到 done latch 的延遲 | 同上 |
| 10 | `frame.*`（已有）＋ `frame.wcet_pgl／ae／line` | ISA 週期 | 現值 | Frame Timing 的 WCET 值與 commit 資格判斷用的餘裕 | 讀硬體的 frame timing 暫存器 |
| 11 | `i2ch.overhead_cycles` | ISA 週期 | 0 | CSR→i2ch→i2cm_top 啟動 EXEC_XFER 的額外延遲 | XFER 發出到 SCL 第一個脈衝 |
| 12 | `i2c.ack_delay`、`i2c.stretch`、`i2c.turnaround` | SCL 週期／ISA 週期 | 0 | 每個位元組的 ACK 延遲、時脈延展、讀寫轉換 | 邏輯分析儀量 SCL／SDA |
| 13 | `event.dispatch_extra`、`event.pending_to_checkpoint_jitter` | ISA 週期 | 0 | 事件派送與取樣抖動（現有 `eventDispatch`／`eret` 的補充） | 事件注入後量 raised→dispatched |
| 14 | `reset.boot_cycles` | ISA 週期 | 0 | 重置解除到第一條指令發出 | 看第一次 CSR 寫的時間 |

> 以上名稱與預設值是**草案**。新增參數不會改變現有測試與範例結果（預設＝現行行為）。第 2、5 項（搶資源）是**行為**而不只是數字，要先確認硬體真的有這種競爭才建模。

## 4. 目前模擬器沒建模、但圖上有的事

1. **CSR Arbiter 裁決**：兩個 decoder 經同一個 Arbiter 存取 CSR；現在是各自獨立計時，不會互相等待。
2. **Data Xfer 共用**：Main 的 MOV 與 result_slot／EXEC_PUB 共用同一個資料通道；現在不會互相卡。
3. **i2ch 的額外延遲**。
4. **Status Flag 的取樣延遲**。

這四項預設都「關閉＝與現在相同」，由參數開啟，這樣模擬器可以在不確定硬體行為時維持現狀，確認後逐項打開。

## 5. 視覺化設計

### 5.1 ISA Top 即時面板（主畫面）
- 版面照架構圖；每個方塊顯示即時數值：兩個 decoder 的 PC 與目前指令、scratchpad 位元組、ALU 的 A／B／Out 與旗標、result_slot（valid／service_id／長度）、csr_buf、rx_buf、兩個 Timer 的計數與 done、Frame Timing 的 vblank／hblank 狀態、Status Flag 的各條件。
- 單步時，該指令走過的資料路徑亮起並以箭頭播放；數值變化閃一下；兩個 decoder 各自高亮正在執行的指令。
- 點方塊跳到對應面板（scratchpad→Memory、CSR→CSRs），點 decoder 跳到程式行或積木。
- 圖以**資料檔**描述（方塊、連線、位置、對應的模擬器狀態），之後 ISA Top 設計改了，改資料檔即可。

### 5.2 雙軌時間軸
- 兩條軌道（Main、Peri）共用週期軸；指令畫成方塊（寬＝延遲）；EXEC_XFER 拉長；事件的提出→派送→返回、PUB→slot→Main 消耗、Timer 啟動→done 畫成互動標記。
- 需要核心提供記憶體內的軌跡環形緩衝（最近 N 個動作）。
- **疊圖比對**：可載入 FPGA 擷取（見 §6）疊在模擬軌跡上，一眼看出哪裡差多少。

### 5.3 參數面板
- 依群組列出 §2、§3 的所有參數：名稱、單位、目前值、預設值、範圍、說明、**狀態**（規劃值／已校正／實測值）、來源與日期。
- 改了參數按「套用」就重置模擬器並沿用同一個程式與情境重新跑；可存成**目標檔**。
- 每個參數旁邊有「怎麼在 FPGA 上量」的簡短提示（§3 的「量法」）。

### 5.4 編輯器與積木
- 文字與積木上顯示兩個執行游標（Main、Peri 各一個顏色）；積木畫布上 Main、Peri 的帽子有即時狀態標籤（執行中／停止／等匯流排／等結果槽／fault）。
- 單步：**Step**（全域時間順序，誰最早就跑誰）、**Step Main**、**Step Peri**；命中中斷點兩邊一起暫停。

## 6. FPGA 校正流程

1. **目標檔**（`*.simtarget`）：一份檔案裝所有參數＋出處（規劃／實測）＋量測日期＋板子與位元檔版本。擴充現有 `Profile.confirmed`／`profileGaps()`：每一群組標示「已用 FPGA 校正」與否，未校正的列為缺口（不是錯誤）。
2. **校正套件**：一組小型組合語言程式（每個只量一個參數）：連續 N 條 MOV／ALU／EXEC_CSR、Timer 啟動到 done、EXEC_XFER 位元組數掃描、兩邊同時 EXEC_CSR、PUB→Main 取用……，附說明「在 FPGA 上用什麼量（ILA、邏輯分析儀、CSR 內的週期計數器）」與預期的算式。
3. **匯入實測**：支援匯入 CSV／VCD（ILA 或邏輯分析儀匯出，含時間戳記與訊號名稱對照表）。
4. **比對報告**：用同一個程式與情境在模擬器跑，與 FPGA 擷取逐事件比對，列出每個參數的差異與**建議修正值**（例如由 N 條指令的總週期反推每條延遲）；可一鍵套用到目標檔。
5. **疊圖**：把兩份軌跡疊在時間軸上看。
6. **回歸**：目標檔存進專案；之後任何程式都可選用該目標檔模擬，結果與 FPGA 的差異有已知上限。

## 7. 工作項目（建議順序）

| 階段 | 項目 | 說明 |
|---|---|---|
| **S1** | 參數資料庫 | 把 §2 的現有參數整理成一份有中繼資料（群組、單位、預設、範圍、狀態、說明）的表；環境檔／profile 都經由它讀寫；加入 §3 的新參數（預設＝現行行為，不改既有結果）；單元測試（預設不改結果） |
| **S2** | 目標檔與參數面板 | `*.simtarget` 的讀寫、IDE 參數面板（編輯、套用、重置、存檔、狀態標示） |
| **S3** | 軌跡記錄 | 核心記憶體內軌跡環形緩衝（指令發出／結束、事件、PUB／slot、Timer、CSR）；供視覺化與比對共用 |
| **S4** | ISA Top 即時面板 | 資料檔驅動的架構圖、即時數值、單步路徑動畫、點選跳轉 |
| **S5** | 雙游標與單步控制 | 編輯器與積木的 Main／Peri 游標、狀態標籤、Step／Step Main／Step Peri |
| **S6** | 雙軌時間軸 | 依 S3 的軌跡畫兩條軌道與互動標記 |
| **S7** | 搶資源建模 | 依硬體確認的結果打開 §4 的行為（Arbiter、Data Xfer、i2ch、Status Flag 取樣） |
| **S8** | 校正套件與匯入比對 | 校正程式、CSV／VCD 匯入、比對報告、建議值、疊圖 |

## 8. 待使用者確認

1. §1 的對應與「觸發」語意是否正確（尤其 Main 是否只由 Frame Timing WCET ＋ 一個 Timer 放行）？
2. §3 的參數清單有沒有漏的硬體條件？哪些你已知道實際值（可直接當預設）？
3. 搶資源（§4）硬體上是否真的存在？Arbiter 的裁決規則是什麼？
4. FPGA 上你打算用什麼量測（ILA、邏輯分析儀、CSR 內的週期計數器）？匯出格式（CSV／VCD）為何？這決定 S8 的匯入格式。
5. 工作順序是否照 §7？（我建議先做 S1、S2，因為它們不依賴畫面，之後所有視覺化都建立在參數資料庫上。）

## S4 實作：ISA Top 即時圖（已完成）

位置：**模擬器分頁**（`Ctrl+Shift+M`）右半的上方，時間軸在它下面。

- **畫的是**：Main（ISA Main Code → Main-instruction Decoder，ALU、MAIN_SCRATCHPAD、MAIN_CSR_DATA_BUF、Main Status Flag，放行它的 Frame Timing WCET 與 Main Timer）、Peri（ISA Peri Code → Peri-instruction Decoder，Peri Timer、輪詢、PERI_RX_BUF、result_slot → Main）、共用的 CSR Arbiter／CSR／Data Xfer／i2ch／i2cm_top／I2C 裝置。每個方塊的列是**模擬器目前的值**（PC、下一條指令、暫存區位元組、ALU 的 OPA／OPB／OUT 與旗標、slot、Timer、Frame Timing、輪詢旗標、CSR）。
- **動態**：每執行一條指令，那條指令走過的**連線會亮起**（Main 琥珀色、Peri 青色，帶流動的虛線，約 0.7 秒淡出），對應的 decoder 方塊閃一圈；值**改變的那一列會黃色閃一下**。配合 `sim.speed` 調慢就能一條一條看；全速時就是連續發亮。停在斷點時，圖停在那一刻。
- **點選**：點方塊開對應的面板（scratchpad／csr_buf／rx_buf／slot → Memory；CSR → CSRs；ALU／Timer／旗標 → State／Signals）；點 **decoder** 跳到它正在執行的那一行（積木程式就是那個積木）。滑鼠停在方塊上有說明。
- **縮放**：「整張」把整張圖放進視窗，「符合寬度」讓字比較大（其餘用捲動），「100%」，`Ctrl+滾輪`縮放，空白處雙擊 = 整張。
- **圖是資料**：方塊、列、連線、位置、「某指令點亮哪些連線」都在一份文字檔（`services/isa_layout.cpp` 內建的預設，格式見 `services/isa_layout.h`）。ISA Top 的設計改了，改這個檔就好。要用自己的檔：設定 `isatop.layout` = 檔案路徑（格式同預設；讀不到或有錯就維持內建的圖，錯誤寫進輸出面板）。預設布局是依我手上對你的架構圖的描述排的，**方塊位置與連線若和你的圖不一樣，直接改檔**。
- **還沒建模的**：CSR Arbiter 與 Data Xfer 的搶資源（圖上標「not modelled yet」），等你確認硬體真的有這種競爭與規則（見 §8 的問題）。
