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

### 迴圈積木（loop）

Control 抽屜的 **loop**（只在 Main）：有一個放積木的 C 形槽，標題上是**下拉選單「repeat」**：
- **for**：多一格「**times**」填次數（1…65535，也可填 `.equ` 名稱），以及選填的「**counter at**」。產生的程式與範例 `01_hello_csr` 手寫的一樣：暫存區 6 個位元組放計數／步進／上限，`ALU ADD` 加一、`ALU CMP.EQ` 比較、`BR.COND ALU_CMP_TRUE` 離開、`JMP` 回頭——**迴圈本體跑剛好 N 次**（測試用模擬器數過）。
- **forever**：標籤＋本體＋`JMP` 回頭。
- 計數器位置留空＝自動：最外層用暫存區 26…31，再往內依序 20、14、8、2，**巢狀的迴圈不會互相覆蓋**；自己填的位置超出 0…26、或和外圈的重疊會顯示錯誤。本體裡不要動到這 6 個位元組。
- 迴圈產生的行（計數、比較、跳轉）都屬於 loop 積木，組譯訊息會顯示在它身上；本體各行屬於本體裡的積木。

### 工作區整合（M5）

- **積木是唯一的真相**：每次編輯，`.acblk`（含各堆疊位置）與它產生的組譯檔都會放進工作區緩衝區（未存檔），**建置與除錯立刻用得到，不必先存檔**；Save All 兩個一起寫；關閉分頁選「放棄」兩個都還原；分頁星號涵蓋兩個檔案。只是**開啟**積木程式不會改任何檔案。外部改了 `.acblk` 會重新載入，產生檔跟著更新。
- **產生的檔案**：由 `.acblk` 的 `output` 指定（相對於它的資料夾），預設 `NAME.generated.asm`；檔頭有「Generated from X.acblk - edit the blocks, not this file」。**漂浮堆疊不輸出**。
- **專案**：`block = 檔名.acblk`（可多個）；產生檔會排進來源清單；專案樹有「積木」群組，產生的來源以斜體標「（產生）」、懸停顯示來自哪個積木。第一次編輯時自動登記（只限專案資料夾內的程式）。
- **產生檔唯讀**：在文字編輯器開產生檔，上方有橫幅「這個檔案由 X.acblk 產生，請編輯積木」與〔開啟積木〕按鈕；不能輸入、取代、註解、複製行、搬行，跨檔「重新命名符號」若會改到產生檔也會擋下並說明。**除錯、中斷點、跳到定義、搜尋照常可用**。
- **轉換**：File → Block Program → **Show Current File as Blocks**（轉換文字檔）：`output` 設成**原來的檔名**，專案來源清單與所有 `.include` 都不必改；原文先備份成 `NAME.asm.bak`（只備份一次），第一次編輯積木後原檔才變成「產生的檔案」。**Convert to Text (Detach)**：產生檔變回一般文字（去掉產生檔頭、存檔），積木程式留在磁碟但離開專案。
- **建置錯誤顯示在積木上**：組譯器對產生檔的錯誤／警告，依行號對回產生那一行的積木，右上角 ⚠／✖，懸停顯示訊息與 `asmc --explain` 的修法。只在建置的文字與目前產生的文字相同時顯示（編輯後行號可能移動，徽章先消失，等下一次建置）。
- **狀態列**：「N 串浮動積木」「N 行以文字保留」「N 個問題」都是連結，點一下選取並顯示下一個（再點跳到再下一個）。

### 工具箱與 Typeblock（M4）

- **工具箱**（畫布左側，工具列「Toolbox」按鈕可收起）：**抽屜由登錄表產生**（新增積木定義不用改畫面）——範式、五個指令抽屜（Data／Math／CSR／Control／Peripheral）、結構、定義、文字，然後是四個元件抽屜（Values／Regions／Options／References）；每個抽屜有自己的顏色，**指令與元件從不混在同一個抽屜**；Main 專用／Peri 專用的標示在名稱後。
  上方搜尋框即時過濾（名稱、類型、助記符如 `EXEC_CSR`、抽屜；字母依序也找得到，`mv`→move）。**拖曳**樣本到畫布：和拖動別的積木一樣即時顯示放置目標（綠／橘／紅色斜紋與原因）、吸附、放在空白處就浮著、離開畫布取消；**雙擊或按 Enter**＝接在選取的積木後面（沒有選取就放在畫面中央）。樣本預填：指令是一行寫好的組譯器經匯入器拆成元件（`MOV CONST(0, 2) -> MAIN_SCRATCHPAD[0..1]`），元件帶著零件（區域有 `[0..1]`、運算有兩個數字）。
- **範式**（可一次拖出多個積木）：把常數載入 ALU、兩數相加、用常數寫入 CSR、等到 ALU 回答成立（輪詢）、讀取裝置並發佈（Peri）。每個範式測試過可以組譯。
- **Typeblock**：在畫布上**直接打字**（任何可印字元，或 Ctrl+Space）跳出輸入框與候選清單：名稱符合的積木；**打的是一整行組譯器**（`ALU SUB unsigned`、`start:`、`.equ X = 1`）就以「以組譯器插入」排在最前面，Enter 就變成積木（有元件）；看不懂的行保留為文字並提示。↑↓／Tab 選擇、Esc 取消。
  插入的位置：有選取的指令就接在它下面（**連續輸入多行＝一行接一行**）、單一元件放進選取積木的空插槽、沒有選取時若有空的程式部分（新檔的 `.main`）就放進去、否則成為畫面中央的浮動堆疊。**純鍵盤可打出整個程式**（測試：不用滑鼠打出 `01_hello_csr` 的 21 行並組譯成功）。
- **自訂工具箱項目**：每個抽屜標題右側有 **＋** 按鈕；在抽屜上（或空白處）按**右鍵 →「新增積木…」**也可以。對話框選抽屜、取名，再寫組成它的組譯器文字（多行＝多個積木；元件抽屜寫一個運算元，如 `MAIN_SCRATCHPAD[4..7]`、`LIMIT + 1`），即時告訴你做得出幾個積木、有幾行會保留為文字；同一抽屜名稱不可重複。自訂項目以斜體加 ★ 顯示，右鍵可編輯／刪除，存在設定（`blocks.custom`），下次啟動還在，也能用 Typeblock 找到。
- **把拼好的積木存成新積木**：在畫布上框選（或 Shift 點選）兩個以上接在一起的積木，**右鍵 →「將選取的積木存成新積木…」**，輸入名稱（預設放進「範式」抽屜，可改抽屜），之後從工具箱拖出就是同一組積木。
- 積木標題、欄位標籤、抽屜與範式名稱都有繁體中文（`BlockTitle`／`BlockField`／`BlockDrawer`／`BlockPattern`）。

### 編輯（M3）

| 要做的事 | 怎麼做 |
|---|---|
| 改欄位／運算元／數字 | **雙擊**那一塊（或選取積木後按 Enter／F2 編輯它的第一個部分）→ 在原處開編輯框：Enter 保留、Esc 取消、**Tab／Shift+Tab 保留並跳到下一個／上一個部分**。選項有下拉清單（運算、條件、…），名稱有下拉與行內補全（CSR、service、裝置、`.equ` 數字、標籤、程序，來自符號表與硬體描述）；打字時即時提示（例如位移量是 1 到 8）。打進去的文字能讀成元件就變成元件（`5` 成為數字積木，`MAIN_SCRATCHPAD[n..n+1]` 成為區域）；讀不通的保留為文字，積木上會標出問題。空洞也是這樣填 |
| 開關旗標（event checkpoint…） | 單擊旗標 |
| 結尾註解 | 雙擊積木或帽子的尾端（多行：Shift+Enter） |
| 移動／抽出／放回 | 拖曳（見上）；拖帽子＝移動整個堆疊並改變順序 |
| 選取 | 點選、Ctrl 加選、框選、Ctrl+A；**方向鍵**：↑↓ 前一個／下一個指令（跨堆疊）、← 到包住它的、→ 進入它裡面（第一個元件或槽內第一塊）；Esc 取消 |
| 複製／剪下／貼上 | Ctrl+C／X／V（Edit 選單也是）：剪貼簿同時有積木本身（貼到另一個積木編輯器）與組譯器文字（貼到文字編輯器）；貼上的文字會變成積木。貼上時：有選取的指令就接在它下面；單一元件放進選取積木的空插槽；否則成為畫面中央的浮動堆疊 |
| 複製一份／停用／折疊 | Ctrl+D／Ctrl+E（或 Ctrl+/）／右鍵；停用的積木寫成註解，容器可折疊 |
| 刪除 | Delete（元件→留下空洞） |
| 復原／重做 | Ctrl+Z／Edit 選單：拖曳、貼上、複製一份、整理排版、每次保留的註解或欄位編輯各是一步 |
| 右鍵選單 | 依選取顯示可用的動作；空白處：貼上、全選、整理排版、顯示全部 |
| **整理排版** | 工具列「整理」或右鍵：定義在左上、Main 在左欄、Peri 在下一欄、不屬於程式的堆疊在最右欄；**程式本身不變**（產生的文字與順序相同，每個部分嚴格在前一個之下）；一次復原 |
| 帽子順序號 | 每個帽子左上角的圓點是它在程式中的位置（1、2、3…，就是文字寫出的順序）；拖動帽子改變順序，文字隨之改變 |

### 拼圖凸角、結尾註解（使用者回饋 2026-10-22）

- **所有疊放的積木都是拼圖**：Main／Peri／定義的帽子、`start:` `loop:` `done:` 這類標籤、指令、程序、註解與組譯器文字，下緣都有**圓滑的凸角**（一座平滑的小丘），上緣有形狀完全相同的**凹角**——一塊接在另一塊下面時剛好嵌合，沒有尖角。（元件則是左緣的型別凸耳與插槽凹槽。）
- **結尾註解**：每個積木（指令、標籤、程序、組譯器文字、帽子）的**尾端**有一塊空白區，滑鼠移上去會浮出淡淡的 `; …`；**雙擊尾端**就在原處開一個編輯框：Enter 保留、Shift+Enter 換行、Esc 取消、點別處也會保留。
  註解可以有**多行**：產生碼的第一行放在指令那行的尾端（`; …`），其餘各行接在後面各成一行 `; …` 註解（`.main` `.proc` 也一樣）；畫面上積木整塊**變高**去容納，
  **插在裡面的元件跟著長高、再往裡面的元件也是**（置中對齊，連接點隨之移動）；容器（procedure）的註解撐高的是它的頭，本體在下面照常；下面的積木往下推。每次保留是一個獨立的復原步驟。

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
