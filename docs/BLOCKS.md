# 積木程式（Block programs）— 階段 B0 與 B1-M1

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


---

# 自由擺放與編輯（階段 B1-M1，`asmcore/blocks_edit.h`）

圖形畫布（B1 後續里程碑）呼叫的都是這一層；它沒有 UI，所有規則與操作都在核心測試。

## 堆疊在畫布上的位置

`Section` 多了 `x`、`y` 與一種新的 domain `"floating"`：

| domain | 是什麼 | 產生程式？ |
|---|---|---|
| `"main"` / `"peri"` | 以 `.main` / `.peri` 帽子開頭的區段 | 是 |
| `""` | 檔頭的定義區 | 是（最先） |
| `"floating"` | 懸浮在畫布上、不接任何帽子的堆疊 | **否** |

**產生順序**：定義區在前，其餘帶帽子的區段依畫布位置由上到下、同高時由左到右（`programOrder()`）。匯入文字時，每個堆疊的 `y` 依檔案順序嚴格遞增，所以順序等於原檔；舊的 `.acblk`（沒有座標）也保持原順序。

## 連接規則（`checkSequence`、`checkSocket`、`Editor`）

任何會把積木放到某處的操作，都先在副本上套用，檢查「新出現」的違規（原本就有的不算），**硬規則違反就整個拒絕、文件完全不動**，軟規則則接受並回報警告。`dryRun` 只回答「能不能」與原因（畫布用它畫斜紋底色與提示）。

| 規則 | 等級 | 內容 |
|---|---|---|
| H1 | 硬 | 同一堆疊不能同時有 Main 專用與 Peri 專用積木（帶帽子的堆疊以帽子為準；漂浮堆疊以第一個專用積木為準） |
| H2 | 硬 | `.main`／`.peri`／定義區的開頭不能放到別的積木下面（`joinSection`） |
| H3 | 硬 | 程序不能放進程序 |
| H4 | 硬 | `ret` 不能在程式流程中（要在程序內；漂浮堆疊上先允許） |
| H5 | 硬 | 事件向量 `vector` 不能放進程序 |
| H6 | 硬 | 定義區只放 `directive`、註解、空行與 `raw` |
| H7 | 硬 | 值積木只能插進型別相符的值插槽；不能單獨站在程式堆疊裡（漂浮堆疊上單獨一個可以） |
| S1 | 軟（嚴格模式為硬） | 無條件跳躍／`ret`／`br_eret`／`BR.COND ALWAYS` 之後接的不是標籤（執行不到；註解、空行、停用的積木、`proc`、定義類不算） |

每個違規有 `rule`、哪個積木、白話 `reason` 與 `args`（積木標題等，讓介面能翻成中文）。`Raw` 積木不受檢查（內容未知）。

## 編輯操作（`Editor`）

位置用 `Where{section, container, slot, before}` 表示：在某個清單（區段頂層或容器的槽）裡、某個積木**之前**（`before` 空＝接在最後）；`connectionPoints(doc)` 列出所有位置。

`insertBlocks`、`removeBlocks`、`moveBlocks`（`takeBelow`＝連同後面的兄弟）、`splitAt`（從某積木起切成新的漂浮堆疊）、`joinSection`（把漂浮堆疊併進某處）、`newFloating`、`moveSection`、`attachToHat`／`detachFromHat`、`duplicate`、`setField`、`setComment`、`setDisabled`、`setCollapsed`、`plugValue`／`unplugValue`（值插槽；插進已佔用的插槽時舊的積木會漂在旁邊，不會消失）、`setSectionComment`、`removeSection`。

- 被拒絕或 dry-run 時不使用 id 計數器；成功時插入的積木 id 若與現有的衝突會自動換新。
- 把漂浮堆疊的積木全部搬走，空堆疊自動消失。

## 復原／重做（`EditHistory`）

快照式：成功的操作在 `Editor` 帶著 `EditHistory*` 時自動記一步；同一個欄位連續編輯（相同 mergeKey）合併成一步；上限可設；新操作丟棄「可重做」的尾巴；被拒絕的操作不記步；id 計數器不倒退（復原的積木 id 不會被重用）。`undoLabel()`／`redoLabel()` 給選單用。

## 剪貼簿與輸入

`copyBlocks`（JSON，格式名 `acblk-clip`；選到子孫又選到祖先只複製一次）、`pasteBlocks`（貼 JSON 會換新 id；貼組譯器文字會匯入；多個區段的標頭被丟掉）、`blocksToText`（純文字版）、`blocksFromText`（Typeblock 打一行：`start: MOV …` 得到標籤與指令兩個積木；看不懂的行回 Raw 並標記）、`checkFieldValue`（欄位即時提示：位移量 1 到 8、長度 1 到 32、未知區域名稱等；名稱與運算式不做範圍檢查）。

## 元件（M2b-1）：運算元也是積木

指令的運算元拆成**元件積木**，左右組合（設計見 `AssemblyCoreIDE/docs/BLOCKS_COMPOSITION.md`，逐指令分析見 `BLOCKS_INSTRUCTION_ANALYSIS.md`）：

| 元件 | type | 型別 | 例 |
|---|---|---|---|
| 數字（有屬性 `kind`／`sign`／`width`） | `num` | `int` | `5`、`CONST(5, 2)`（插進 `mov` 的來源＝常數）、`CONST(LIMIT + 1, 2)` |
| 名稱 | `name` | `int` | `LIMIT` |
| 運算／一元／括號 | `expr_bin`／`expr_un`／`expr_paren` | `int` | `a + b * 3`（括號只在優先順序需要時才加，與組譯器相同） |
| 區域（由 `isa::kRegions` 產生，每個區域一種） | `region_main_scratchpad` … `region_peri_rx_buf` | `region:<ID>` | `MAIN_SCRATCHPAD[0..1]`、`scratch[slot..+1]`、`MAIN_CONST_POOL[@k]`、`ALU_OPA` |
| 選項 | `alu_op`／`sign_mode`／`cond_main_c`／`cond_peri_c`／`label_ref`／`proc_ref` | `op.alu`…`label.proc` | `ADD`、`unsigned`、`ALWAYS` |

- **插槽與文字鏡像**：指令的運算元欄位（如 `mov.src`）有一個同名的插槽；插進元件時，欄位文字＝元件產生的文字（**鏡像**），沒有元件時就是打進去的文字。
  所以拆不開的運算元只是留成文字，**什麼都不會丟**。`len`／`ptr`（`csr_write_sp`、`csr_read_buf`、`pub` 的 `src`／`len`）由**窗口**（區域範圍）推導：`ptr = first`、`len = last − first + 1`。
- **檔案**：元件存在新的選用鍵 `parts`（M1 的 `values` 仍可讀）；格式版本仍是 1。舊版 IDE 不認得 `parts`、照文字欄位產生，結果相同；舊檔（只有文字）開啟時在記憶體中拆解，存檔才多寫 `parts`；
  兩個版本交替編輯時，**文字為準**（元件與鏡像不一致就丟掉元件、由文字重新拆）。
- **數字屬性**：`kind`＝`int`／`float`／`double`（浮點由產生器換成 IEEE-754 位元組，組譯器只有整數）、`sign`、`width`（8／16／24／32／64）；寬度＝`CONST` 的位元組數。超出範圍、
  64 位元與 `double`（單一 `CONST` 最多 4 位元組）會在積木上報錯並說明原因。
- **規則**（`validate`、`Editor::plugValue` 的 dry-run）：H7a 型別、方向（`ALU out` 不能當目的、`ALU A` 不能當來源、CSR 緩衝只能讀）、H7c 常數只能先進暫存區、H7d 範圍／長度相等、H7g 常數寬度、`alu` 依運算決定哪些插槽出現（EQ/NE 不可 signed、邏輯運算無正負、位移要有 1…8 的量）。
  插進已佔用的插槽 → 原積木彈出成漂浮積木（H7f）。
- 實際程式：範例與 examples 18 個檔 748 個元件；MOV 運算元 110/111、CSR 運算元 142/142、窗口 19/19 拆成元件，且產生的文字與位元組與原文字完全相同。

### 類別、嵌合形狀（使用者回饋 2026-10-22）

- **類別（`BlockDef::cls`）**：`instruction`（一行指令：move、calculate、CSR、分支、call／ret、xfer、pub…，上下疊）、`component`（元件：數字、區域、選項、參照，左右插）、`structure`（procedure、label、vector…）、`definition`（`.equ` `.csr`…）、`text`（註解、空行、組譯器文字）。
  指令與元件的**抽屜也不同**：指令用 Data／Math／CSR／Control／Peripheral；元件用 Values／Regions／Options／References（各有自己的顏色）。`asmblk blocks` 依類別分組列出。
- **嵌合形狀依型別**（`plugShapeOf`）：數字＝圓形凸耳、區域＝尖形、選項（運算／正負／條件）＝方形、標籤＝旗形、參照（CSR／service／裝置）＝菱形；接受多種形狀的插槽＝方形虛線凹槽。元件左緣有對應的凸耳，空插槽畫同形狀的凹槽，所以形狀不合一眼就看得出來。

### 符號表與參照檢查（M2b-2）

- **定義是型別化符號的生產者，元件是消費者**：`.equ`→`int`、`.csr`→`csr`、`.service`→`service`、`.devmap i, I2C, addr, name`→`dev`、`.const`→常數池項目、標籤→`label.flow`、`proc`→`label.proc`（含 8051 寫法 `NAME EQU v`）。來源：文件本身、`.include`（開放世界）、硬體描述（`.achw` 產生的 include）。
- **參照元件**：`csr_ref`／`service_ref`／`dev_ref`（`name` 仍是 `int`）。匯入時，定義為 CSR／service／裝置的名稱自動成為對應的參照；硬體描述後來才載入時，`retypeReferences` 把已知的名稱改成參照。
- **檢查（`validate`，`kind = "reference"`）**：名稱未定義（警告；有 `.include` 或硬體描述時提示可能來自那裡）、種類不符（CSR 參照指向 service：錯誤；標籤當數字：錯誤）、
  標籤可見範圍（程序內的標籤不可跳進：E305；Main／Peri 不可互相跳／呼叫）、`CALL` 遞迴（E305，每個環報一次）。
- **`xfer`**：`args` 拆成 dev／addr／addrlen／data／len／ptr 插槽（`ext` 位址位元組保留文字），顯示哪些依方向而定；三種形式各有規則（多位元組 WRITE 不可用、READ 要有 ptr、`ptr+len ≤ 32`、單位元組位址 10 位元、`addr` 給了 `addrlen` 不可為 0…）。
  順序與定義不同的文字保持為文字（不丟資料）；對拆不開的文字插入零件會被拒絕。

### 拖曳、空洞（M3 的核心互動，使用者回饋 2026-10-22）

- **拖出來**：拖曳一個積木會把它拉出來懸浮在畫布上——元件從插槽拖出（`unplugValue`）、指令連同下面的積木從堆疊拖出（`liftBlocks`；按住 Alt 只拖那一個）；單獨浮著的堆疊直接移動；拖曳帽子移動整個堆疊（限制在畫布內）。
- **空洞**：被拖走的元件在原插槽留下一塊**黑色空洞**（形狀是這個插槽接受的形狀，插槽文字鏡像也清空）；容器空的槽也是黑色空洞。有空洞的指令是不完整的：產生碼寫成 `; (incomplete) …`（絕不輸出半條指令），積木上有錯誤徽章說明缺什麼。
- **放回去**：拖曳中即時顯示放置目標（`findDropTarget`，與放開時的 `performDrop` 問同一個編輯器 dry-run）：元件看附近的插槽、指令看附近的連接點（積木下方、容器的槽、帽子下、浮動堆疊上方）。
  綠框＝可以、橘框＝可以但有警告、**紅色斜紋＝不能**（原因在提示裡）；放在空白處＝繼續浮著；Esc 取消、全部放回原位。整個拖曳（拉出＋放下）是**一個復原步驟**（`Editor::beginGroup`）。
- **Delete** 刪除選取（元件→留下空洞；指令→連同裡面的積木）；Edit ▸ Undo／Redo 對積木編輯器分頁生效。

### 軟嫩 Q 彈的效果（只影響畫面，不改文件與版面）

用**彈簧物理**（`services/blocks/block_spring.h`，欠阻尼，會過衝再晃到靜止），不是固定的緩動曲線：
- **抽離**：被拉出的積木「啵」一聲（先被壓扁再彈大，壓扁伸長保持體積感）；它離開的那一列從原來較寬的寬度**彈回收窄**到空洞的大小；拖曳時整疊會**依指標速度傾斜**（果凍般的慣性），指標停下就擺回。
- **插回**：放進黑色空洞時，那一列先是較窄，再**撐開、過衝、回彈**到容納積木的寬度；放進去的積木同樣「啵」地落位。
- 設定 `blocks.animate = 0` 可關閉；效果不改變文件、不影響測試與復原。

### M2b-3

- **位元**：`(1 << n)` 是 `bit_mask` 元件；`(1 << 3) | (1 << 5)` ＝兩個位元用 `|` 結合（`SET8`／`CLR8` 的遮罩）。其他運算式（`(2 << 1)`）仍是括號運算式。
- **cascade**：舊的 `cascade_begin`／`cascade_end` 型別不變（相容），畫面把整段框起來；檢查編號依序 0、1、2…（E304）、選擇值不超過最後區塊、離開標籤相同（W405）、BEGIN 要有 END。
- **旗標分支提示**（W506）：`BR.COND ALU_CMP_TRUE／ALU_OUT_OV16` 前一個積木不是 ALU（註解、空行、停用的不算；標籤之後不提示）。
- 沒做：描述子用量（組譯器的 `.map` 已經有精確數字，積木不重複）。

## 值插槽（為 B4 預留）

`BlockDef.inputs`／`valueType`、`Block.values`、範本中的 `{插槽名}`、型別檢查（H7）、`.acblk` 的 `"values"`。目前沒有 Level 1 積木使用，測試用自訂定義驗證（巢狀運算式產生、拒絕錯誤型別、插入已佔用插槽）。

## 產生器檔頭與輸出檔名

`generate(doc, reg, GenerateOptions{header})`：檔頭註解行會放在最上面，來源對照把它們算進行號（對應「無積木」）。`Document.output` 是產生的檔案名稱（`.acblk` 的 `"output"`）。
