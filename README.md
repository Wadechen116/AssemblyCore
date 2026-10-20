# AssemblyCore

Sensor Micro-Sequencer ISA v1.6 r10（Main / Peri）的組譯器，加上日後的 IDE。規格書在 `docs/`。

```
core/            組譯器與模擬器核心函式庫（標準 C++17：不含平台 API、不讀寫檔案、不依賴第三方函式庫）
  include/asmcore/  asmcore.h（組譯器）、sim.h（模擬器）、disasm.h（反組譯器）、latency.h（共用的延遲模型）
  src/              組譯器；codec.h 是指令與描述子位元欄位的唯一來源；image_view.* 解析映像檔版面（模擬器與反組譯器共用）；src/sim/ 是模擬器
cli/             asmc（組譯器）、asmsim（組譯並模擬）、asmdis（反組譯）；唯一碰檔案系統的程式
tests/           main.cpp：組譯器（手算的金標準位元組、錯誤情境）；sim_tests.cpp：模擬器（規格的 ALU 範例、IMU 端到端、fault、時序、除錯）；disasm_tests.cpp：反組譯器（組譯→反組譯→組譯得到同一份映像檔、損壞映像）
examples/        範例原始碼（IMU 陀螺儀流程、CALL/RET 兩種模型）
profiles/        目標 profile 範本（資料表深度、延遲、softret 保留位置）
docs/            規格書、ASM_SYNTAX.md（語法與假設）、SIM.md（模擬器與環境檔）、DISASM.md（反組譯器）、EXTENDING.md（規格變動時改哪裡）、DEBUG_INFO.md
AssemblyCoreIDE/ IDE（尚未開始）
```

## 建置與測試

```
cmake -S . -B build
cmake --build build
ctest --test-dir build            # 或直接執行 build/asmcore_tests
build/asmc --list --map --dbg examples/imu_gyro.asm
build/asmsim examples/imu_gyro.asm --env examples/imu_env.cfg --scenario examples/imu_gyro.scn --vcd run.vcd
build/asmdis --main --check imu.main.bin          # 反組譯並確認重新組譯得到同一份映像檔
```
需要 C++17 編譯器與 CMake 3.16。已在 GCC 13（Linux）與 MSVC v141（VS2017）驗證，兩者產出的映像檔逐位元組相同。

## 設計原則

- **核心只用標準 C++**，以文字進、結果出（`asmcore::assemble`）；`#include` 透過回呼解析，所以 IDE 可以直接傳記憶體中的未存檔內容。
- **組譯器的輸出就是除錯資訊的來源**：`Result` 帶有每行指令的 pc／原始行／展開來源，程序、呼叫點、符號與 WCET；模擬器與 IDE 之後直接吃這份資料。
- 規格書沒有寫的數值（ALU 子運算碼、位元組順序等）全部集中標示為「假設」，見 `docs/ASM_SYNTAX.md` 第 9 節。

## 目前狀態

**組譯器**：兩個 domain 的全部指令、描述子去重與表格、映像檔佈局、驗證、listing／map／debug JSON／Intel HEX、WCET 查詢、
`CALL`／`RET`（`inline` 與 `softret`）。
**模擬器**：Main／Peri 雙核（規格第 4–12 章的指令語意與 fault）、共用結果槽（overrun、同週期規則）、可設定的 CSR 行為、
I2C 裝置與 NACK 注入、影像視窗與 commit 資格（`*_COMMIT_ELIGIBLE`）、計時器與 Poll Unit、中斷點／監看／單步／
stepOver／call stack／狀態傾印與差異／快照與回溯、文字／CSV／VCD trace、情境腳本。
**反組譯器**：還原成可重新組譯的原始碼，往返驗證逐位元組相同；損壞或手工的映像檔以 D1xx／D2xx／D3xx 回報而不是靜默亂解。
測試：組譯器 180 項、模擬器 106 項、反組譯器 38 項；ASan／UBSan 乾淨；MSVC v141 與 GCC 13 皆可編譯並通過。
**資料流檢查**（W5xx）：ALU 運算元、緩衝區先寫後讀、ALU 結果不遺失、結果槽的正確消耗、Peri 發佈與 Main 消耗的長度與分派一致、軟體 CALL/RET 造成的破壞。
**語言服務層**（`lang.h`、`index.h`，詳見 `docs/LANG_SERVICE.md`）：診斷欄位、詞彙／token、符號索引、行↔位址對應。
**進行中**：IDE（`AssemblyCoreIDE/`，計畫見其 `docs/`）；模擬器中 SPI、FIFO 結果槽與 CSR 仲裁競爭（見 `docs/SIM.md` 第 9 節）。

規格沒寫、由我選擇的項目全部列在 `docs/ASM_SYNTAX.md` 第 9 節與 `docs/SIM.md` 第 8 節，請與 RTL 負責人確認。
