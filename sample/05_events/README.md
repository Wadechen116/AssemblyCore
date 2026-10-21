# 05_events

規格 v31 §16 的硬體事件：Main 與 Peri 各有自己的事件控制器。事件隨時可以 **pending**，但**只有成功退休、帶 `safe` 的一般分支**才會派送到 handler
（`.vector ID, label` 宣告入口），handler 以 `BR.ERET` 返回被打斷流程「已解析的下一個 PC」。硬體不保存 ALU／scratchpad／RX 等狀態。

- Main：`fg_loop` 的 `BR.COND PERI_RESULT_VALID, …, safe` 是檢查點；`on_frame`（事件 0）把計數器加 1；`on_error`（事件 1）寫 CSR。
- Peri：`BR.IF_NO_ERROR loop, safe` 是檢查點；`peri_evt` 設 CSR 位元。

## 試試看
1. 建置：零警告（`W512`、`W513` 都沒有：handler 只動前景不讀的位元組，且每個前景迴圈都有 `safe`）。
2. **讓警告出現**：
   - 刪掉 `fg_loop` 那行的 `, safe` → **W513**（沒有 safe 分支，事件回應時間無上界）。
   - 在 `fg_loop` 前讓前景用 `ALU_OPA`（例如 `MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA`，且迴圈後還要用），handler 又改 `ALU_OPA` → **W512**。
   - 把 `BR.ERET` 換成 `JMP on_frame` → **E315**（handler 走不到 ERET）。
   - 在 `start:` 前面加 `BR.ERET` → **E315**（正常流程不能執行 ERET）。
3. 打開 `out/05_events.events.json`：列出向量，以及事件時間分項（pending→checkpoint、派送、handler、ERET）與總和；`.map` 有同樣的 `events` 段。
4. **除錯中看事件**：事件由 `events_env.cfg` 的 `[inject]` 段在第 200、300、500、800 週期注入（IDE 目前沒有「即時注入」對話框，所以用環境檔）。在 `on_frame` 設中斷點，F5 開始 → 約第 200 週期後停在 handler 第一行；**Signals** 面板看到 `main.in_handler = 1`；**Call Stack** 看不到呼叫（handler 不是 CALL）；F5 繼續 → ERET 後回到前景。改環境檔的時間可以讓事件落在不同的前景位置。
5. **Events 面板**：篩選 `event`：看到 `raise`、`dispatch`、`eret` 三種紀錄，點一筆跳到對應原始碼。
6. 觀察「不巢狀」：把環境檔的 `at 800 = event main id=1` 改成與事件 0 的 handler 同一時間（例如 `at 205`），事件 1 會維持 pending（**Signals** 面板的 `main.events_pending` 位元 1），ERET 之後下一個前景 `safe` 分支才派送。

## 預期
`asmsim -I ../common events.asm --env events_env.cfg --scenario events.scn` → `PASS (6 checks)`。
