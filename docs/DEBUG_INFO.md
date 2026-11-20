# 除錯資訊（`--dbg` 產生的 `PREFIX.dbg.json`）

由 `Result::debugJson()`（`core/src/report.cpp`）產生，給模擬器與 IDE 使用。欄位（2026-11-20 對照程式碼核對）：

| 欄位 | 內容 |
|---|---|
| `tool`、`isa` | 產生者（`asmcore`）與 ISA 名稱 |
| `callModel`、`maxCallDepth` | 呼叫模型、靜態算出的最大呼叫深度 |
| `domains[]` | `name`、`codeSize`、`imageSize`、`baseWord`／`reservedWords`（在共用程式 SRAM 中的起始 word 與保留的 word 數）、`layout[]`（v31 旁路 layout 的各區 offset）、`sections[]`（`name`、`offset`、`size`、`entries`、`depth`） |
| `instrs[]` | 每一條指令：`domain`、`pc`、`len`、`file`、`line`、`text`、`cycles`（佔位延遲）、`synthetic`（組譯器產生的呼叫／返回序列）、`note`（有才出現）、`table`／`desc`（用到的描述子表名稱與索引，有才出現）、`branch`／`target`（是分支時出現，`target` 為目標 pc）、`fall`（不會落到下一條時為 `false`，否則省略）、`frames[]`（內嵌展開時的呼叫鏈，最內層在最後，每層有 `proc` 與呼叫點的 `file`/`line`） |
| `procs[]` | `domain`、`name`、`file`/`line`、`emitted`、`startPc`/`endPc`、`callSites`（呼叫點數量）、`slot`（softret：回傳編號所在的 `MAIN_SCRATCHPAD` 位移，-1 為不需要）、`wcetBounded`／`wcetCycles`（從呼叫到返回的最壞週期，取所有呼叫點的最大值；佔位延遲） |
| `callSites[]` | `domain`、`callee`、`caller`、`file`/`line`、`transferPc`（進入程序的分支）、`returnPc`、`retId`（softret：存入回傳槽的 16 位元編號，0 為不需要）、`wcetBounded`／`wcetCycles` |
| `symbols[]` | `name`、`kind`、`value`、`file`/`line`：標籤（`main:`／`peri:` 前綴）、`.equ`/`.csr`/`.service`/`.const`/`.devmap` |
| `vectors[]` | `.vector` 宣告：`domain`、`id`、`label`、`pc`、`file`/`line` |
| `wcet[]` | 每個查詢的 `name`、`from`、`to`、`bounded`、`cycles`、`note` |

## 用影子堆疊還原 call stack

硬體沒有堆疊，除錯器用這份資訊在模擬時自己維護一份：

- **inline**：不需要影子堆疊。以目前 pc 的 `instrs[].frames` 直接得到呼叫鏈。
- **softret**：執行到 `callSites[i].transferPc` 時，推入 `{callee, returnPc, 呼叫點的 file/line}`；pc 抵達該程序某個 `returnPc` 時彈出。
  回傳槽（`procs[].slot`）的值與 `retId` 可用來與影子堆疊交叉檢查。

Stack dump 就是這份堆疊加上各區域與 latch 的快照。

## 相關的其他輸出

`dbg.json` 只描述組譯結果。下列資訊在別的檔案裡：

| 檔案 | 內容 | 說明 |
|---|---|---|
| `PREFIX.eeprom.sym` | EEPROM 映像中各位址的名稱 | `docs/EEPROM.md`；`asmdis --eeprom` 用它把名稱放回反組譯結果 |
| `*.layout.json`、`section_map.json`、`descriptor_map.json`、`boot_load_plan.json`、`memory_proof.json`、`services.json`、`event_vectors.json`、`validation_report.json`、`build_manifest.json` | v31 的各項產物（`asmc --artifacts`） | `core/src/artifacts.cpp` |

IDE 不讀 `dbg.json` 檔案，而是直接使用記憶體中的 `Result`（同一份資料）；行與位址的對應用 `index.h` 的 `SourceMap`（見 `docs/LANG_SERVICE.md`）。
