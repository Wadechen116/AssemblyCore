# 積木程式（Block programs）— 階段 B0

把一支程式表示成「積木樹」而不是文字行。這是積木式編輯器的**基礎**（模型、定義、檔案格式、文字⇄積木轉換）；圖形畫布在 B1。設計與路線見 `AssemblyCoreIDE/docs/BLOCKS_DESIGN.md`。
標準 C++17、無 Qt（`core/include/asmcore/blocks.h`）；命令列工具 `asmblk`。

## 模型

```
Document (.acblk)            一個來源檔
 └ Section  domain = "" | "main" | "peri"      "" = 第一個 .main/.peri 之前的行（註解、.include、定義）
    └ Block  id, type, fields{名稱→文字}, stacks{名稱→Block 列表}, comment, disabled, collapsed
```

- **`id`**：檔內唯一、存檔後不變（`b1`、`b2` …），讓診斷、中斷點、編輯狀態能指到積木。
- **`fields`**：所有運算元都以**文字**保存（`"src": "CONST(5, 2)"`），所以任何合法的運算元都存得下，產生的文字與原本等價。編輯器用 `parseOperand`／`formatOperand` 把 `MAIN_SCRATCHPAD[0..1]` 拆成「區域＋位元組範圍」顯示成下拉與數字，拆不開的（`CONST(5, 2)`、`@名稱`）就當文字欄位。
- **`stacks`**：容器積木的語句槽。目前只有 `proc`（槽 `BODY`）；之後的 if／while 用同一個機制。
- **`comment`**：行尾註解；**`disabled`**：產生時變成註解行。

## Level 1 積木（一指令一積木，文字完全對應）

`asmblk blocks` 列出全部。抽屜（category）：

| 抽屜 | 積木 |
|---|---|
| Data | `mov` |
| Math | `alu`（運算、位移量、signed/unsigned） |
| CSR | `csr_write8` `csr_set8` `csr_clr8` `csr_read8` `csr_write_sp` `csr_read_buf` |
| Control | `br_cond` `jmp`（皆有 *event checkpoint* 旗標＝`, safe`）`br_peri` `br_eret` `cascade_begin` `cascade_end` `call` `ret` |
| Peripheral | `xfer` `pub` |
| Structure | `proc` `label` `vector` `wcet` `assume` |
| Definitions | `directive`（`.equ` `.const` `.csr` `.csrbit` `.service` `.devmap` `.include` `.end` …；多半由硬體設計器擁有） |
| Text | `comment` `blank` `raw` |

- 下拉選項（ALU 運算、Main 條件、Peri 條件…）**由語言詞彙表產生**；`open` 欄位接受其他值（原樣保存，由組譯器判斷）。
- **`raw`**：任何看不懂的行（未知指令、運算元怪異、`NAME EQU 5` 這種 8051 寫法）原封不動保存。**匯入永遠成功**；`asmblk import` 會列出有多少行留成 Raw，那些是積木尚未涵蓋的。
- `domains`：Main 專用（`mov` `alu` …）、Peri 專用（`xfer` `pub` `br_peri`）、兩者皆可；放錯 domain 由 `validate` 指出。

## 轉換與保證

| | |
|---|---|
| `importAsm(text)` | 文字 → 積木。標籤獨立成 `label` 積木；行尾註解掛在該行的積木；空行與整行註解保留；`.main`／`.peri` 變成 Section；`.proc … .endp` 變成 `proc`。 |
| `generate(doc)` | 積木 → 文字，**同時給每一行對應的積木 id**（`Generated::atLine`／`linesOf`）：診斷、中斷點、「顯示產生的文字」都靠它。格式固定：標籤頂格、指令縮排 4 格、關鍵字運算元依固定順序、Section 之間一個空行。 |
| 保證 | 文字 → 積木 → 文字，**組譯後的映像位元組與診斷碼相同**，且再轉一次文字不變。`asmcore_blocks_tests` 對 `examples/` 與 `sample/` 的**每個**程式（含 `.inc` 定義檔）檢查；除了故意寫壞的 `07_errors` 外，**全部不產生 Raw 積木**。 |

## `.acblk` 檔案（JSON）

```json
{
  "format": "acblk", "version": 1, "name": "hello", "hardware": "board.achw", "next_id": 26,
  "sections": [
    {"domain": "main", "comment": "", "blocks": [
      {"id": "b5", "type": "label", "fields": {"name": "start"}},
      {"id": "b6", "type": "mov", "fields": {"src": "CONST(0, 2)", "dst": "MAIN_SCRATCHPAD[0..1]"}, "comment": "counter = 0"},
      {"id": "b9", "type": "proc", "fields": {"name": "leaf"}, "stacks": {"BODY": [ {"id": "b10", "type": "ret"} ]}}
    ]}
  ]
}
```
欄位順序固定、縮排固定，適合進版本控制。讀檔錯誤會說明位置（「sections[0].blocks[3]: missing "type"」）；較新的版本會明確拒絕；缺 `id` 的積木會補上；欄位值是數字也接受。

## 命令列：`asmblk`

```
asmblk import prog.asm [-o prog.acblk]     文字 → 積木檔
asmblk gen    prog.acblk [-o prog.asm]     積木檔 → 文字（預設 prog.generated.asm；有結構錯誤時不輸出）
asmblk check  prog.acblk | prog.asm        檢查結構；對 .asm 還會把往返前後各組譯一次比對映像
asmblk blocks                              列出所有積木與欄位
```
結束碼：0 正常、1 有問題、2 用法／讀寫錯誤。

## 結構檢查（`validate`）

未知積木種類、必填欄位為空、封閉選項填了別的值、Main 積木放在 Peri 區段（反之亦然）、`.main/.peri` 之前就出現指令、`id` 重複、程序裡再放程序、積木沒有的槽——都用白話指出是**哪一個積木**。程式對不對（位址、名稱、型別…）仍是組譯器的事：把 `generate()` 的文字交給 `assemble()`。

## 擴充（給之後的階段）

新增一種積木＝在 `Registry::defaults()` 加一個 `BlockDef`（欄位、範本、domain），測試會要求詞彙表裡每個指令都有積木。範本語法：`{欄位}` 代入、`{?旗標:文字}` 旗標成立時才輸出、`[ … ]` 內含的欄位有空的就整段省略。需要多行或有邏輯的積木（`proc`、之後的 if／while）在 `generate()` 有自己的程式碼，高階積木會降階成 Level 1 文字。
