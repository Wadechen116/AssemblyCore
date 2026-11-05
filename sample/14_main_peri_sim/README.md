# 14_main_peri_sim　Main 與 Peri 兩段組合語言一起跑

給**模擬器頁面**看的範例：一個專案裡同時有 `.main` 與 `.peri` 兩段程式，彼此用「結果槽」與硬體計時器配合。

| 誰 | 做什麼 | 什麼時候動 |
|---|---|---|
| **Peri** | 啟動自己的 Timer（600 ticks）→ 等 Timer 跑完 → `EXEC_XFER` 經 I2C 讀 6 bytes 陀螺儀 → `EXEC_PUB` 發佈到結果槽 → 重來 | 由 **Peri Timer** 觸發 |
| **Main** | 等結果槽有效 → 取出 → 啟動**自己的 Timer**（40 ticks）等它跑完 → 等 **Frame Timing** 的消隱視窗（`PGL_COMMIT_ELIGIBLE`，和 WCET 比較）→ 算校正值 → 寫 CSR `GYRO_X_CORR_L` | 由 **Main Timer + Frame Timing WCET** 觸發 |

## 怎麼玩
1. 開專案 `14_main_peri_sim.acproj`（專案樹雙擊）。它已登記 `simparams = main_peri.simparams`。
2. `Ctrl+Shift+M` 開**模擬器頁面**：上面是 ISA Top 架構圖，下面是 Main／Peri 兩條時間軸。參數在方塊或積木上按右鍵「設定參數…」，或用 ISA Top 標題列的「所有參數…」。
3. 改參數看差異（基準軌道是初始值跑出的）。這份參數下（兩個 Timer 都數 13.5 MHz，即 27 MHz 除 2，每個 tick 2 個 ISA 週期）：Peri 在第 6686 週期發佈，Main 在 6708 取走，**在第二個畫面的消隱視窗（7000～7900）的 7015 提交**。
   - `clock.timer_hz` 改 6750000（每 tick 4 個 ISA 週期）：兩個 Timer 一起慢一倍，發佈延到 7884，越過了 7000 的消隱視窗，提交延到下一個畫面的 11015。
   - `flag.arbiter_policy`（初始值 `main_first`：同一週期 Main 先）改 `none`：Main 與 Peri 的 BR 不再搶同一個 Status Flag 單元，發佈早 1 週期（6685），取用早 2 週期（6706）。
   - `csr.arbiter_policy`（初始值 `main_first`：先到先得，同時抵達 Main 先）改 `none`：Main 寫 CSR 時不必等 Peri，提交早 1 週期（7014）。
   - `timer.prescaler`（`free_running`／`restart`）、`timer.count`（`n`／`n_plus_1`）：Timer 的 tick 格點與週期計數，等 RTL 確認。
   - `i2c.stretch` 改 20、`i2ch.overhead`、`i2c.ack_delay`：I2C 讀取變長，發佈時間往後移。
   - 注意：**提交時間不會跟著發佈連續移動**，它被 Frame Timing 量化到消隱視窗；發佈再晚，只要還在 7000 之前，提交就還是 7015 附近。想讓提交跟著動，要讓發佈越過 7000，或改 `frame.wcet_pgl`。
   - `frame.wcet_pgl`：視窗剩餘時間不夠 WCET 就不算「可提交」。調到 1000（超過視窗長度 900）→ `PGL_COMMIT_ELIGIBLE` 永遠不成立，Main 一直卡在 `window:`，不提交（情境檔最後一項檢查會失敗，這正是要看到的）。
   - `timer.main.start_latency`、`csr.access_main`、`main.fetch_cycles`：Main 側的節奏。
4. 想看單步：Debug → Start Debugging，Main／Peri 的暫存器與事件都在面板裡。
5. 命令列：`asmsim main_peri.asm -I ../common --env main_peri_env.cfg --params main_peri.simparams --scenario main_peri.scn`。

CSR 位址（`MAIN_TPER` 0x320…）是**範例值**，和 `common/defs.inc` 一樣。
