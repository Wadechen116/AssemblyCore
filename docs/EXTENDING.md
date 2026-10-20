# 規格變動時要改哪裡

規格還在確定中，這份文件列出每一種變動對應的**最小修改處**。原則：能用資料（環境檔）解決的，就不改程式；
要改程式的，限定在一個檔案裡。

## 組譯器

| 想做的事 | 修改處 |
|---|---|
| ALU 子運算碼的數值、區域編號、分支條件編號、CSR 運算碼 | `core/src/isa.h`（所有編碼表集中在這裡；假設項目標示 `ASSUMED-n`） |
| 指令位元組與描述子的位元欄位位置 | **只改 `core/src/codec.h`**。組譯器（打包）、模擬器、反組譯器、listing（拆解）全都經由它；`pack(unpack(x)) == x` 由測試保證 |
| 映像檔的位元組順序、ImageLayout 欄位寬度 | profile：`big_endian`、`layout_offset_bytes`（不用改程式） |
| 表格順序與 ImageLayout 欄位順序 | `core/src/layout.cpp` `buildImage()` 的 `addTable` 呼叫順序，以及 `sim/impl.cpp` 的 `ImageView::load` 名稱表 |
| 描述子表深度、程式大小上限 | profile |
| 新增一條指令 | `lower.cpp` 的 `instr()` 加解析；`layout.cpp` 加編碼與長度；`isa.h` 加編碼；模擬器見下 |
| 延遲數字 | profile 的 `[latency]` 鍵（組譯器與模擬器共用同一份 `core/include/asmcore/latency.h`） |
| 呼叫模型（`CALL`/`RET`）的實作 | `lower.cpp` 的 `inlineCall`／`softCall`／`softRet`／`softDispatch`。若 ISA 將來加入真正的 CALL/RET，新增第三種模型並在 `CallModel` 加一個值，原始碼寫法不變 |
| 新的錯誤檢查 | `layout.cpp` 的 `Layout::validate*`，錯誤碼加到 `docs/ASM_SYNTAX.md` |
| 新的資料流規則 | `core/src/dataflow.cpp`：規則是 `MainFlow::transfer`／`PeriFlow` 裡的一小段，經 `Reporter::report` 輸出（嚴重度、去重、呼叫鏈由它處理）；分析引擎 `solve<S>` 與規則無關。新規則要加狀態欄位就加在 `MState`／`PState` 並補 `join` |
| 規格改了「什麼算已初始化」 | profile 的 `*_reset_defined` 旗標或 `.assume`，不必改程式 |

## 反組譯器

| 想做的事 | 修改處 |
|---|---|
| 某個 opcode 的編碼變了 | `codec.h`（欄位）；若指令長度或文字格式變了，再改 `disasm.cpp` 裡該 opcode 的解碼函式（`DecoderSet::defaults()`），或由呼叫端傳入自訂的 `DecoderSet` |
| 指令的組合語言寫法變了 | 組譯器的解析（`lower.cpp`）與反組譯器的輸出格式（`disasm.cpp`）要一起改；`tests/disasm_tests.cpp` 的往返測試會立刻抓出兩邊不一致 |
| 映像檔版面變了（表格順序、尾端欄位） | `image_view.cpp`（模擬器與反組譯器共用）；欄位寬度與位元組順序是 profile 參數，不必改程式 |
| 想把軟體 CALL/RET 還原成 `CALL`/`RET` | 新增「提升」階段：在 `disassemble()` 之後依 `Result.callSites` 辨認序列，產生 `.proc`/`CALL`；解碼本身不用動 |
| 在輸出加上資訊（原始行號、WCET…） | `DisasmOptions::commentHook`，不需要改核心 |

## 模擬器

| 想做的事 | 修改處 |
|---|---|
| 改變某個 opcode 的語意 | `Impl::registerOps()`（`sim/impl.cpp`）把 `mainOps[n]`／`periOps[n]` 換成新的處理函式；原本的在 `main_core.cpp`／`peri_core.cpp`，一個 opcode 一個函式 |
| 分支條件變了、新增條件 | 環境檔 `[condition main\|peri N]`；預設表在 `buildDefaultConditions()` |
| CSR 的行為、位元對應 | 環境檔 `[csr ...]`；要新的「kind」就加在 `csrRead8`／`csrWrite`（`impl.cpp`） |
| 新的 CSR 位元動作 | `Extensions::registerAction(name, fn)`（不需改核心），或 `builtin.cpp` 的 `Extensions::defaults()` |
| 新的 I2C 裝置行為 | `Extensions::registerDevice(kind, factory)`，實作 `I2cDevice`；狀態放在 `state` 向量裡（才會進快照） |
| 新的匯流排類型（SPI） | `ImageView` 已讀出 DeviceMap 的 bus_type；在 `peri_core.cpp` 的 `opPeriXfer`／`xferComplete` 依 bus_type 分派，並新增對應的裝置介面 |
| 新的訊號 | 環境檔 `[signal NAME]`（儲存型），或 `buildSignals()` 的 `defComputed`（由其他狀態算出） |
| 新的追蹤或輸出格式 | 實作 `EventSink` 並 `addSink`（不需改核心） |
| 新的情境腳本指令 | `runScenario(sim, script, &extra)` 傳入 `ScenarioCommand` |
| 新的注入命令 | `Impl::applyCommand`（`run.cpp`）與 `Simulator::inject` 的已知命令清單 |
| 狀態要多存什麼 | 加進 `State`（`sim/impl.h`）；快照、回溯、傾印自動涵蓋（它是單一可複製的值） |
| 公開介面 | `core/include/asmcore/sim.h` 是唯一的邊界；內部全部在 `Simulator::Impl` 之後（pimpl），可以隨時重構 |
