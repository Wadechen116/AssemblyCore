# 計時器規格：兩顆獨立的計時器

建立日期：2026-11-20 ・ 狀態：**使用者定義**（2026-11-20）・ 硬體驗證：**尚未執行**

這份文件把「Main 與 Peri 各有一顆獨立的計時器」定為本專案的規格，並記錄如何驗證硬體符合這個定義。
在此之前，這一點是依規格書中兩個不同的名稱（`MAIN_TIMER_DONE`、`IF_TIMER_DONE`）所做的假設（`docs/SIM_VS_SPEC.md` 的 Q1）。

> 廠商的規格書（`MicroISA_Spec_v31.pdf`、交接文件）不是本專案的檔案，沒有被修改。這份文件是本專案對它的補充定義；
> 規格書中與計時器有關的原文見第 5 節。

---

## 1. 定義

### 1.1 兩顆計時器

| | Main 計時器 | Peri 計時器 |
|---|---|---|
| 使用者 | Main sequencer | Peri sequencer |
| 完成旗標 | `MAIN_TIMER_DONE`（Main 的分支條件 0x7） | `IF_TIMER_DONE`／`IF_TIMER_NOT_DONE`（Peri 的分支型別 6／7） |
| 週期 | 自己的週期暫存器 | 自己的週期暫存器 |
| 啟動 | 自己的啟動位元 | 自己的啟動位元 |

**T-1（獨立）** 兩顆計時器各自擁有週期、計數狀態與完成旗標。對其中一顆的任何操作（設定週期、啟動、再次啟動、清除）都**不影響**另一顆的計數與完成旗標。

**T-2（歸屬）** Main 只能以 `MAIN_TIMER_DONE` 觀察 Main 計時器；Peri 只能以 `IF_TIMER_DONE`／`IF_TIMER_NOT_DONE` 觀察 Peri 計時器。兩個完成旗標是兩個不同的旗標。

**T-3（唯一共用的是時脈）** 兩顆計時器數同一個時脈 `clock.timer_hz`（13.5 MHz＝ISA 時脈 27 MHz 除以 2，每個 tick 2 個 ISA 週期；使用者確認，2026-11-04）。共用時脈不構成互相影響：一顆的啟動不改變另一顆的 tick。

**T-4（不是共用資源）** 計時器不經過任何仲裁；一邊啟動或等待自己的計時器，不會讓另一邊等待。

### 1.2 單顆計時器的行為（兩顆相同）

**T-5（啟動）** 啟動時完成旗標立刻清為 0；經過「週期」個 tick 之後完成旗標設為 1，並保持到下一次啟動或清除。

**T-6（再次啟動）** 計時中再啟動一次：從頭開始計時，前一次的到期作廢。完成後再啟動：完成旗標先清為 0，再重新計時。

**T-7（清除）** 清除會把完成旗標清為 0，並使進行中的計時作廢。

**T-8（輪詢，不是中斷）** 計時器到時**只設完成旗標**，不會觸發硬體事件，也不會改變程式流程；程式必須用分支指令輪詢完成旗標（規格書：「不要發明 timer interrupt」）。

**T-9（未啟動）** 完成旗標為 0 不代表計時器已經啟動；沒有「計時器有效」旗標。以週期 0 啟動不會計時。

### 1.3 這份定義不包含的

下列項目由板子或 RTL 決定，仍是規劃值或待確認，不屬於這份定義：

| 項目 | 現況 |
|---|---|
| 週期暫存器、啟動／清除位元的 CSR **位址與位元** | 規格書只給名稱；由專案的環境檔與 `conformance/conf_board.inc` 提供（目前是範例值） |
| 週期暫存器的寬度 | 目前以 2 個位元組（低、高）處理 |
| 預除器是自由運轉還是啟動時重新開始（`timer.prescaler`） | 規劃值：`free_running` |
| 週期 N 實際數 N 個還是 N＋1 個 tick（`timer.count`） | 規劃值：`n` |
| 相位、啟動延遲、完成延遲（`timer.phase`、`timer.*.start_latency`、`timer.*.done_latency`） | 規劃值：0 |

---

## 2. 模擬器如何實作這份定義

| 定義 | 模擬器的做法 | 程式位置 |
|---|---|---|
| T-1、T-2 | 兩組各自的信號：Peri 用 `timer_period`、`timer.done`、`timer.gen`；Main 用 `main_timer_period`、`main_timer.done`、`main_timer.gen`。動作 `timer_start`／`timer_clear` 只動 Peri 的一組，`main_timer_start`／`main_timer_clear` 只動 Main 的一組 | `core/src/sim/builtin.cpp`（`startTimer`、`Extensions::defaults`） |
| T-2 | Main 的條件 0x7 讀 `main_timer.done`；Peri 的型別 6／7 讀 `timer.done` | `core/src/sim/impl.cpp`（`buildDefaultConditions`） |
| T-3 | 兩顆都以 `clock.timer_hz` 與同一個預除器模型計算到期時刻 | `builtin.cpp`（`timerDoneAt`） |
| T-4 | 計時器不在共用資源的仲裁之內 | `core/src/sim/run.cpp`（`gate` 只處理 CSR 仲裁器、Data Xfer、Status Flag、I2C 匯流排） |
| T-5、T-6 | 啟動時把完成旗標清為 0，並把「世代」加 1；到期事件帶著啟動時的世代，世代不符就作廢 | `builtin.cpp`（`startTimer`） |
| T-7 | 清除：世代加 1、完成旗標清為 0 | `builtin.cpp` |
| T-8 | 到期只設信號；事件只能由外部注入 | `run.cpp`（`raiseEvent` 只有注入一個來源） |
| T-9 | 週期為 0 時不排到期事件，只留一則提示 | `builtin.cpp`（`startTimer`） |

詳細的逐項對照見 `docs/SIM_VS_SPEC.md` 2.12、2.15.7、2.16.2。

---

## 3. 驗證方式

### 3.1 測試程式

`conformance/b01_timers`（原始碼 `b01_timers.asm`，由 `conformance/make_conformance.py` 產生；預期結果在 `conformance/EXPECTED.md`）。

它驗證 T-1、T-2、T-5、T-6，而且**只看寫入的先後順序，不需要量時間**。

### 3.2 程式做什麼

```
Peri：設定長週期（65000 tick）→ 啟動 → 等完成 → 再啟動 → 等完成 → 寫入標記 RES+8 = 0x21
Main：設定短週期（2000 tick） → 啟動 → 等完成 → 寫入標記 RES+0 = 0x11
                              → 再啟動 → 等完成 → 寫入標記 RES+1 = 0x12
      然後設定長週期，連等三次 → 寫入完成標記 RES_DONE = 0xA5
```

兩邊同時開始。Peri 的兩次長計時遠比 Main 的兩次短計時久。

### 3.3 判定準則

**通過（符合「兩顆獨立」的定義）必須同時成立：**

| # | 條件 | 驗證的定義 |
|---|---|---|
| V-1 | `RES+0` 的第一次寫入早於 `RES+1` 的第一次寫入 | T-5、T-6：Main 的計時器可以再次啟動，每次都重新計時 |
| V-2 | `RES+1` 的第一次寫入早於 `RES+8` 的第一次寫入 | **T-1、T-2**：Main 啟動自己的計時器沒有影響 Peri 正在等的計時 |
| V-3 | `RES+8` 的第一次寫入早於 `RES_DONE` 的寫入 | Peri 的計時器確實會完成，而且 Peri 看得到 |
| V-4 | 最後的值：`RES+0 = 0x11`、`RES+1 = 0x12`、`RES+8 = 0x21`、`RES_DONE = 0xA5` | 程式走完 |

**不通過時的判讀：**

| 觀察到的現象 | 結論 |
|---|---|
| `RES+8` 比 `RES+1`（甚至 `RES+0`）早出現 | **硬體只有一顆計時器**：Main 啟動時蓋掉了 Peri 正在等的那一次，Peri 跟著 Main 的短計時一起完成。違反 T-1 |
| `RES+8` 一直沒有出現 | 一顆共用的計時器，而且完成旗標只有啟動它的一方看得到，或被另一方的啟動清掉。違反 T-1、T-2 |
| `RES+0` 與 `RES+1` 幾乎同時出現 | 再次啟動時完成旗標沒有被清除。違反 T-5、T-6（此時 V-2 無法用來判斷是否獨立，見 3.6） |
| `RES+0` 一直沒有出現 | 計時器沒有啟動：`conf_board.inc` 的位址或位元與板子不符 |

時間上的參考（不列入判定）：`RES+0` 約在開始後 2000 個 tick 出現，`RES+8` 約在 2 × 65000 個 tick 之後。

### 3.4 在硬體（RTL 模擬或 FPGA）上怎麼做

1. **先改 `conformance/conf_board.inc`**：把 `MAIN_TPER`、`MAIN_TPER_H`、`MAIN_CTRL`、`PERI_TPER`、`PERI_TPER_H`、`PERI_CTRL` 的位址，以及 `MAIN_TIMER_START`、`PERI_TIMER_START`、`PERI_RESET` 的位元，改成板子實際的數值。目前是範例值。
2. 產生映像：`python3 conformance/make_conformance.py --bin-dir <工具資料夾>`，取 `conformance/out/b01_timers/` 下適合測試平台的格式（Main 與 Peri 兩份映像都要）。
3. 執行到 `RES_DONE`（預設 `0x67F`）被寫入 `0xA5`，記錄期間所有 CSR 寫入與發生的先後。
4. 依 3.3 的 V-1～V-4 判定。只需要比較 `RES+0`、`RES+1`、`RES+8`、`RES_DONE` 四個位址**第一次被寫入的先後**。

如果板子的計時器介面與範例不同（例如週期不是兩個位元組、啟動不是寫 1），程式也要跟著改；請回報實際的介面。

### 3.5 這個測試本身可靠嗎

| 檢查 | 結果 |
|---|---|
| 模擬器（兩顆獨立）執行 `b01` | V-1～V-4 全部成立；在「取指不花時間」與「每道指令經 I2C 讀 EEPROM」兩種模式下都成立，表示順序不依賴取指有多慢 |
| **反向驗證**：以環境檔把 Main 的計時器 CSR 接到 Peri 的計時器，並讓 `MAIN_TIMER_DONE` 讀同一個完成旗標（等於只有一顆共用計時器），再執行 `b01` | **V-2 不成立**（`RES+8` 早於 `RES+1`）——測試確實分辨得出一顆與兩顆 |
| 自動化 | 以上兩項都在 `ctest` 的 `conformance_kit` 裡，每次建置都會重跑；模擬器若被改成不符合這份定義，測試會失敗 |
| Windows 與 Linux 的工具 | 產生的映像逐位元組相同 |

### 3.6 限制

- **尚未在任何 RTL 或實體硬體上執行。** 這份定義目前只由模擬器實作並自我驗證；硬體是否符合，要等 3.4 的結果。
- 如果硬體「再次啟動時不清除完成旗標」，Main 的第二次等待會立刻結束，V-2 仍可能成立，此時無法由 `b01` 判斷是否獨立；3.3 的判讀表列出了這種情形的徵狀（`RES+0` 與 `RES+1` 幾乎同時）。
- `b01` 不驗證 T-3（共用時脈的頻率）、T-7（清除）、T-8（不觸發事件）、T-9（未啟動），也不驗證任何時間長度。時間屬於第 3 層（`docs/SIM_VS_SPEC.md` 第 4 節）。
- 測試用到的 CSR 位址與位元是範例值，必須先換成板子的。

---

## 4. 這份定義帶來的結論

- `docs/SIM_VS_SPEC.md` 的 **Q1 已有答案**（依本定義：兩顆）；Q1a–Q1c 是「若只有一顆」才需要的問題，不再適用。
- `docs/PROJECT_STATUS.md` 7.3 的 C1–C3 與原訂排在最後的 **`timer.shared` 實驗不再需要**。只有在硬體驗證（3.4）顯示相反的結果時才重新開啟。
- 範例 14（`sample/14_main_peri_sim`）讓 Main 與 Peri 各用自己的計時器，符合這份定義。
- 計時器到時不產生事件（T-8），所以 Q3（事件與計時器的關係）依本定義為「沒有關聯」。

## 5. 規格書中的相關原文

| 出處 | 內容 |
|---|---|
| 交接文件，Main 條件表 | `0x7 MAIN_TIMER_DONE`：Main timer done latch |
| 交接文件，Peri 分支表 | `0x6 IF_TIMER_DONE`：timer_done_latch；`0x7 IF_TIMER_NOT_DONE`：`!timer_done_latch` |
| 交接文件第 6 節「Peri Poll、Hardware Timer、Error/Done（不要發明 timer interrupt）」 | Timer 是硬體計時＋BR polling；用合法的 EXEC_CSR start／restart／clear；`BR.IF_TIMER_DONE`／`IF_TIMER_NOT_DONE` 只是 sample latch，不能隱含啟動 Timer |
| 同上 | `timer_done=0` 不代表 timer 已 start；baseline 沒有 mandatory `timer_valid_q` 或 `TIMER_NOT_VALID` fault。新 service 應在 poll loop 外 start；不要每次 retry 都 restart 同個 timeout timer |

規格書沒有明寫 Main 與 Peri 的計時器是否為同一顆；它使用了兩個不同的名稱。「兩顆獨立」是本專案在 2026-11-20 的定義。
