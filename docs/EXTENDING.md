# 規格變動時要改哪裡

規格還在確定中，這份文件列出每一種變動對應的**最小修改處**。原則：能用資料（profile、環境檔、模擬器參數）解決的，就不改程式；
要改程式的，限定在一個檔案裡。

（2026-11-20 對照程式碼核對；文中的函式與檔案名稱都存在。）

## 組譯器

| 想做的事 | 修改處 |
|---|---|
| ALU 子運算碼的數值（arith／cmp／logic） | profile：`alu_arith_add`、`alu_cmp_eq`、`alu_logic_and`…（不用改程式；預設是規格列出名稱的順序，屬未確認的規劃值） |
| 移位方向、區域編號、分支條件編號、CSR 運算碼 | `core/src/isa.h`（編碼表集中在這裡；假設項目標示 `ASSUMED-n`） |
| 指令位元組與描述子的位元欄位位置 | **只改 `core/src/codec.h`**。組譯器（打包）、模擬器、反組譯器、listing（拆解）全都經由它；`pack(unpack(x)) == x` 由測試保證 |
| 描述子的位元組順序、layout 欄位寬度 | profile：`big_endian`、`layout_offset_bits`（不用改程式） |
| DeviceMap 每筆的大小與位元排列 | profile：`device_map_entry_bytes`、`device_map_bus_shift`、`device_map_addr_shift` |
| 程式 SRAM 的配置、PC 寬度、容量 | profile：`sram_depth_words`、`main_image_base_word`／`peri_image_base_word`、`main_pc_width`／`peri_pc_width`、`main_capacity_bytes`／`peri_capacity_bytes` |
| 表格順序與 layout 欄位順序 | `core/src/layout.cpp` `buildImage()` 的 `addTable` 呼叫順序，以及 `core/src/image_view.cpp` 的名稱表（`mainN`／`periN`） |
| 描述子表深度、程式大小上限 | profile |
| 新增一條指令 | `lower.cpp` 的 `instr()` 加解析；`layout.cpp` 加編碼與長度；`isa.h` 加編碼；模擬器見下 |
| 延遲數字 | profile 的 `[latency]` 鍵（組譯器與模擬器共用同一份 `core/include/asmcore/latency.h`） |
| 呼叫模型（`CALL`/`RET`）的實作 | `lower.cpp` 的 `inlineCall`／`softCall`／`softRet`／`softDispatch`。若 ISA 將來加入真正的 CALL/RET，新增第三種模型並在 `CallModel` 加一個值，原始碼寫法不變 |
| 新的錯誤檢查 | `layout.cpp` 的 `validateFlow`／`validateCascade`／`validateReach`／`validatePlacement`；新代碼要同時加到 `core/src/diag_names.cpp`（說明、修法、v31 名稱）與 `docs/ASM_SYNTAX.md`——測試會檢查兩邊列出的代碼完全相同 |
| 診斷的說明或修法文字 | `core/src/diag_names.cpp`（`asmc --explain`、驗證報告、IDE 共用這一張表） |
| v31 產物的內容或新增一種產物 | `core/src/artifacts.cpp`（`Result::*Json()`） |
| 新的資料流規則 | `core/src/dataflow.cpp`：規則是 `MainFlow::transfer`／`PeriFlow` 裡的一小段，經 `Reporter::report` 輸出（嚴重度、去重、呼叫鏈由它處理）；分析引擎 `solve<S>` 與規則無關。新規則要加狀態欄位就加在 `MState`／`PState` 並補 `join` |
| 規格改了「什麼算已初始化」 | profile 的 `*_reset_defined` 旗標或 `.assume`，不必改程式 |

## 反組譯器

| 想做的事 | 修改處 |
|---|---|
| 某個 opcode 的編碼變了 | `codec.h`（欄位）；若指令長度或文字格式變了，再改 `disasm.cpp` 裡該 opcode 的解碼函式（`DecoderSet::defaults()`），或由呼叫端傳入自訂的 `DecoderSet` |
| 指令的組合語言寫法變了 | 組譯器的解析（`lower.cpp`）與反組譯器的輸出格式（`disasm.cpp`）要一起改；`tests/disasm_tests.cpp` 的往返測試會立刻抓出兩邊不一致 |
| 映像檔版面變了（表格順序、旁路 layout） | `image_view.cpp`（模擬器與反組譯器共用）；欄位寬度與位元組順序是 profile 參數，不必改程式。v31 起映像尾端沒有 ImageLayout，layout 由旁路資料（`*.layout.json`）提供 |
| 想把軟體 CALL/RET 還原成 `CALL`/`RET` | **尚未實作**。做法：新增「提升」階段，在 `disassemble()` 之後辨認呼叫／返回序列，產生 `.proc`/`CALL`；解碼本身不用動 |
| 解碼問題的分級（A 不合法編碼／B 違反規格規則／C 執行期） | 反組譯器（`disasm.cpp`）與模擬器的解碼器（`sim/decode.cpp`）必須判定一致；改任何一邊都要跑 `tests/crosscheck_tests.cpp`（要求 0 個不一致） |
| 在輸出加上資訊（原始行號、WCET…） | `DisasmOptions::commentHook`，不需要改核心 |

## 模擬器

| 想做的事 | 修改處 |
|---|---|
| 改變某個 opcode 的語意 | `Impl::registerOps()`（`sim/impl.cpp`）把 `mainOps[n]`／`periOps[n]` 換成新的處理函式；原本的在 `main_core.cpp`／`peri_core.cpp`，一個 opcode 一個函式 |
| 分支條件變了、新增條件 | 環境檔 `[condition main\|peri N]`；預設表在 `buildDefaultConditions()` |
| CSR 的行為、位元對應 | 環境檔 `[csr ...]`；要新的「kind」就加在 `csrRead8`／`csrWrite`（`impl.cpp`） |
| 新的 CSR 位元動作 | `Extensions::registerAction(name, fn)`（不需改核心），或 `builtin.cpp` 的 `Extensions::defaults()` |
| 新的 I2C 裝置行為 | `Extensions::registerDevice(kind, factory)`，實作 `I2cDevice`；狀態放在 `state` 向量裡（才會進快照） |
| 新的匯流排類型（SPI） | **尚未實作**（非 I2C 的 bus_type 目前回報 `xfer_fault_reserved_bus_type`）。`ImageView` 已讀出 DeviceMap 的 bus_type；在 `peri_core.cpp` 的 `opPeriXfer`／`xferComplete` 依 bus_type 分派，並新增對應的裝置介面 |
| 新的訊號 | 環境檔 `[signal NAME]`（儲存型），或 `buildSignals()` 的 `defComputed`（由其他狀態算出） |
| 新的追蹤或輸出格式 | 實作 `EventSink` 並 `addSink`（不需改核心） |
| 新的情境腳本指令 | `runScenario(sim, script, &extra)` 傳入 `ScenarioCommand` |
| 新的注入命令 | `Impl::applyCommand`（`sim/run.cpp`）與 `Simulator::inject`（`sim/debug.cpp`）的已知命令清單 |
| 新的硬體參數（延遲、仲裁規則、時脈…） | `core/src/sim_params.cpp` 的目錄（`add`／`choice`：鍵、群組、單位、預設、範圍、說明、FPGA 上怎麼量）；之後重新產生文件：`gen_sim_params_doc > docs/SIM_PARAMS.md`。**新參數的預設值不可改變既有結果**（使用者明確指定的例外才可以） |
| 取指來源與時間（EEPROM、SRAM、Frontend） | 參數 `fetch.source`、`fetch.read_mode`、`boot.load`、`frontend.*`；EEPROM 的六個區域在 `core/include/asmcore/eeprom.h`／`eeprom.cpp`；Frontend 七個狀態的模型在 `sim/frontend.cpp`（`bootLoadSram`、`planFrontend`、`beginFrontend`） |
| 解碼器如何由位元組形成指令（顯示用的各階段） | `sim/decode.cpp`（`DecodedLine`、`DecodeRead`） |
| 狀態要多存什麼 | 加進 `State`（`sim/impl.h`）；快照、回溯、傾印自動涵蓋（它是單一可複製的值） |
| 公開介面 | `core/include/asmcore/sim.h` 是唯一的邊界；內部全部在 `Simulator::Impl` 之後（pimpl），可以隨時重構 |

## 語言服務、積木、硬體描述

| 想做的事 | 修改處 |
|---|---|
| 新的助記符、指示字、子運算、具名參數出現在補全／上色／懸停 | `core/src/lang.cpp` 的 `Language::defaults()`（區域與分支條件直接取自 `isa.h`，其餘在這裡逐項 `add`）；或由呼叫端 `Language::add(Entry)` |
| 新的積木種類 | `core/src/blocks.cpp` 的 `Registry::defaults()`（選項取自語言詞彙）；元件在 `blocks_values.cpp`，參照在 `blocks_symbols.cpp` |
| 硬體描述（`.achw`）的欄位、產生器 | `core/src/hardware.cpp`（`asmcore/hardware.h`） |
| profile 鍵的說明、範圍、未確認群組 | `core/src/profile_info.cpp`（鍵的解析在 `profile.cpp`） |

## IDE

IDE 的分層與元件見 `AssemblyCoreIDE/docs/ARCHITECTURE.md`；原則是介面只呼叫服務層，服務層只呼叫核心的公開標頭。注意：該文件最後更新於 2026-10-20，之後新增的部分（積木編輯器、模擬器頁面、自動除錯、EEPROM 工具、架構圖佈局）尚未寫進去。
