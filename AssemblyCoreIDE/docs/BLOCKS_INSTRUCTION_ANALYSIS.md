# 指令拆解分析：每條指令由哪些模組組成、規則與相關性

> 依使用者 2026-10-22 的要求：「拆解分析一個指令如何被更細的拆開成模組，分析每一種指令的規則，找其相關性來拆解，增加積木編寫組譯器語法的彈性。」
> 資料來源：`docs/ASM_SYNTAX.md`、`core/src/lower.cpp`（實際的檢查）、`core/src/isa.h`（區域／條件表）、`core/src/diag_names.cpp`（錯誤與警告）。
> 本文件**是設計依據**：`BLOCKS_COMPOSITION.md` 的型別與規則由這裡導出；實作分三期（第 8 節）。

## 0. 分析方法

對每條指令問五個問題：

1. **成分**：它由哪些部分組成？每個部分是什麼型別、合法範圍從哪裡來？
2. **指令內的相依**：哪個部分決定另一個部分能不能用、能填什麼？（這是「拆解的線索」：互相決定的部分，要一起表達；彼此獨立的部分，各自成為模組。）
3. **跨指令的相依**：這條指令對前後的指令有什麼要求（資料流、旗標、結構）？
4. **可共用的模組**：這個部分在別的指令也出現嗎？（出現兩次以上就值得做成共用模組。）
5. **能不能做到「寫不出非法組合」**：若某些組合一定非法，能不能讓積木的形狀就排除它（比事後報錯更好用）？

## 1. 相容原則（先講，因為它限制拆解的方式）

**舊版 IDE 與舊檔都必須能用。** 因此：

- **陳述積木的 `type` 不改名、不新增**：`mov`、`alu`、`csr_*`、`br_cond`、`jmp`、`xfer`、`pub`… 保持原樣（舊版看到未知 type 會產生註解行 `; unknown block`，會讓程式壞掉）。
- **拆解只發生在 `parts`（新的選用鍵，舊版整個忽略）**，而原來的**文字欄位永遠保留為鏡像**（舊版照文字產生，結果相同）。
- 因此「同一個 `alu` 積木在不同運算下有不同的插槽」這種設計，**用「依目前選擇動態顯示插槽」實現**，而不是拆成四種 type。

## 2. 逐指令分析

### 2.1 MOV（規格 5）　`MOV 來源 -> 目的`

| 成分 | 型別／範圍 | 範圍從哪來 |
|---|---|---|
| 來源 | 區域範圍（可讀區域）｜常數 `CONST(運算式[, 位元組數])`｜常數池項目 `MAIN_CONST_POOL[@名稱]` | `isa::kRegions` 的 `readable`；常數寬度 1…4 位元組（`lower.cpp:103`） |
| 目的 | 區域範圍（可寫區域）｜ALU 運算元 | `isa::kRegions` 的 `writable` |

**指令內的相依（找到的關聯）**
- **長度相等**：兩側長度必須相同（常數的位元組數預設＝目的長度）。→ 來源與目的互相決定長度；一端知道長度，另一端就能檢查。
- **方向權限**：`ALU_OPA/OPB` 只能寫、`ALU_OUT/ALU_STATUS/MAIN_CSR_DATA_BUF/PERI_RESULT_*/MAIN_CONST_POOL` 只能讀（`MAIN_CSR_DATA_BUF` 在 v31 對 MOV 唯讀）。
- **常數來源 → 目的必須是暫存區**（E202）：來源種類決定目的的候選集。
- **ALU 運算元固定 offset 0、長度 2**：目的選了 `ALU_OPA/OPB`，範圍就不用（也不能）再填。
- **同區域搬移不可重疊**（E202）：來源與目的在同一區域時，兩段範圍互相約束。
- 範圍不可超出區域大小（暫存區 32、ALU_OUT 4…）。

**跨指令**：讀暫存區前要先寫過（W502，資料流）；ALU 運算元要兩個都載入（W501）；`PERI_RESULT_SLOT` 消耗前要先測 `PERI_RESULT_VALID`（W505）。

**拆解**：`MOV = from[來源模組] → to[目的模組]`。來源模組家族：{區域範圍、常數、常數池項目}；目的模組家族：{區域範圍、ALU 運算元}。
**區域範圍**再拆成 {區域, first, last}，其中 first/last 是 `int` 模組。這個「區域範圍」是**全系統最重要的共用模組**（見 2.3、2.7、2.8）。

### 2.2 ALU（規格 6）

```
ALU ADD|SUB|MUL|INC|DEC [signed|unsigned]        ← 算術類
ALU CMP.EQ|NE|LT|GE     [signed|unsigned]        ← 比較類（EQ/NE 不可 signed）
ALU AND|OR|XOR|NOT                                ← 邏輯類（無正負；給了就 W410）
ALU SHL|SHR 數量(1..8)  [signed|unsigned]        ← 位移類
```
| 成分 | 型別／範圍 |
|---|---|
| 運算 | 選項（19 個名稱，含 `EQ/NE/LT/GE` 別名），來源＝語言詞彙表 `group="alu"` |
| 正負 | 選項 `signed`／`unsigned`（預設 unsigned） |
| 位移量 | `int`，1…8，**只有 SHL／SHR** |

**指令內的相依（這條最有價值）**：**「運算」決定「正負」與「位移量」能不能出現**——
算術：正負可選；比較：正負可選但 `EQ/NE` 不可 signed；邏輯：**沒有**正負；位移：正負可選＋位移量必填 1…8。
**拆解**：運算選項元件本身帶一個「類別」屬性（arith／cmp／logic／shift）；`alu` 積木**依目前運算動態顯示插槽**：
選 `SHL` → 出現「位移 [1..8]」；選 `AND` → 「正負」插槽消失；選 `EQ` → 正負下拉只剩 `unsigned`。
→ 錯誤組合（W410、E203）**無法被表達**，不是事後報錯。
ALU 本身沒有運算元插槽（固定吃 `ALU_OPA/OPB`，產出 `ALU_OUT/STATUS`）；但這個**輸入／輸出關係是資料流的核心**，記錄成指令的「讀／寫集合」，供資料流警告與之後的配方積木使用（見 2.10）。

### 2.3 EXEC_CSR（規格 4）

六種形式，其實是兩族：

| 族 | 形式 | 成分 |
|---|---|---|
| 單位元組 | `WRITE8 / SET8 / CLR8 csr, imm`（Peri 也可）；`READ8 csr -> CSRBUF[n]` | csr、立即值（-128…255）、（READ8：目的位置 0…31） |
| 多位元組 | `WRITE_SP csr, len=N, ptr=P`；`READ_BUF csr, len=N, ptr=P` | csr、**len（1…32）**、**ptr（0…31）**，且 **ptr+len ≤ 32** |

**指令內的相依**
- `csr`：`.csr` 符號或 0…0xFFF 的運算式 → 型別「CSR 參照」（符號來自定義區／硬體設計器）。
- **(ptr, len) 其實就是一段位元組窗口**：`WRITE_SP` 的來源是 `MAIN_SCRATCHPAD[ptr..ptr+len-1]`；`READ_BUF` 的目的是 `MAIN_CSR_DATA_BUF[ptr..ptr+len-1]`。
  → **不要讓使用者填兩個互相牽制的整數，而是插一個「區域範圍」模組**（與 MOV 的區域範圍是同一個模組！），`len`、`ptr` 由它推導，`ptr+len ≤ 32` 自動成立。
- 多位元組時，CSR 位址也是一段：`csr[addr .. addr+len-1]`（連續 CSR）→ 顯示「寫入 AE_CTRL..AE_CTRL+3」。
- `SET8/CLR8` 的立即值是**遮罩**：由 `.csrbit` 定義的位元名稱組成（`(1<<3)|(1<<5)`）→ 之後的「位元遮罩」模組（需要硬體定義，第三期）。
- 每個相異的 (操作, 位址) 或 (操作, 頁, 長度, ptr) 組合占一個描述子（會共用，上限 32）→ 可在積木上顯示「描述子用量」。

**跨指令**：`READ8/READ_BUF` 之後 `MAIN_CSR_DATA_BUF` 才有資料（W502）；`WRITE_SP` 的來源暫存區要先寫過（W502）。

**拆解**：`csr 參照` ＋ 依形式不同的 {立即值｜遮罩｜區域範圍窗口}。

### 2.4 分支（規格 7）

| 指令 | 成分 |
|---|---|
| `BR.COND 條件, 標籤 [, safe]` | 條件、目標標籤、safe 旗標 |
| `JMP 標籤 [, safe]` | ＝ `BR.COND ALWAYS`（`jmp` 是 `br_cond` 的特例） |
| `BR.ERET` | 無成分（描述子欄位固定 0，不可帶 safe：E208） |
| Peri：`BR.<條件> 標籤` | 條件取自 **Peri 條件表**（與 Main 不同的詞彙） |

**指令內的相依**
- **條件的詞彙依 domain 而異**：`cond.main`（15 個，`PROFILE_COND0` 需 profile 啟用）／`cond.peri`（8 個）。→ 型別分兩種，Peri 堆疊裡插不進 Main 條件。
- **目標標籤的可見範圍**：流程標籤是 domain 內全域；`.proc` 內的標籤是區域的；**不可跳進程序**（E305，要用 CALL）；目標要在 8 位元範圍內（E310，組譯時檢查）。→ 型別 `label.flow`（`CALL` 用 `label.proc`，二者不同）。
- `safe` 是**共用旗標模組**（`br_cond`、`jmp`、`br_peri` 都有；`CASCADE_*`、`BR.ERET` **沒有**，形狀上就排除）。
- 「旗標條件」（`ALU_CMP_TRUE`、`ALU_OUT_OV16`）必須在 ALU 運算之後（W506）；`PERI_RESULT_VALID` 之後才能消耗結果槽（W505）。→ 條件元件帶「需要的前置狀態」屬性，編輯器可在「上一個積木不是 ALU」時提醒（跨指令規則，軟性）。

**拆解**：`條件元件（cond.main／cond.peri，帶前置狀態屬性）＋ 標籤參照（label.flow）＋ safe 旗標`；`jmp` ＝ 條件固定 ALWAYS。

### 2.5 CASCADE_BEGIN／CASCADE_END（規格 7.6）

```
CASCADE_BEGIN 選擇值k, 區塊0標籤
 …區塊0…   CASCADE_END 0, 離開標籤
 …區塊1…   CASCADE_END 1, 離開標籤
```
相依：區塊必須連續、索引依序 0,1,2…、BEGIN 的選擇值 ≤ 最後索引（E304）、所有 END 的離開標籤相同（W405）、區域內不可有一般分支／ERET／safe。
**觀察**：這是**一個容器**（C 形，N 個槽），不是 N+1 條獨立指令。索引 0..n-1、離開標籤共用、區塊連續全都**由結構自動決定**。
**拆解（第三期，結構積木）**：`cascade [選擇值]` 容器，內含任意個「區塊槽」；產生時輸出 BEGIN／各區塊／各 END。索引與離開標籤不再由人填。
（舊檔的 `cascade_begin`／`cascade_end` 兩個 type 保留，匯入時由結構辨識器合併為容器放在 `parts`；相容原則 §1。）

### 2.6 程序：`.proc`／CALL／RET（第 6 節）

成分：程序名稱（定義一個**程序符號**）、CALL 的目標（使用一個**程序符號**）。
相依：程序以 RET 或無條件分支結尾（E305）；不可遞迴；`RET` 只能出現在程序內（H4）；分支不可跳進程序；Main／Peri 不互相呼叫。
**拆解**：`proc` 容器（已有）定義符號；`CALL` 的 target 是 `label.proc` 參照（下拉＝目前檔案的程序名，**依 domain 過濾**）。
跨指令：呼叫圖無環 → 在 `CALL` 積木上即時檢查（能不能接：目標程序沒有（間接）呼叫到目前所在程序）。

### 2.7 EXEC_PUB（Peri，規格 10）　`EXEC_PUB service=S, src=N, len=L`

成分：service（1…15）、src（0…31）、len（1…32）。
**相依**：`(src, len)` ＝ `PERI_RX_BUF[src..src+len-1]`（與 2.3 完全同構）；`src+len ≤ 32`。
**跨 domain 相依（重要）**：`service` 的 `len` 契約：`.service NAME = id, len=N` 宣告後，Peri 發佈長度與 Main 消耗長度必須相等（W507）；發佈的位元組必須先被 XFER 讀進 `PERI_RX_BUF`（W508）。
**拆解**：`service 參照`（型別，來源：`.service`）＋ **區域範圍窗口（PERI_RX_BUF）**。

### 2.8 EXEC_XFER（Peri，規格 8／9）

三種形式（`lower.cpp: doXfer`）：

| 形式 | 成分 |
|---|---|
| one-byte WRITE | dev、addr（≤10 位元）、addrlen、**data**（-128…255） |
| one-byte READ | dev、addr、addrlen、**ptr**（讀到 `PERI_RX_BUF[ptr]`） |
| multi-byte READ | dev、**len**、**ptr**、ext=[b,b]（0…2 個內嵌位址位元組） |

**相依**
- `dev`：`.devmap` 項目（0…7）→ 型別「裝置參照」（來源：`.devmap`／硬體設計器）。
- `addr` 與 `addrlen`：addr>0xFF 則 addrlen 必為 2；addrlen=0 則 addr 必為 0；one-byte 的 addr ≤ 0x3FF（E206）。→ **addrlen 可由 addr 推導**（只要給 addr，addrlen 預設自動）。
- **形式由有哪些參數決定**（有 `len` ⇒ multi-byte；multi-byte WRITE 在 baseline 非法）。
- `(ptr, len)`＝`PERI_RX_BUF[ptr..ptr+len-1]`（與 PUB 同構）；ext 的個數 ＝ addrlen。
**拆解**：`xfer` 積木**依「方向＋形式」動態顯示插槽**（`rw` 選擇後只出現合法的 {dev, addr, data}｜{dev, addr, ptr}｜{dev, len, ptr, ext…}），原本自由文字的 `args` 改為由插槽產生的鏡像。

### 2.9 指示字（定義區）：符號的生產者

| 指示字 | 定義的符號型別 | 成分 |
|---|---|---|
| `.equ NAME = expr` | `int` | 名稱、`int` 運算式 |
| `.const NAME db\|dw v,v…` | 常數池項目 | 名稱、寬度（db/dw）、**int 清單** |
| `.csr NAME = 0x123` | CSR 參照 | 名稱、12 位元位址 |
| `.csrbit CSR[b] = 角色` | 位元遮罩 | CSR、位元、角色 |
| `.service NAME = n [, len=N]` | service 參照 | 名稱、1…15、長度契約 |
| `.devmap i, I2C, addr7 [, name]` | 裝置參照 | 索引 0…7、匯流排、7 位元位址 |
| `.vector id, label` | — | 事件 id（0…event_count-1）、`label.flow` |
| `.wcet name, from, to` | — | 兩個 `label.flow` |
| `.assume REGION[a..b] defined` | — | **區域範圍**（只限可被外部初始化的區域） |

**關鍵觀察：定義（生產者）與使用（消費者）有型別化的相關性。**
`.equ` 產生 `int` 符號 → 任何 `int` 插槽的「名稱」模組下拉列出它；`.csr` 產生 CSR 參照 → `csr` 插槽下拉列出它；`.service` → `pub.service`；`.devmap` → `xfer.dev`；`.const` → `MAIN_CONST_POOL[@名稱]`。
→ 做成**型別化的符號表**（由目前文件與硬體描述 `.achw` 產生，不手抄）：名稱模組的下拉只會列出「型別對得上」的符號；定義被刪除或改名時，使用處即時出現徽章。這同時是 B2（硬體設計器整合）的基礎。

### 2.10 跨指令的「配方」（高階積木 B4 的素材）

幾個固定的多行模式，**就是這些模組的固定組合**，拆解之後可以做成配方積木（產生多行，仍可還原成原文字）：

| 配方 | 展開 |
|---|---|
| 載入常數到 ALU | `MOV CONST(k,2) -> SCRATCH[a..a+1]`；`MOV SCRATCH[a..a+1] -> ALU_OPx` |
| 計算 `dst = A op B` | 載入 A、載入 B → `ALU op` → `MOV ALU_OUT -> dst` |
| 讀 CSR 到暫存區 | `EXEC_CSR READ_BUF …` → `MOV CSRBUF[..] -> SCRATCH[..]` |
| 寫常數到 CSR | `MOV CONST -> SCRATCH` → `EXEC_CSR WRITE_SP …` |
| 比較後分支 | `ALU CMP.x` → `BR.COND ALU_CMP_TRUE, label` |

這些配方用到的相依（常數必先進暫存區、ALU 結果要搬走、旗標條件要接在 ALU 後面）正是 2.1–2.4 找到的規則——有了模組化的區域範圍與型別，配方積木只是「把已有模組以固定方式串起來」。

## 3. 共用模組目錄（由上述分析歸納）

| 模組 | 出現在 | 型別 | 內含 |
|---|---|---|---|
| **數字／運算式** | 幾乎所有指令 | `int`（可加 `kind/sign/width/const` 屬性） | 數字、名稱、運算、括號、一元運算 |
| **區域範圍** | MOV 兩端、WRITE_SP、READ_BUF、READ8、PUB、XFER ptr/len、.assume | `region:<ID>` | 區域＋first＋last（`len`、`ptr` 皆由它推導） |
| **常數** | MOV 來源 | 數字（有寬度）或常數池項目 | `CONST(值, 位元組數)` |
| **CSR 參照** | 所有 EXEC_CSR | `csr` | 符號或 0…0xFFF；多位元組時為連續區間 |
| **標籤參照** | 分支、`.vector`、`.wcet` | `label.flow` | 目前 domain 的流程標籤（區域標籤依可見範圍） |
| **程序參照** | CALL | `label.proc` | 目前 domain 的程序 |
| **條件** | br_cond、br_peri | `cond.main`／`cond.peri` | 帶「前置狀態」屬性 |
| **safe 旗標** | br_cond、jmp、br_peri | 旗標 | — |
| **運算（ALU）** | alu | `op.alu`（帶類別） | 決定正負／位移量是否出現 |
| **正負** | alu | `sign` | `signed`／`unsigned` |
| **service／裝置參照** | pub／xfer | `service`／`dev` | 來自 `.service`／`.devmap` |
| **位元遮罩** | SET8／CLR8 | `int` | 來自 `.csrbit`（第三期） |

## 4. 規則總表（每條規則歸屬哪個模組或哪一對模組）

| 規則 | 對象 | 實作方式 |
|---|---|---|
| 方向權限（可讀／可寫） | 區域範圍 ↔ MOV 兩端／READ8 目的／WRITE_SP 來源 | **型別**：插槽只列出權限符合的區域 id（由 `kRegions` 產生）|
| 常數來源 → 目的暫存區 | MOV：來源 ↔ 目的 | 兩插槽的組合規則 |
| 兩側長度相等 | MOV：來源 ↔ 目的 | 兩插槽的組合規則（兩端都是數字時檢查；否則交給組譯器）|
| 範圍在區域內、first ≤ last | 區域範圍 | 模組自身規則（大小取自表）|
| ALU 運算元 offset 0／長度 2 | MOV 目的＝ALU 運算元 | 形狀：該區域積木沒有範圍插槽 |
| 運算決定正負／位移量 | alu：運算 ↔ 其他插槽 | **動態插槽**（不可能表達 W410／E203）|
| `ptr + len ≤ 32` | WRITE_SP／READ_BUF／PUB | **自動成立**（由區域範圍推導）|
| 條件詞彙依 domain | br_cond／br_peri | **型別**（cond.main／cond.peri）|
| 不可帶 safe | BR.ERET／CASCADE | **形狀**（沒有該插槽）|
| 標籤可見範圍、不可跳進程序 | 分支 ↔ 標籤參照 | 型別 `label.flow` ＋ 可見範圍檢查 |
| addrlen 由 addr 推導；addr ≤ 10 位元 | xfer one-byte | 動態插槽＋推導 |
| 旗標條件之前要有 ALU | br_cond ↔ 前一個積木 | 跨指令軟性規則（提醒）|
| 資料流（W50x） | 全部 | 仍由組譯器的資料流檢查負責；積木只顯示徽章 |

**原則**：能用「形狀／型別／推導」讓錯誤無法發生的，就不要留到事後報錯；跨多個積木、需要全程式資訊的（資料流、描述子容量、程式大小），仍交給組譯器，積木上只顯示它的診斷。

## 5. 分解後帶來的彈性（對使用者的實際好處）

1. **像組樂高一樣寫指令**：`MOV` 的來源與目的各自是一塊可獨立拖動、複製、重複使用的元件。
2. **不再有互相牽制的整數**：寫 CSR 多位元組時，插一個「暫存區 4..7」就同時決定 `ptr=4`、`len=4`。
3. **不可能寫出非法組合**：`AND` 後面根本沒有正負下拉；`ALU out` 放不進 `MOV` 的目的。
4. **名稱不會打錯**：CSR／service／裝置／常數都來自定義的下拉，刪除定義時使用處馬上標示。
5. **配方可以建立在模組上**（B4）：`dst = A + B` 一塊積木展開成四條指令，仍可還原成原文字。
6. **文字永遠不被拋棄**：拆不開的運算元留成文字，不影響組譯；舊版與舊檔照常使用。

## 6. 型別系統（修訂 `BLOCKS_COMPOSITION.md` 第 3 節）

原本的 `region.r/w/rw` 改成**精確的區域 id 型別**，方向由插槽的接受清單表達（比三種粗型別更準，錯誤訊息也更具體）：

| 型別 | 例 |
|---|---|
| `int` | 數字、名稱、運算 |
| `region:<ID>` | `region:MAIN_SCRATCHPAD`、`region:ALU_OPA`… |
| `csr`、`service`、`dev` | 參照模組 |
| `label.flow`、`label.proc` | 標籤／程序參照 |
| `op.alu`、`sign`、`cond.main`、`cond.peri` | 選項元件 |

插槽的接受清單由表**產生**：`mov.from` ＝ 所有 `readable` 區域＋常數；`mov.to` ＝ 所有 `writable` 區域；`csr_read8.into` ＝ 只有 `region:MAIN_CSR_DATA_BUF`；`csr_write_sp.from` ＝ 只有 `region:MAIN_SCRATCHPAD`；`pub.from` ＝ 只有 `PERI_RX_BUF`。
（`PERI_RX_BUF` 目前不在 `kRegions`，因為它不是 MOV 區域；區域積木表會把它與 Peri 專用區域一起加入。）
注意：`H7e`（域）不需要——區域積木本身標示 domain，沿用既有的 H1。

## 7. 與使用者先前決定的對照

| 決定 | 這份分析如何落實 |
|---|---|
| ①指令可拆成元件 | 第 2、3 節：每條指令的成分與共用模組 |
| ②數字是有屬性的積木 | `int` 模組帶 `kind/sign/width/const`；常數寬度 1…4 位元組（組譯器上限）|
| ③插入已佔用插槽 → 原積木彈出 | 規則 H7f，不變 |
| ④舊檔與舊版 IDE 都能用 | 第 1 節相容原則 |
| ⑤先做 M2b 再做 M3 | 第 8 節分期 |

## 8. 實作分期（M2b-1、M2b-2 已完成：2026-10-22）

M2b-2 完成：型別化符號表（`.equ`／`.csr`／`.service`／`.devmap`／`.const`／標籤／程序）、`csr_ref`／`service_ref`／`dev_ref`、參照檢查（未定義／種類不符／可見範圍／遞迴）、`xfer` 依方向與形式的動態插槽與規則、硬體描述的符號進入檢查（`hardwareSymbols`）。
M2b-3 尚未做：`cascade` 容器、`.csrbit` 位元遮罩、旗標條件前置狀態提醒、描述子用量。



M2b-1 實測：範例與 examples 18 個檔、748 個元件；MOV 運算元 110/111、CSR 運算元 142/142、窗口 19/19 拆成元件；`assemble(generate(import(t)))` 位元組與診斷與原文字完全相同；
600 個隨機運算式樹（無括號）→ 積木 → 文字，組譯器求值與樹相同；600 個隨機含括號運算式文字 → 積木 → 文字完全相同。



| 期 | 內容 | 需要硬體設計器資料？ |
|---|---|---|
| **M2b-1**（先做） | `int` 模組（數字屬性／運算／名稱）、**區域範圍**模組（含推導 len/ptr）、常數、`mov`／`csr_*`／`pub` 的插槽、選項元件、`alu` 動態插槽、`safe` 共用旗標、運算元／運算式 parser、鏡像與一致性驗證、規則（方向／常數→暫存區／長度／範圍／寬度）、相容測試 | 否 |
| **M2b-2** | 型別化符號表（`.equ`／`.csr`／`.service`／`.devmap`／`.const`／標籤／程序 → 名稱模組的下拉與即時徽章）；`xfer` 動態插槽；`CALL` 呼叫圖檢查 | 部分（B2） |
| **M2b-3** | `cascade` 容器（結構辨識）、位元遮罩（`.csrbit`）、旗標條件的前置狀態提醒、描述子用量顯示 | 是 |

M3（拖曳吸附）在 M2b-1 之後即可開始，因為連接點與型別已齊備；M2b-2／3 與 M3 可交錯進行。
