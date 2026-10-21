# 02_imu_gyro

完整的 Main + Peri 流程（對應規格 13.2）：Peri 在 `poll_loop` 啟動並 arm Poll、讀狀態、再讀 6 位元組陀螺儀資料並發佈；Main 等到結果槽有效，
比對 service id，把資料取出、**交換高低位元組**（感測器先送高位元組，ALU 運算元是小端）、減去 bias、用 `WRITE_SP` 寫到 CSR。

重點：`common/defs.inc` 宣告了 `.service SID_IMU_GYRO_RAW = 1, len=6, big` 與 `.csrbit`（Timer／Poll 的角色），
所以組譯器能檢查位元組序（W514）、計時器啟動（W515）、Poll arm（W516）、發佈／消耗長度（W517）。**這個專案零警告就是通過。**

## 試試看
1. **建置**：零警告。
2. **讓警告出現**（每次改完看問題面板，再復原）：
   - 刪掉 `MOV … [8..9]` 前的兩行交換 → **W514**（位元組序）。
   - 把 `EXEC_PUB … len=6` 改成 `len=4` → **W517**、W507、W511。
   - 把 `SET8 PERI_CTRL, PERI_TIMER_START` 那行刪掉 → **W515**。
   - 刪掉 `SET8 POLL_CTRL, POLL_ARM_W1P` → **W516**。
3. **除錯**：在 Peri 的 `read_data:` 設中斷點，Debug 選單把「Step focus」切到 Peri；F5 → 停在讀 6 位元組之前，看 **Memory** 的 `rx`（空）；單步後 `rx[0..5]` 變成 `00 0A 00 14 00 1E`。
4. **Signals 面板**：看 `poll.ready`、`slot.valid`、`timer.done` 隨時間變化；在 **CSRs** 面板看 `0x120` 最後變成 10。
5. **環境檔**：Debug → Edit Environment File…，把 `reg 0x3A = 0x00, 0x00, 0x01` 改成 `0x00` 一直不到就緒 → 迴圈不結束（用 F6 暫停）。
6. **NACK 注入**：在環境檔加 `[inject]` 段 `at 2000 = nack dev=0 count=1`，觀察 `peri.error`。
7. **事件記錄面板**：篩選 `bus`，看每筆 I2C 交易；匯出 VCD（用 GTKWave 等開）。

## 預期
`asmsim -I ../common imu_gyro.asm --env imu_env.cfg --scenario imu_gyro.scn` → `PASS (8 checks)`。
