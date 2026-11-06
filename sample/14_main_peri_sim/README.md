# 14_main_peri_sim　Main 與 Peri 兩段組合語言一起跑

給**模擬器頁面**看的範例：一個專案裡同時有 `.main` 與 `.peri` 兩段程式，彼此用「結果槽」與硬體計時器配合。

| 誰 | 做什麼 | 什麼時候動 |
|---|---|---|
| **Peri** | 啟動自己的 Timer（600 ticks）→ 等 Timer 跑完 → `EXEC_XFER` 經 I2C 讀 6 bytes 陀螺儀 → `EXEC_PUB` 發佈到結果槽 → 重來 | 由 **Peri Timer** 觸發 |
| **Main** | 等結果槽有效 → 取出 → 啟動**自己的 Timer**（40 ticks）等它跑完 → 等 **Frame Timing** 的消隱視窗（`PGL_COMMIT_ELIGIBLE`，和 WCET 比較）→ 算校正值 → 寫 CSR `GYRO_X_CORR_L` | 由 **Main Timer + Frame Timing WCET** 觸發 |

## 怎麼玩
1. 開專案 `14_main_peri_sim.acproj`（專案樹雙擊）。它已登記 `simparams = main_peri.simparams`。
2. `Ctrl+Shift+M` 開**模擬器頁面**：上面是 ISA Top 架構圖，下面是 Main／Peri 兩條時間軸。參數在方塊或積木上按右鍵「設定參數…」，或用 ISA Top 標題列的「所有參數…」。
3. 改參數看差異（基準軌道是初始值跑出的）。**每條指令都先從 I2C EEPROM 讀出來才執行**（`fetch.read_mode = sequential`，一條約 6,000～10,000 週期），所以畫面用規格 14 章的實際時序：27 MHz／60 fps，一個畫面 450,000 週期，消隱視窗是最後 14,362 週期（第一個畫面 435,638～450,000）。這份參數下：Peri 在第 111,854 週期發佈，Main 在 209,728 取走，第 458,953 週期寫入 CSR。
   - `fetch.read_mode` 改 `per_byte`（每個 byte 各讀一次）：發佈 160,514，寫入 2,270,294。改 `instant`（取指令不花時間，舊模型）：發佈 6,686，寫入 435,640（在視窗內）。
   - `frame.wcet_pgl`：視窗剩餘時間不夠 WCET 就不算「可提交」。超過視窗長度（例如 20000）→ `PGL_COMMIT_ELIGIBLE` 永遠不成立，Main 一直卡在 `window:`，不提交（情境檔最後一項檢查會失敗，這正是要看到的）。
   - `i2c.stretch`、`i2c.ack_delay`、`clock.i2c_hz`：I2C 匯流排變慢或變快，取指令和讀感測器都跟著變，整個時間軸等比例移動。
   - `clock.timer_hz`、`flag.arbiter_policy`、`csr.arbiter_policy`、`timer.prescaler`、`timer.count`：這些只差 1 到幾千週期，在讀法 A 下被取指令的時間蓋過，看不出差別；改 `fetch.read_mode = instant` 再試就看得到。
   - Decoder 解碼面板（View 選單）：看兩個 Decoder 從 EEPROM 解出的每一條指令、各要讀多少週期，和原始碼對照。

4. 想看單步：Debug → Start Debugging，Main／Peri 的暫存器與事件都在面板裡。
5. 命令列：`asmsim main_peri.asm -I ../common --env main_peri_env.cfg --params main_peri.simparams --scenario main_peri.scn`。

## 提交能不能落在消隱視窗內

**寫法**：校正值（`MOV`、`ALU SUB`…共 5 條）在 `take:` 就先算好，`commit:` 通過 `PGL_COMMIT_ELIGIBLE` 之後**只剩寫入 CSR 那一條**（`EXEC_CSR WRITE_SP`）。舊寫法把 5 條計算放在檢查之後，每條都要先讀 EEPROM，寫入時早就離開視窗（超出約 88,000 週期）；新寫法超出的量縮小到幾千週期。

**但在 400 kHz 的共用 I2C 匯流排下，仍然無法保證**。通過檢查後，Main 要讀 `WRITE_SP` 之前，常常得先等 Peri 用完匯流排（Peri 一直在輪詢，每條指令都要讀 EEPROM）：

| 通過檢查到寫入（最壞情況） | 400 kHz | 1 MHz |
|---|---|---|
| 等 Peri 用完匯流排（最長是 Peri 讀 `EXEC_XFER`：程式碼 2 + 描述子 2 + DeviceMap 1 byte） | 10,334 | 4,137 |
| Main 讀 `WRITE_SP`（程式碼 2 + 描述子 2 byte） | 7,294 | 2,920 |
| 執行 `WRITE_SP` | 6 | 6 |
| **合計＝`frame.wcet_pgl` 該設的值** | **17,634**（比視窗 14,362 還長） | **7,063** |

實測（`--param frame.wcet_pgl=…`，看寫入時 `vblank_open`）：

| 設定 | 寫入週期 | 在視窗內？ |
|---|---|---|
| 400 kHz，WCET 120（這份參數） | 458,953 | 否，超出 8,953 |
| 400 kHz，WCET 7,400（只算寫入本身） | 904,542 | 否，等了 Peri 7,294 週期，超出 4,542 |
| 400 kHz，WCET 14,000 | 40,049,626 | 是，但第 89 個畫面才碰巧成功 |
| 400 kHz，WCET 17,634（正確的保證值） | 不提交 | 比視窗還長，永遠不符合條件 |
| **1 MHz，WCET 7,063** | **897,285** | **是**（第二個畫面的視窗 885,638～900,000），有保證 |

重現：`asmsim main_peri.asm -I ../common --env main_peri_env.cfg --params main_peri.simparams --param clock.i2c_hz=1000000 --param frame.wcet_pgl=7063 --cycles 1000000 --decoded`。

**結論**：照目前的硬體模型（所有指令經 400 kHz I2C 從 EEPROM 讀、Peri 與 Main 共用匯流排），要保證提交落在消隱視窗內，需要硬體改變其中一項：I2C 改 1 MHz、Main 取指令有自己的匯流排或比 Peri 優先且能中斷、或 Decoder 從 SRAM／快取取指令（`fetch.source = sram` 或 `fetch.read_mode = instant` 可模擬）。只讓 Main 在匯流排上優先而不能中斷，最壞仍要等 Peri 一次讀取（10,334），不夠。

CSR 位址（`MAIN_TPER` 0x320…）是**範例值**，和 `common/defs.inc` 一樣。
