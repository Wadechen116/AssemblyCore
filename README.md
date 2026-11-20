# AssemblyCore

Sensor Micro-Sequencer ISA v1.6 r10（規格 v31，Main／Peri 雙 domain）的組譯器、模擬器、反組譯器，以及 Qt Widgets IDE（含積木編輯器、硬體設計器、模擬器頁面、EEPROM 工具）。規格書在 `docs/`。

進度與待辦以 `docs/PROJECT_STATUS.md` 為準；本文只說明專案長什麼樣、怎麼建置。

```
core/            核心函式庫（標準 C++17：不含平台 API、不讀寫檔案、不依賴第三方函式庫）
  include/asmcore/  asmcore.h（組譯器）、sim.h（模擬器）、disasm.h（反組譯器）、lang.h／index.h（語言服務）、
                    hardware.h（硬體描述 .achw）、eeprom.h（EEPROM 映像）、sim_params.h（模擬器硬體參數）、latency.h（共用延遲模型）
  src/              codec.h 是指令與描述子位元欄位的唯一來源；image_view.* 解析映像版面（模擬器與反組譯器共用）；
                    src/sim/ 是模擬器（含 decode.cpp 解碼器、frontend.cpp Frontend 七狀態模型）；blocks*.cpp 是積木模型
cli/             asmc（組譯）、asmsim（組譯並模擬）、asmdis（反組譯）、asmhw（硬體描述）、asmblk（積木檔轉換）、
                 gen_sim_params_doc（產生 docs/SIM_PARAMS.md）；只有這裡碰檔案系統
tests/           核心測試：組譯器、模擬器、反組譯器、解碼交叉比對、硬體描述、積木
examples/        早期的範例原始碼（IMU 陀螺儀流程、CALL/RET 兩種模型）
sample/          範例專案 01–17（各附說明；build_samples.py 建置、check_samples.py 檢查），索引見 sample/README.md
profiles/        目標 profile 範本（資料表深度、延遲、版面、softret 保留位置）
docs/            規格書與各項說明（索引見 docs/PROJECT_STATUS.md 第 6 節）
AssemblyCoreIDE/ IDE：services/（標準 C++17，不含 Qt）、ui/（Qt Widgets）、tests/、tools/md2pdf、docs/
dist/            Windows 發佈資料夾（dist\msvc2026，不進版本控制）
```

## 建置與測試

```
cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt6 路徑>     # 找不到 Qt6 時只建核心、CLI、服務層與其測試
cmake --build build
ctest --test-dir build                                  # 15 項；GUI 測試需 QT_QPA_PLATFORM=offscreen
build/asmc --list --map --dbg examples/imu_gyro.asm
build/asmsim examples/imu_gyro.asm --env examples/imu_env.cfg --scenario examples/imu_gyro.scn --vcd run.vcd
build/asmdis --main --check imu.main.bin                # 反組譯並確認重新組譯得到同一份映像
```

需要 C++17 編譯器與 CMake 3.16；IDE 另需 Qt 6.5。IDE 也可以用 QMake 建置（`AssemblyCoreIDE/AssemblyCoreIDE.pro`）。

| 平台 | 工具鏈 | 狀態 |
|---|---|---|
| Linux（WSL Ubuntu） | GCC 13、Qt 6.5.3、CMake | `ctest` 15/15 通過（2026-11-20） |
| Windows | Visual Studio 2026（MSVC 14.51）、Qt 6.5.3 `msvc2019_64`、CMake＋Ninja | 0 個 `warning C`；核心與服務層測試通過（2026-11-19）；UI 測試自 2026-11-18 起未重跑 |
| macOS | — | 尚未驗證 |

Windows 的發佈資料夾需要目標機器已安裝 VC++ 可轉散發套件（資料夾內沒有附）。

## 設計原則

- **核心只用標準 C++**，以文字進、結果出（`asmcore::assemble`）；`#include` 透過回呼解析，所以 IDE 可以直接傳記憶體中的未存檔內容。
- **組譯器的輸出就是除錯資訊的來源**：`Result` 帶有每行指令的 pc／原始行／展開來源，程序、呼叫點、符號與 WCET；模擬器與 IDE 直接使用。
- **位元欄位只有一份**：`core/src/codec.h`。組譯器打包，模擬器、反組譯器、清單解開；規格變動時改哪裡見 `docs/EXTENDING.md`。
- **IDE 分層**：`services/` 不含 Qt，`ui/` 才用 Qt；介面不直接碰模擬器，一律經過 `SimSession`。
- **規格沒寫的數值一律標示為假設或規劃值**，由 profile 與模擬器參數提供，工具會明說哪些尚未確認（`asmc --profile-gaps`）。

## 目前狀態（摘要，2026-11-20）

- **組譯器**：兩個 domain 的全部指令（v31 寫法）、描述子去重與表格、profile 決定的版面與 SRAM 打包、各項產物（`--artifacts`）、
  `CALL`／`RET`（`inline` 與 `softret`）、硬體事件（`.vector`、`BR.ERET`、`safe`）、資料流檢查（W5xx）、EEPROM 映像（`--eeprom`）。
- **模擬器**：Main／Peri 雙核、共用結果槽、CSR／I2C／計時器／Poll、硬體事件、硬體參數資料庫（運行中可改）、共用資源競爭（S7）、
  從 EEPROM 取指與共用 I2C 匯流排、Frontend 七狀態模型（選用）、解碼問題分級（A／B／C）、快照與倒回、文字／CSV／VCD 軌跡、情境腳本。
- **反組譯器**：還原成可重新組譯的原始碼，往返驗證逐位元組相同；可直接反組譯 EEPROM 映像；損壞的映像以 D 代碼回報。
- **IDE**：編輯、建置、除錯、模擬器頁面（架構圖與時間軸）、積木編輯器、硬體設計器、自動除錯與修復、EEPROM 視窗、English／繁體中文。
- **尚未完成**：模擬結果尚未對照 RTL／FPGA（多項數值是規劃值）；SPI 與 FIFO 結果槽；S8 校準；IDE 的分析視圖（P2）、格式化、
  歡迎頁與精靈。完整清單見 `docs/PROJECT_STATUS.md` 第 3 節。

規格沒寫、由本專案選擇的項目列在 `docs/ASM_SYNTAX.md` 第 9 節與 `docs/SIM.md` 第 8 節，請與 RTL 負責人確認。
