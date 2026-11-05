# 14_main_peri_sim　Main 與 Peri 兩段組合語言一起跑

給**模擬器頁面**看的範例：一個專案裡同時有 `.main` 與 `.peri` 兩段程式，彼此用「結果槽」與硬體計時器配合。

| 誰 | 做什麼 | 什麼時候動 |
|---|---|---|
| **Peri** | 啟動自己的 Timer（600 ticks）→ 等 Timer 跑完 → `EXEC_XFER` 經 I2C 讀 6 bytes 陀螺儀 → `EXEC_PUB` 發佈到結果槽 → 重來 | 由 **Peri Timer** 觸發 |
| **Main** | 等結果槽有效 → 取出 → 啟動**自己的 Timer**（40 ticks）等它跑完 → 等 **Frame Timing** 的消隱視窗（`PGL_COMMIT_ELIGIBLE`，和 WCET 比較）→ 算校正值 → 寫 CSR `GYRO_X_CORR_L` | 由 **Main Timer + Frame Timing WCET** 觸發 |

## 怎麼玩
1. 開專案 `14_main_peri_sim.acproj`（專案樹雙擊）。它已登記 `simparams = main_peri.simparams`。
2. `Ctrl+Shift+M` 開**模擬器頁面**：上面是 ISA Top 架構圖，下面是 Main／Peri 兩條時間軸。參數在方塊或積木上按右鍵「設定參數…」，或用 ISA Top 標題列的「所有參數…」。
3. 改參數看差異（基準軌道是初始值跑出的）。**每條指令都先從 I2C EEPROM 讀出來才執行**（`fetch.read_mode = sequential`，一條約 6,000～12,000 週期），所以畫面用規格 14 章的實際時序：27 MHz／60 fps，一個畫面 450,000 週期，消隱視窗是最後 14,362 週期（435,638～450,000）。這份參數下：Peri 在第 108,814 週期發佈，Main 在 206,688 取走，在第一個畫面的消隱視窗通過 `PGL_COMMIT_ELIGIBLE`，**第 531,292 週期才寫入 CSR**。
   - **注意這個結果**：檢查視窗之後還有好幾條指令（算校正值、寫 CSR），每條都要讀 EEPROM，寫入時已經離開消隱視窗了。照這個硬體架構，「先檢查視窗、再做好幾條指令才提交」可能來不及。
   - `fetch.read_mode` 改 `per_byte`（每個 byte 各讀一次）：發佈延到 157,474，提交延到 1,488,398。改 `instant`（取指令不花時間，舊模型）：發佈 6,686，提交 435,653。
   - `frame.wcet_pgl`：視窗剩餘時間不夠 WCET 就不算「可提交」。調到 20000（超過視窗長度 14,362）→ `PGL_COMMIT_ELIGIBLE` 永遠不成立，Main 一直卡在 `window:`，不提交（情境檔最後一項檢查會失敗，這正是要看到的）。
   - `i2c.stretch`、`i2c.ack_delay`、`clock.i2c_hz`：I2C 匯流排變慢或變快，取指令和讀感測器都跟著變，整個時間軸等比例移動。
   - `clock.timer_hz`、`flag.arbiter_policy`、`csr.arbiter_policy`、`timer.prescaler`、`timer.count`：這些只差 1 到幾千週期，在讀法 A 下被取指令的時間蓋過，看不出差別；改 `fetch.read_mode = instant` 再試就看得到。
   - Decoder 解碼面板（View 選單）：看兩個 Decoder 從 EEPROM 解出的每一條指令，和原始碼對照。
4. 想看單步：Debug → Start Debugging，Main／Peri 的暫存器與事件都在面板裡。
5. 命令列：`asmsim main_peri.asm -I ../common --env main_peri_env.cfg --params main_peri.simparams --scenario main_peri.scn`。

CSR 位址（`MAIN_TPER` 0x320…）是**範例值**，和 `common/defs.inc` 一樣。
