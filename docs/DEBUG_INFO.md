# 除錯資訊（`--dbg` 產生的 `PREFIX.dbg.json`）

由 `Result::debugJson()` 產生，給模擬器與 IDE 使用。欄位：

| 欄位 | 內容 |
|---|---|
| `callModel`、`maxCallDepth` | 呼叫模型、靜態算出的最大呼叫深度 |
| `domains[]` | `name`、`codeSize`、`imageSize`、`sections[]`（`name`、`offset`、`size`、`entries`、`depth`） |
| `instrs[]` | 每一條指令：`domain`、`pc`、`len`、`file`、`line`、`text`、`cycles`（佔位延遲）、`synthetic`（組譯器產生的呼叫／返回序列）、`note`、`frames[]`（內嵌展開時的呼叫鏈，最內層在最後，每層有 `proc` 與呼叫點的 `file`/`line`） |
| `procs[]` | `name`、`emitted`、`startPc`/`endPc`、`callSites`、`slot`（softret：回傳編號所在的 `MAIN_SCRATCHPAD` 位移，-1 為不需要） |
| `callSites[]` | `callee`、`file`/`line`、`transferPc`（進入程序的分支）、`returnPc`、`retId`（softret：存入回傳槽的 16 位元編號，0 為不需要） |
| `symbols[]` | 標籤（`main:`／`peri:` 前綴）、`.equ`/`.csr`/`.service`/`.const`/`.devmap` |
| `vectors[]` | `.vector` 宣告：`domain`、`id`、`label`、`pc`、`file`/`line` |
| `wcet[]` | 每個查詢的 `from`、`to`、`bounded`、`cycles`、`note` |

## 用影子堆疊還原 call stack

硬體沒有堆疊，除錯器用這份資訊在模擬時自己維護一份：

- **inline**：不需要影子堆疊。以目前 pc 的 `instrs[].frames` 直接得到呼叫鏈。
- **softret**：執行到 `callSites[i].transferPc` 時，推入 `{callee, returnPc, 呼叫點的 file/line}`；pc 抵達該程序某個 `returnPc` 時彈出。
  回傳槽（`procs[].slot`）的值與 `retId` 可用來與影子堆疊交叉檢查。

Stack dump 就是這份堆疊加上各區域與 latch 的快照。
