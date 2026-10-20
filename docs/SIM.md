# 模擬器（asmsim / `asmcore/sim.h`）

Main 與 Peri 兩個 sequencer 的行為模型，加上可設定的外部環境（CSR、I2C 裝置、影像時序）。核心只用標準 C++17。
它吃組譯器的輸出（`asmcore::Result`：映像檔加除錯資訊），所以組譯與模擬在同一個程序內完成，不必先存檔再讀回。

```
asmsim [--env FILE] [--scenario FILE | --cycles N --until EXPR] [--trace F] [--events F] [--vcd F] [--dump]
       [--call-model inline|softret] [--profile FILE] [-I DIR]  file.asm ...
asmsim examples/imu_gyro.asm --env examples/imu_env.cfg --scenario examples/imu_gyro.scn --vcd run.vcd
```

## 1. 時間模型

- 全域週期計數器（`Cycle`）。每個 domain 有「下一條指令何時可發出」；排程器永遠挑最早的動作執行。
- 同一時刻的順序：**計時事件先於兩個 domain**，其次 Main 再 Peri（`[sim] tie_order = peri main` 可對調）。
- 延遲取自 `asmcore::Latency`（組譯器算 WCET 用的同一份，規格 14.6 要求兩者一致）。預設是規格 14.3／14.4 的**規劃數值**：
  MOV = 1+len、消耗結果槽 = 2+len、EXEC_PUB = 2+len、單位元組 EXEC_CSR = 1+2、ALU = 1（MUL = 2）、BR = 1；
  `EXEC_XFER` = 發出成本 2 + I2C 位元組階段數 × 9 個 SCL 脈衝所需的 ISA 週期（27 MHz／400 kHz：67.5 週期／脈衝，
  與規格表的 1,823／2,430／5,468 週期一致）。
  真實的延遲表由 RTL 決定：以 profile 檔（`--profile`）或環境檔的 `[latency]` 區段覆寫。
- 指令效果在**發出時**生效；例外：`EXEC_XFER` 在匯流排時間結束時才與裝置對話並填 `PERI_RX_BUF`；
  `MOV` 讀結果槽時，`valid` 在 MOV **完成**時才清除（期間 `consume_busy`）。
- 一條 `BR.COND ALWAYS` 跳到自己 = 程式結束（該 domain `halted`）。兩個 domain 都停止時 `run()` 回報 Idle。

## 2. 環境檔

區段式文字檔，`;` 起頭為註解。`[種類 名稱]` 後接 `key = value`（key 可含空白，可重複）。

| 區段 | 內容 |
|---|---|
| `[sim]` | `tie_order = main peri`；`unmapped_csr = ram`（預設）或 `fault`；`fault_stops = domain`（預設）或 `all` |
| `[timing]` | `frame_cycles`、`vblank_start`、`vblank_len`、`line_cycles`、`hblank_start`、`hblank_len`（皆為 ISA 週期）。`vblank_open`、`remaining_vblank_cycles` 等由時間算出；`frame_end_evt` 在每個幀邊界被鎖存 |
| `[latency]` | 覆寫延遲，鍵名同 profile：`mov_byte = 2` … |
| `[bus i2c]` | `bus_hz`、`cpu_hz` |
| `[signal NAME]` | `init = 0`：宣告新的儲存型訊號（給自訂條件、CSR 動作使用） |
| `[csr NAME]` | 一個 CSR 的行為，見第 3 節。`addr` 省略時取程式中的 `.csr NAME` |
| `[device NAME]` | 一個 I2C 裝置，見第 4 節 |
| `[condition main\|peri N]` | 覆寫或新增分支條件 N（0..15）：`expr = ...`、`name = ...` |
| `[inject]` | `at 5000 = nack dev=0 count=1`：在指定週期套用一個注入命令（第 6 節） |

### 訊號（可用於條件、`run until`、`expect`、中斷點）

| 名稱 | 說明 |
|---|---|
| `poll.ready` `poll.not_ready` `poll.timeout` `poll.arm` `poll_mask` | Poll Unit（規格 11.3） |
| `timer.done` `timer_period` `timer.gen`；`main_timer.*` | Peri／Main 計時器 |
| `peri.done` `peri.error` | Peri 服務的完成與錯誤 latch（規格 11.5） |
| `slot.valid` `slot.busy` `slot.overrun` `slot.service_id` | 結果槽（規格 12） |
| `alu.cmp_true` `alu.zero` `alu.negative` `alu.carry_u` `alu.borrow_u` `alu.overflow_s` `alu.ov16` | `ALU_STATUS`（規格 6.8） |
| `vblank_open` `hblank_open` `remaining_vblank_cycles` `remaining_hblank_cycles` | 影像視窗（由時間算出） |
| `frame_end_evt` `pgl_pending` `ae_pending` `line_pending` `wcet_pgl` `wcet_ae` `wcet_line` | 時序條件的輸入 |

另有可索引的資料：`scratch[i]` `scratch[a..b]` `csrbuf[..]` `rx[..]` `slot[..]` `meta[..]` `const[..]` `csr[addr]`
（`a..b` 為這些位元組的小端值），以及 `opa` `opb` `out` `now` `main.pc` `peri.pc` `main.halted` `main.faulted` `main.instrs`（Peri 同）。

運算式語法：C 風格運算子 `! ~ - * / % + - << >> < <= > >= == != & ^ | && ||`，數字可寫 `0x1F`、`1Fh`、`0b101`。

### 預設的分支條件
Main 的 0x0..0xD 與 Peri 的 0..7 就是規格 7.3／11.2 的表，每一項都是一條運算式，例如
`PGL_COMMIT_ELIGIBLE = pgl_pending && vblank_open && remaining_vblank_cycles >= wcet_pgl`。
Main 的 0xE（`PROFILE_COND0`）預設**未定義**（使用會 fault，規格說未定義時為非法），可用 `[condition main 0xE]` 定義；0xF 保留。

## 3. CSR

| `kind` | 行為 |
|---|---|
| `ram`（預設） | 8 位元暫存器，`reset = n` |
| `value` | 讀寫對應一個訊號：`bind = timer_period`、`bytes = 2` 表示它涵蓋該訊號的兩個位元組（小端） |
| `action` | 寫 1 的位元觸發動作：`bit0 = poll_clear`、`bit1 = poll_arm`、`bit2 = timer_start:200`（冒號後是參數）。`SET8` 的遮罩視同寫入；`CLR8` 無作用；讀回 0 |
| `status` | 唯讀，`bitN = <運算式>`：該位元為運算式的真假 |
| `const` | 唯讀常數，`const = 0x44` |

沒有列出的位址視為 RAM（`unmapped_csr = fault` 則是 fault）。規格只命名 CSR（`POLL_MASK`、`POLL_CTRL`…）而沒給位址與位元定義，
所以位元對應完全由這個檔案決定。

內建動作：`poll_clear` `poll_arm` `poll_timeout_set` `done_set` `done_clear` `error_set` `error_clear` `service_reset`
（清 poll、error、done）`timer_start[:週期]` `timer_clear` `main_timer_start[:週期]` `main_timer_clear` `frame_end_clear` `overrun_clear`，
以及通用的 `set:訊號名`、`clr:訊號名`（不寫程式就能驅動任何訊號）。計時器用「世代」守衛：重啟或清除會讓先前排定的到期事件失效。

## 4. I2C 裝置

內建 `scripted` 裝置（`kind = scripted` 可省略）：

```
[device imu]
addr = 0x68
reg 0x3A = 0x00, 0x00, 0x01        ; 依序回傳這些值，之後停在最後一個（模擬 ready 位元）
regseq 0x3B = 1, 2, 3              ; 循環
block 0x43 = 0x0A 0x00 0x14 0x00   ; 連續暫存器的常數
default = 0x00                     ; 沒列出的暫存器
nack = true                        ; 不回 ACK（write_nack：只有寫入不回）
extra_cycles = 100                 ; 裝置端延遲
```
每次交易讀到某個暫存器，該暫存器的計數器加一（多位元組讀取時每個位址各加一）。位址上沒有裝置、或注入了 NACK，交易以 NACK 結束：
設 `peri.error`，`PERI_RX_BUF` 不變，指令照常結束。

## 5. 除錯功能（C++ 介面，`Simulator`）

| 功能 | 介面 |
|---|---|
| 執行 | `run(RunLimits)`（週期、指令數、`untilExpr`）、`step(domain)`、`stepOver(domain)`（以**來源行**為單位）、`runToPc` |
| 停止原因 | `CycleLimit InstrLimit Breakpoint Watchpoint Fault Idle Condition` |
| 中斷點 | `addBreakpoint(domain, pc, 條件運算式)`；在指令**執行前**停止，再繼續時會越過同一處 |
| 監看 | `addWatch("scratch[8..9]" \| "csr 0x120" \| "signal slot.valid" \| 任一運算式, 條件)`，值改變時停止 |
| call stack | `callStack(domain)`：inline 模型由展開資訊還原；softret 模型由影子堆疊（轉移點推入、抵達返回點彈出）。每個 frame 有程序名與呼叫點的檔案／行號 |
| 狀態 | `readRegion` `writeRegion` `signal` `setSignal` `csr` `eval(運算式)`、`dumpState()`、`diffDumps(a, b)` |
| 回溯 | `snapshot()`/`restore()`；`enableHistory(間隔, 保留數)` 後 `rewindTo(週期)`：還原最近的快照再往前重播（模型是確定性的，注入事件也屬於狀態） |
| 事件 | `addSink(EventSink)`：`Issue RegionWrite CsrAccess Signal Bus Publish Consume Overrun Fault Inject Note`。內建文字 trace、CSV、`VcdSink`（GTKWave） |

fault（規格 15.1）：記下出事的 pc、**pc 不前進**、該 domain 停止（另一個照常執行）。已實作：描述子保留位元非零、
descriptor 索引超出表、MOV 前置檢查失敗（不會部分寫入）、cascade 巢狀／步數不符／END 無 BEGIN、分支目標超出程式、
Peri 的 MultiByte CSR 與 `READ8`、MultiByte WRITE、DeviceMap 項目缺失或 bus_type 保留、pc 超出程式範圍。

## 5b. 給互動工具用的介面

`RunLimits::cancel` 與 `maxMillis`（失控保護）、`Simulator::setCancelHook`（讓另一個執行緒中斷 run／stepOver／stepOut）、`stepOut`、`regionSize`、`injectNow`
（立即套用注入命令）。IDE 的 `SimSession` 以這些建構（`AssemblyCoreIDE/docs/DEBUGGING.md`）。

## 6. 注入命令（`[inject]`、`Simulator::inject`、情境腳本的 `inject`）

| 命令 | 效果 |
|---|---|
| `nack dev=N count=K`（或 `addr=0x68`） | 該位址接下來 K 次交易回 NACK |
| `signal NAME=value ...` | 設定訊號（例如 `pgl_pending=1`、`slot.overrun=0`） |
| `slot_busy cycles=N` | 結果槽忙碌 N 個週期（`consume_busy`） |
| `fault_clear main\|peri` | 清除 fault 並讓該 domain 從原 pc 重新嘗試 |
| `csr 0x120=5` | 直接寫一個 CSR |
| `note 文字` | 在事件串流中留一筆備註 |

## 7. 情境腳本（`--scenario`、`runScenario`）

```
run 5000                          前進 5000 週期
run until EXPR [max N]            直到運算式為真
run idle [max N]                  直到兩個 domain 都停止
step main|peri [n]   stepover main|peri
break main:label [if EXPR]        中斷點（label 可加 main:/peri: 前綴，或寫數字）;  delete ID
watch scratch[8..9] [if EXPR]
inject at 5000 nack dev=0         inject in 100 signal pgl_pending=1
poke scratch[0] = 0x12 0x34       set slot.overrun = 0
expect EXPR [:: 說明]             檢查；expect_stop 原因 / expect_fault main|peri [代碼] / expect_pc main 標籤
dump | log 文字 | history 1000 50 | rewind 4000 | trace on
```
註解用 `;`。呼叫端可用 `runScenario(sim, script, &extra)` 加入自己的指令。

## 8. 規格沒寫、模擬器自行選擇的地方

| # | 選擇 | 規格狀況 |
|---|---|---|
| 1 | 效果時點如第 1 節 | 規格未規定 |
| 2 | 同一時刻計時事件 > Main > Peri | 未規定 |
| 3 | Peri 的 `EXEC_PUB` 遇到「Main 在同一週期才清除」的槽：**延後 1 週期**（不算 overrun） | 12.6 只說最早 T+1 |
| 4 | `MOV` 讀 `valid = 0` 的結果槽：fault `fault_slot_not_valid` | 只說前置檢查失敗不清 `valid` |
| 5 | `ALU_STATUS` 的位元組佈局：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 | 只列旗標 |
| 6 | 非 CMP 的 ALU 運算重新計算全部旗標；CMP 只改 `cmp_true`（它持續到下一個 CMP）；LOGIC 的 negative = bit 15 | 6.8 |
| 7 | 沒定義的 CSR 當 RAM | 未規定 |
| 8 | NACK 時指令照常結束、`PERI_RX_BUF` 不變、pc 前進 | 只說會設 error latch |
| 9 | poll 評估：「已 arm」且「寫入 `PERI_RX_BUF[0]` 的成功 READ」那一次；`ready`／`not_ready` 互斥；評估後解除 arm | 11.3, 11.4 |
| 10 | `INC`／`DEC` 作用在 `ALU_OPA`（沿用 ADD／SUB 的延伸規則） | 6.4 |
| 11 | I2C 交易時間不含 START／STOP 額外成本 | 14.4 自己也只算位元組階段 |
| 12 | `poll.timeout` 不會自動設定；由 CSR 動作 `poll_timeout_set` 或腳本設定 | 規格未說誰設 |
| 13 | CSR 不檢查是 Main 還是 Peri 在存取 | 規格說後端共用仲裁器 |

## 9. 尚未做

SPI、`MultiByte WRITE`、FIFO 結果槽（皆為規格的未來擴充）；CSR 仲裁競爭（兩個 domain 同時存取）；
指令層的資料流檢查；與 RTL 的逐週期比對；GUI。
