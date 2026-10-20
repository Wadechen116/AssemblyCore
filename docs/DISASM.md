# 反組譯器（`asmdis` / `asmcore/disasm.h`）

把映像檔還原成組合語言。核心承諾：

> 對**組譯器產生的任何映像檔**，`disassemble(...).source()` 的文字再組譯，得到**逐位元組相同**的映像檔。

所以「組譯 → 反組譯 → 組譯 → 比對」本身就是整套工具鏈的回歸測試（`tests/disasm_tests.cpp` 在每一種指令形式、兩種呼叫模型、
不同位元組順序與 ImageLayout 欄位寬度下都做這件事；`asmdis --check` 讓你對任何一個 `.bin` 做同樣的驗證）。

```
asmc examples/imu_gyro.asm -o imu                  # 產生 imu.main.bin / imu.peri.bin
asmdis --main --check imu.main.bin                 # 印出組合語言，並檢查「重新組譯 == 原映像檔」
asmdis --peri --annotate --labels names.txt -o peri.asm imu.peri.bin
```

| 選項 | 說明 |
|---|---|
| `--main` / `--peri` | 映像檔屬於哪個 domain（必要；映像檔本身不帶這個資訊） |
| `--layout FILE` | 映像的 ImageLayout（`asmc` 寫的 `PREFIX.main.layout.json`）。v31 起 layout 是旁路資料、不在映像內；預設讀 `image.layout.json`，找不到會明確說明 |
| `--sram FILE` `--length N` | 改由 `PREFIX.sram.hex` 的 32 位元 word 還原邏輯映像（獨立驗證 word 打包），N 預設為最後一個區段結尾 |
| `--events FILE` | `asmc --artifacts` 寫的 `PREFIX.events.json`：該 domain 的事件向量會變成標籤與 `.vector` 行，文字用同樣的檢查重新組譯。向量不在映像內，沒有它時 handler 程式碼仍會被解碼，只是成為不可達程式碼（W402），映像位元組相同 |
| `--profile FILE` | 組譯時用的 profile（位元組順序、DeviceMap 排列、ALU 碼、SRAM 基底）。**要和組譯時一致**，否則會被偵測為版面錯誤 |
| `--code-size N` | 程式碼的確切位元組數；不給就推斷（見下） |
| `--labels FILE` | 標籤名稱，每行 `位址 名稱`（如 `0x0008 handle_gyro`）；沒給的目標以 `L_xxxx` 命名（`--label-prefix` 可改） |
| `--long-jumps` | 無條件分支寫成 `BR.COND ALWAYS` / `BR.ALWAYS`，不用 `JMP` |
| `--annotate` | 每行加上位址與原始位元組的註解 |
| `--check` | 把輸出再組譯一次並與輸入映像檔比對 |

## 它怎麼做

1. **版面**：依旁路 ImageLayout（layout.json）找出各描述子表、常數池／DeviceMap（`image_view.*`，與模擬器共用）。版面不合理（表格跑出映像檔外、
   順序錯亂，常見於位元組順序或欄位寬度設錯）就直接拒絕，不會硬解出一堆垃圾。
2. **程式碼範圍**：映像檔裡沒有 code size（規格 3.1）。有組譯器的結果就用精確值；只有裸 `.bin` 時用推斷：
   從位移 0 依序解碼，**在無條件分支之後，若下一個位址不是 BranchDesc 表中任何一項的目標，就視為程式結束**
   （後面是對齊用的 0 或其他資料）。限制：結尾之後「沒有任何分支指向、又恰好接在無條件分支後」的不可達程式碼會被當成填補而漏掉；
   想要精確請給 `--code-size`。
3. **解碼**：每個 domain、每個 opcode 一個解碼函式（`DecoderSet`），描述子欄位一律經 `codec.h` 拆解。
4. **標籤**：所有分支目標；目標必須落在指令起點上，否則報 D203。
5. **定義**：Main 的常數池輸出成一個 `.const __pool db ...`（MOV 以 `MAIN_CONST_POOL[a..b]` 的數字範圍引用，所以位元組與原來相同）；
   Peri 的 DeviceMap 輸出成 `.devmap`。CSR 位址、service_id 以數字輸出（原本的名字不在映像檔裡）。

## 回報（`DisasmIssue`，`asmdis` 印到標準錯誤）

| 碼 | 嚴重 | 意義 |
|---|---|---|
| D101 | 致命 | 該 domain 沒有映像檔 |
| D102 | 致命 | ImageLayout 不合理（映像太小、表格出界、與區段清單不符） |
| D201 | 致命 | 不合法的指令編碼（保留位元非零、保留的運算碼、索引超出表格…）；該行輸出成註解，後面的仍照常解碼 |
| D202 | 備註 | 指令合法，但有被忽略的位元（例如 `addr_len = 0` 時非零的 `xfer_addr`）；文字不含它 |
| D203 | 致命 | 分支目標不在指令起點上 |
| D301 | 致命 | DeviceMap 項目的 bus_type 是保留值 |
| D302 | 備註 | 表格裡有沒有任何指令使用的項目（例如用 `--pad-tables` 補滿的）；不會被重現 |

`ok` 表示沒有致命項目。有 D201 時，輸出裡那一行是註解，所以**不能**保證重新組譯回同一份映像檔。

## 不會做的事

- 不會把軟體 `CALL`/`RET` 的展開（存回傳編號、比較鏈）還原成 `CALL`/`RET`、也不會還原 `.proc`：映像檔裡沒有這個概念，
  看到的是一般的 MOV／ALU／BR。要做的話是「提升」階段（見 `EXTENDING.md`），建議用組譯器的除錯資訊來辨認。
- 不會還原符號名稱（CSR 名、service 名、`.equ`），除非用 `--labels` 提供標籤。
- 手工製作或損壞的映像檔：能解碼的部分照常輸出，其餘以 D2xx 回報。

## C++ 介面

```cpp
DisasmResult r = disassemble(result.main, profile);          // 一個 domain
std::string all = disassembleResult(result, profile);        // 兩個 domain，沿用原標籤名稱
```
`DisasmOptions` 有：`codeSize`、`names`、`labelPrefix`、`useJmp`、`commentHook`（對每一行加自訂註解，例如把 pc 對回原始行號）、
`decoders`（自訂解碼表）。`DecoderSet` 是延伸點：規格改了某個 opcode 的編碼，換掉那個 opcode 的解碼函式即可，
函式看到的 `DecodeContext` 只有 `byteAt` / `entry` / `deviceMap` 三個回呼，不直接碰映像檔緩衝區。
