# AssemblyCore 組合語言參考

對象：Sensor Micro-Sequencer ISA v1.6 r10，**規格 v31**（`docs/MicroISA_Spec_v31.pdf`；交接說明 `docs/MicroISA_v31_assembly_compiler_handoff.md`）。
從 v24 升級請看 `docs/SPEC_V24_VS_V31.md`（逐項差異）與 `docs/V31_MIGRATION.md`（遷移進度）。
規格書只給了位元編碼與偽碼範例，**沒有定義組合語言的原始碼語法**；本文件就是這套組譯器的語法定義。
指令的寫法盡量沿用規格書範例（`MOV 來源 -> 目的`、`BR.COND cond, label`、`EXEC_CSR WRITE_SP ...`），
結構（標籤、註解、`EQU`、`CALL`／`RET`）採 8051 組譯器的慣例。

## 1. 使用

```
asmc [選項] file.asm [file2.asm ...]
  -o PREFIX            輸出名稱前綴（預設：第一個輸入檔去掉副檔名）
  --call-model M       inline（預設）| softret，CALL/RET 的實作方式（第 6 節）
  --profile FILE       目標 profile（資料表深度、延遲、softret 保留位置）
  -I DIR               .include 的搜尋目錄（同時搜尋含入檔所在目錄）
  --list --map --dbg --hex     產生 .lst / .map / .dbg.json / .hex
  --wcet A:B           Main 標籤 A 到 B 之間的最壞情況週期數（可重複）
  --pad-tables         每個描述子表補到 profile 的深度
  --werror             警告視為錯誤
```

輸出：`PREFIX.main.bin`、`PREFIX.peri.bin`（映像檔，第 8 節）。有錯誤時不寫任何檔案，結束碼 1。

建置本專案（只需要 C++17 編譯器與 CMake）：`cmake -S . -B build && cmake --build build && ctest --test-dir build`。

## 2. 原始碼基本規則

| 項目 | 規則 |
|---|---|
| 註解 | `;` 到行尾 |
| 標籤 | `name:`（一行可有多個）；程式碼區（flow）內全域，`.proc` 內為該程序的區域標籤 |
| 大小寫 | 助記符、區域名、關鍵字不分大小寫；符號與標籤**分大小寫** |
| 數值 | `123`、`0x7F`、`7Fh`（8051 寫法，需以數字開頭，如 `0FFh`）、`0b101`、`101b`、`'A'` |
| 運算式 | `( ) + - * / % << >> & ^ \| ~`，可使用已定義的符號 |
| 定義順序 | `.equ` / `.csr` / `.service` / `.const` 必須先定義後使用；標籤可向前參照 |
| 區域 | `.main`、`.peri` 切換目前的 domain；一個檔案可同時含兩者 |

## 3. 指示字

| 指示字 | 說明 |
|---|---|
| `.main` / `.peri` | 之後的指令屬於 Main / Peri |
| `.include "file"` | 含入檔案（建議放共用的 `.csr`、`.service`、`.devmap`） |
| `.equ NAME = expr`（或 `NAME EQU expr`、`.equ NAME, expr`） | 常數 |
| `.csr NAME = 0x123` | CSR 位址（12 位元）。**規格書只命名 CSR 沒有編號，位址由你的 CSR 表決定** |
| `.service NAME = n` | service_id（1..15；0 保留）。Main 與 Peri 共用同一份配置 |
| `.const NAME db\|dw v, v...` | 放進 MAIN_CONST_POOL 的常數（dw 為小端）。`NAME` 的值是它在常數池的位移 |
| `.devmap i, I2C, addr7 [, name]` | DeviceMap 第 i 項（0..7）。目前只支援 I2C |
| `.proc NAME` … `.endp` | 程序（第 6 節） |
| `.wcet NAME, from, to` | 登錄一組 WCET 查詢（Main，標籤到標籤） |
| `.assume REGION[a..b] defined` | 宣告這段位元組由別人初始化（開機流程、另一個 domain…），資料流檢查不再要求先寫入。Main：`MAIN_SCRATCHPAD`、`MAIN_CSR_DATA_BUF`；Peri：`PERI_RX_BUF` |
| `.end` | 停止讀取此檔案 |
| `.org` | **不支援**：PC 固定從 0 開始，映像檔佈局由規格書固定（規格 3.1） |

## 4. Main 指令

### MOV（規格 5）
```
MOV 來源 -> 目的
```
區域寫法：`區域[a..b]`（含 b）、`區域[a]`、`ALU_OPA` / `ALU_OPB`（固定 offset 0、長度 2）、
`MAIN_CONST_POOL[@常數名]`（該常數的位移與長度）、`CONST(運算式[, 位元組數])`（自動放進常數池並去重；
位元組數預設等於目的的長度）。

區域名：`MAIN_SCRATCHPAD`、`MAIN_CSR_DATA_BUF`、`PERI_RESULT_SLOT`、`ALU_OPA`、`ALU_OPB`、`ALU_OUT`、`ALU_STATUS`、
`MAIN_CONST_POOL`、`PERI_RESULT_META`（亦可用 `SCRATCH`、`CSRBUF`、`SLOT`、`CONST`、`META` 等縮寫）。

**資料流向（v31 5.1）**：
```
MAIN_CONST_POOL --MOV--> MAIN_SCRATCHPAD --MOV--> ALU_OPA / ALU_OPB --ALU--> ALU_OUT --MOV--> MAIN_SCRATCHPAD --EXEC_CSR WRITE_SP--> CSR
EXEC_CSR READ8 / READ_BUF --> MAIN_CSR_DATA_BUF --MOV--> MAIN_SCRATCHPAD 或 ALU_OPA / ALU_OPB
PERI_RESULT_SLOT --MOV（消耗）--> MAIN_SCRATCHPAD
```
| 區域 | MOV 來源 | MOV 目的 |
|---|---|---|
| `MAIN_SCRATCHPAD` | ✔ | ✔ |
| `MAIN_CSR_DATA_BUF` | ✔（CSR 讀回的位元組） | ✘ 只有 `EXEC_CSR READ8`／`READ_BUF` 寫它 |
| `MAIN_CONST_POOL` | ✔ | ✘ |
| `PERI_RESULT_SLOT`、`PERI_RESULT_META`、`ALU_OUT`、`ALU_STATUS` | ✔（SLOT 會消耗） | ✘ |
| `ALU_OPA`、`ALU_OPB` | ✘ | ✔（固定 offset 0、長度 2） |

檢查：來源可讀、目的可寫；`MAIN_CONST_POOL` **只能**搬到 `MAIN_SCRATCHPAD`（不可直接進 ALU，也不可進 csrbuf）；
同一區域內的搬移只有**範圍不重疊**時合法（v31 5.2：不提供 memmove 語意；profile `same_region_mov = false` 可連不重疊的也禁止）；
ALU 運算元長度必須為 2；兩側長度相同；範圍不得超出區域。

**從 v24 改過來**：v24 把常數經 `MAIN_CSR_DATA_BUF` 中轉；v31 改為
`MOV CONST(5) -> MAIN_SCRATCHPAD[20..21]` 再 `MOV MAIN_SCRATCHPAD[20..21] -> ALU_OPB`。想寫的位元組放在 scratchpad，
`EXEC_CSR WRITE_SP` 直接從那裡寫 CSR。

### ALU（規格 6）
```
ALU ADD|SUB|MUL|INC|DEC [signed|unsigned]
ALU CMP.EQ|NE|LT|GE     [signed|unsigned]      （也接受 ALU EQ unsigned）
ALU AND|OR|XOR|NOT
ALU SHL|SHR 數量(1..8)  [signed|unsigned]
```
預設 unsigned。`EQ` / `NE` 不可 signed。ALU 只吃 `ALU_OPA`／`ALU_OPB`，結果在 `ALU_OUT`／`ALU_STATUS`，要用請再 `MOV`。

### EXEC_CSR（規格 4）
```
EXEC_CSR WRITE8 csr, imm            ; CSR = imm
EXEC_CSR SET8   csr, mask           ; CSR |= mask
EXEC_CSR CLR8   csr, mask           ; CSR &= ~mask
EXEC_CSR READ8  csr -> MAIN_CSR_DATA_BUF[n]
EXEC_CSR WRITE_SP  csr, len=N, ptr=P     ; CSR[addr+i] = MAIN_SCRATCHPAD[ptr+i]       （v31；v24 的 WRITE_BUF 已不存在）
EXEC_CSR READ_BUF  csr, len=N, ptr=P     ; MAIN_CSR_DATA_BUF[ptr+i] = CSR[addr+i]
```
`WRITE_SP` 的 `ptr` 指向 `MAIN_SCRATCHPAD`；`READ_BUF`／`READ8` 的目的是 `MAIN_CSR_DATA_BUF`。`ptr + len ≤ 32`。寫 `WRITE_BUF` 會得到
一則說明改名與新資料來源的錯誤（E204）。
`csr` 為 `.csr` 符號或 0..0xFFF 的運算式。每個相異的 (操作, 位址) 或 (操作, 頁, 長度, ptr) 組合占用一個描述子表項目（相同的會共用）。

### 分支（規格 7）
```
BR.COND 條件, 標籤 [, safe]        （JMP 標籤 [, safe] = BR.COND ALWAYS, 標籤）
BR.ERET                            ; 從事件 handler 返回（規格 16.5）
CASCADE_BEGIN 選擇值k, 區塊0標籤
CASCADE_END   區塊索引i, 離開標籤
```
**`safe`（`event_safe`，v31 7.2、16.3）**：只有帶 `safe` 的**一般**分支，成功退休時才可能派送硬體事件；不寫就是 0（連 `ALWAYS` 也不會自動變 safe）。
也可寫 `event_safe=1`。`event_safe` 是描述子的一部分，所以只差在 safe 的兩條分支用兩個描述子。
`CASCADE_BEGIN`／`CASCADE_END`／`BR.ERET` 不可帶 `safe`（E208）；Cascade 區域內不可有一般分支、`BR.ERET` 或 safe（E304／E208）。
`BR.ERET` 沒有運算元，描述子欄位固定為 0（Main：`br_type=3`；Peri：`br_type=8`），永遠不會往下執行。
Peri 的分支同樣寫法：`BR.IF_READY 標籤, safe`、`JMP 標籤, safe`、`BR.ERET`。事件的機制與 handler 規則見下方「硬體事件」。
條件：`ALWAYS PERI_RESULT_VALID PERI_RESULT_OVERRUN PERI_ERROR PERI_NO_ERROR ALU_CMP_TRUE ALU_OUT_OV16 MAIN_TIMER_DONE
FRAME_END_EVT VBLANK_OPEN HBLANK_OPEN PGL_COMMIT_ELIGIBLE AE_COMMIT_ELIGIBLE LINE_COMMIT_ELIGIBLE`（`PROFILE_COND0` 需 profile 啟用）。
也接受規格書的具名寫法 `BR.COND cond_sel=NAME, target=LABEL`。

Cascade：BEGIN 之後的各區塊必須連續，每個區塊以 `CASCADE_END i, exit` 結尾，i 依序為 0、1、2…，且 BEGIN 的選擇值不得超過最後一個區塊索引。

### 硬體事件：`.vector`、handler、`BR.ERET`（規格 v31 §16）

硬體隨時可以記下一個「pending 事件」，但**只有成功退休的一般 BR 且 `event_safe=1`** 才會派送（並滿足：沒有 handler 在執行、Main 不在 Cascade 內、該事件有 vector）。沒有 context bank、不保存 ALU／scratchpad／RX／Poll／Timer／Error。

```
.main
.vector 0, on_frame          ; 事件 0 的 handler 入口（目前 domain；id 0..event_count-1）
loop: ...
      BR.COND ALU_CMP_TRUE, loop, safe     ; 可以在這裡被打斷
      JMP loop
on_frame:                    ; handler 放在主流程的無條件 JMP 之後
      ...
      BR.ERET                ; 回到被打斷流程「已解析的下一個 PC」
```

- `.vector ID, label`：label 必須是本 domain 的流程標籤（不可在 `.proc` 內）、落在指令邊界、不可在 Cascade 本體內。ID 範圍由 profile `event_count` 決定（預設 8）；同一 domain 的同一 ID 只能宣告一次。向量的**來源、優先序、載入方式由目標 profile 決定**，組譯器只記錄你宣告的向量（`events.json`）。
- 派送時把 BR 解析後的下一個 PC（taken 為目標、not-taken 為 pc+1）存成完整寬度的 resume PC；`BR.ERET` 跳回該 PC，不使用堆疊，且 ERET 本身不會再派送新事件。同時有多個 pending 時，模擬器先派送編號最小的。
- 驗證（E315）：vector 標籤未定義／不在指令邊界／在 Cascade 內／ID 超出範圍；**從程式起點不經事件就走得到 `BR.ERET`**；handler 走不到 `BR.ERET`，或有任何路徑永遠到不了 ERET（迴圈）。沒有被任何 vector 或主流程碰到的 `BR.ERET` 只是不可達程式碼（W402），不是錯誤，這樣沒有向量資訊的反組譯結果仍可重新組譯。
- **W512**：組譯器不替你決定哪個分支「安全」，`safe` 永遠由你寫。但如果某個 `safe` 分支之後的程式碼還要讀的狀態（ALU 運算元／結果／旗標、scratchpad／csrbuf／RX 位元組、Peri ready／error 狀態），handler 可能覆寫，就警告並指出狀態與 handler。無法證明 handler 有自行存取還原；修法：拿掉該分支的 `safe`、讓 handler 改用別的位元組，或自行存取還原後確認。限制：Timer／Poll 由 CSR 動作控制，其副作用不在靜態分析範圍；以 `.vector` 以外方式載入的向量也看不到。
- **W513**：該 domain 沒有 `safe` 分支，或前景有一個迴圈上沒有 `safe` 分支 → 事件回應時間**沒有上界**，不可宣稱有界。
- 事件時間（`Result::eventTiming`、`.map`、`events.json`）分開列出：pending→下一個 safe 分支（最壞）、派送、handler（vector→ERET 最長路徑）、ERET＋重取指；有界時給總和。數值是 profile 的規劃延遲（`event_dispatch`、`event_eret`），**不是簽核值**；偵測到 pending 之前的延遲與 SRAM／Frontend 競爭不在內。

## 5. Peri 指令

```
EXEC_CSR WRITE8|SET8|CLR8 csr, imm        ; 只有 one-byte；READ8 與 multi-byte 在 baseline 為非法
BR.ALWAYS|IF_READY|IF_NOT_READY|IF_TIMEOUT|IF_ERROR|IF_NO_ERROR|IF_TIMER_DONE|IF_TIMER_NOT_DONE 標籤     （JMP 標籤）
EXEC_PUB service=SID, src=N, len=L        ; PERI_RX_BUF[src..src+L-1] -> PERI_RESULT_SLOT
EXEC_XFER WRITE dev=D, addr=A, addrlen=K, data=V         ; one-byte 寫入
EXEC_XFER READ  dev=D, addr=A, addrlen=K, ptr=P          ; one-byte 讀取（讀到 PERI_RX_BUF[P]）
EXEC_XFER READ  dev=D, len=L, ptr=P, ext=[b, b]          ; multi-byte 讀取，ext 為內嵌的外部位址位元組（0..2 個）
```
`dev` 必須是已用 `.devmap` 定義的項目。one-byte 的 `addr` 最多 10 位元（超過請改用 multi-byte 的 `ext`）。
multi-byte WRITE 在 baseline 非法（沒有 PERI_TX_BUF）。**Peri 沒有 MOV、ALU、scratchpad**，因此也沒有 `CASCADE`。

## 6. 程序與 CALL／RET

規格書沒有 CALL／RET 指令，也沒有堆疊，指令碼的 4 個 opcode 已全部用完。組譯器提供 8051 風格的語法，並以兩種方式實作；
**原始碼寫法相同**，用 `--call-model` 切換。

```
.proc leaf
    EXEC_CSR WRITE8 AE_CTRL, 0x11
    RET                      ; 可有多個；放在最後一行時省略跳躍
.endp
start:  CALL leaf
```
規則：程序以 `RET` 或無條件分支結尾（否則 E305）；不可遞迴（呼叫圖必須無環，深度與 WCET 才有上限）；
程序內的標籤是區域標籤；不可用分支跳進程序（要用 `CALL`）；Main 與 Peri 各自獨立，不能互相呼叫；`RETI` 不支援（沒有中斷）。

| | `inline`（預設） | `softret` |
|---|---|---|
| 作法 | 每個 `CALL` 處把程序本體展開 | 本體只產生一份。呼叫端把回傳編號存入 scratchpad，`RET` 比較編號後跳回 |
| 適用 | Main、Peri | **僅 Main**（Peri 沒有 MOV／ALU，會自動退回 inline 並給 W403） |
| 程式大小 | 隨呼叫次數增加 | 本體一份，但每個呼叫點與每個 `RET` 多幾條指令 |
| 週期 | 無額外成本 | 見下 |
| 會破壞的狀態 | 無 | `ALU_OPA`、`ALU_OPB`、`ALU_STATUS`，以及保留的 scratchpad 位元組 |
| 保留資源 | 無 | `MAIN_SCRATCHPAD[ret_slot_base ..]`（每個有 2 個以上呼叫點的程序 2 位元組）與 `MAIN_SCRATCHPAD[ret_stage_base..+1]`（比較返回編號時暫放常數）；使用者程式碰到會報 E306。v31 起 `MAIN_CSR_DATA_BUF` 不再被佔用 |

softret 的展開（呼叫點有 2 個以上時）：
```
呼叫端:  MOV CONST(k, 2) -> MAIN_SCRATCHPAD[slot..+1]   ; 回傳編號 k：一條 MOV（v31 常數池可直接搬到 scratchpad；v24 要兩條）
         BR.COND ALWAYS, proc
RET:     MOV MAIN_SCRATCHPAD[slot..+1] -> ALU_OPA
         （對 k = 1 .. n-1）MOV CONST(k, 2) -> MAIN_SCRATCHPAD[stage..+1] ; MOV MAIN_SCRATCHPAD[stage..+1] -> ALU_OPB ; ALU CMP.EQ ; BR.COND ALU_CMP_TRUE, 返回點k
         BR.COND ALWAYS, 返回點n
```
只有一個呼叫點的程序不需要回傳編號，`RET` 直接是一條 `BR`。

範例（`examples/call_models.asm`）：程式碼 11 位元組對 28 位元組；WCET 15 對 98 個週期（規格 14.3 的規劃延遲）。
**呼叫 `RET` 的代價隨呼叫點數量成長**，需要 WCET 嚴格時建議用 `inline`。

實體限制：分支目標只有 8 位元（規格 7.2 的 `target_pc_byte`），所以每個 domain 的程式最多 256 位元組；
描述子表最多 64（MOV/BR/ALU/PUB）或 32（CSR/XFER）項，相同描述子會共用。內嵌展開與 softret 都會消耗這些資源，
組譯器會在 `.map` 顯示用量（如 `10/64`）。

## 7. 驗證與錯誤碼

| 碼 | 意義 |
|---|---|
| E101 | 未知的指令或指示字 |
| E102 | 運算元格式不對 |
| E103 | 運算式錯誤／符號未定義 |
| E104 | 符號或標籤重複定義 |
| E105 | 指示字誤用（`.main`/`.peri` 之前就有指令、`.org` 等） |
| E106 | `.include` 失敗 |
| E201 | 數值或範圍超出 |
| E202 | MOV 區域規則（v31：常數池只能進 scratchpad、csrbuf 不可當目的、同區域只准不重疊） |
| E203 | ALU 規則 |
| E204 | EXEC_CSR 規則（含 `WRITE_BUF` 已改名 `WRITE_SP`） |
| E205 | 分支條件不合法 |
| E206 | EXEC_XFER / EXEC_PUB 規則 |
| E207 | 描述子表已滿（超過 profile 深度） |
| E208 | `event_safe` 用在不允許的地方（Cascade／ERET／Cascade 區域內）或值不是 0／1 |
| E301 | 分支目標未定義或不是 opcode 位元組 |
| E302 | 程式太大（上限 = `min(max_code_bytes, 2^PC 寬度)`，訊息會點名 profile 鍵） |
| E303 | 會從程式尾端或進入程序本體「掉出去」 |
| E304 | Cascade 結構錯誤 |
| E305 | 程序／呼叫圖錯誤（遞迴、未定義、掉出程序尾端、分支進入程序） |
| E306 | softret 保留位置不足或與使用者程式衝突 |
| E307 | 指令不屬於該 domain |
| E308 | 常數池滿或範圍超出 |
| E309 | DeviceMap 項目未定義 |
| E310 | 普通 BR 的目標超過 8 位元欄位（0..255）：不會被截斷，直接報錯並說明 |
| E311 | 實體 SRAM 配置錯誤：視窗超出 SRAM 深度，或 Main／Peri 保留視窗重疊 |
| E312 | 映像大小超過 profile 的視窗容量 |
| E313 | DeviceMap 項目欄位重疊或放不進 `device_map_entry_bytes` |
| E314 | 某個 ImageLayout 偏移放不進旁路欄位寬度（`layout_offset_bits`） |
| E315 | 事件向量／`BR.ERET` 錯誤（上面「硬體事件」） |
| W401 | 程序從未被呼叫（不產生程式碼） |
| W402 | 不可達的指令 |
| W403 | Peri 不能用 softret，已改用 inline |
| W404 | DeviceMap 有空洞 |
| W405 | 同一個 Cascade 的離開標籤不一致 |
| W410 | LOGIC 運算指定了 signed／unsigned |
| W501 | ALU 運算元（`ALU_OPA`／`ALU_OPB`）沒有在所有路徑上被載入（第 11 節） |
| W502 | 讀取 scratchpad／`MAIN_CSR_DATA_BUF` 的位元組（含 `EXEC_CSR WRITE_SP` 寫出 scratchpad），之前沒有在所有路徑上寫入；csrbuf 只由 `READ8`／`READ_BUF` 定義 |
| W503 | 讀 `ALU_OUT`／`ALU_STATUS`，但沒有任何會產生它的 ALU 運算在所有路徑上執行過 |
| W504 | ALU 結果在被 MOV 走之前就被下一個 ALU 覆寫 |
| W505 | 沒有先測試 `PERI_RESULT_VALID` 就消耗結果槽，或同一次測試後消耗兩次 |
| W506 | `BR.COND ALU_CMP_TRUE`／`ALU_OUT_OV16` 讀旗標，但之前沒有 CMP／ALU 運算在所有路徑上執行過 |
| W507 | 某 service 的處理流程消耗結果槽的長度，與 Peri 發佈該 service 的長度不符 |
| W508 | Peri `EXEC_PUB` 讀取尚未被 `EXEC_XFER` 讀入的 `PERI_RX_BUF` 位元組 |
| W510 | Peri 發佈了某 service，但 Main 從不依該 service_id 分派 |
| W511 | 同一個 service_id 以不同長度發佈 |
| W512 | `safe` 分支之後仍要讀的狀態，可能被事件 handler 覆寫 |
| W513 | 事件回應時間無上界（沒有 safe 分支或前景迴圈缺 safe） |
| W514 | 線上高位元組在前的 2 位元組感測器值未經交換就進 ALU（第 12 節） |
| W515 | 在計時器可能沒啟動的路徑上測試計時器旗標（第 12 節） |
| W516 | 測試 Poll 結果前，沒有「arm → 成功 READ 進 `PERI_RX_BUF[0]`」（第 12 節） |
| W517 | 發佈／消耗長度與服務契約（`.service …, len=N`）不符（第 12 節） |

## 8. 映像檔佈局（規格 v31）

每個 domain 一份**邏輯映像**（`PREFIX.main.bin`／`PREFIX.peri.bin`），不含 ImageLayout：

```
Main:  程式碼 | MainMoveDesc(4B) | MainBranchDesc(2B) | MainCsrOneByteDesc(2B) | MainCsrMultiByteDesc(2B) | MainAluDesc(1B) | MAIN_CONST_POOL
Peri:  程式碼 | PeriXferOneByteDesc | PeriXferMultiByteDesc | PeriPublishDesc | PeriBranchDesc | PeriCsrOneByteDesc | DeviceMap
```

- 各表起點依 profile 的 `align_*`（預設 = 項目寬度），間隙補 `boot_pad_byte`。
- **ImageLayout 是旁路 latch，不在映像位元組裡**。六個偏移（邏輯**位元組**偏移）由 `PREFIX.main.layout.json`／`PREFIX.peri.layout.json` 提供；`.bin` 沒有它就無法解讀，請放在一起。`asmdis` 會自動讀同名 `.layout.json`，或用 `--layout FILE` 指定。
- 兩份映像共用**一顆** 32 位元 SRAM：`物理 word = IMAGE_BASE_WORD + (邏輯位元組 >> 2)`，lane = `位元組 & 3`（位元組 0 在 bits [7:0]）。映像補滿整個 word，兩份映像不共用 word。`--artifacts` 會寫 `PREFIX.sram.hex`（每行 `位址 資料`）。
- `asmc --artifacts` 另產：`boot.json`（載入計畫）、`memory.json`（佔用範圍／保留視窗不重疊的證明）、`sections.json`、`descriptors.json`、`services.json`、`events.json`、`validation.json`、`manifest.json`（FNV-1a64，只是完整性檢查，不是安全雜湊）。
- 外部 ROM 容器格式屬於開機／產品整合，不在 ISA 內，組譯器不自創標頭。

## 9. 假設與待確認

規格書沒有明講、組譯器採用假設的項目（都集中在 `core/src/isa.h` 與 profile，改一處即可）：

| # | 假設 | 規格書狀況 | 如何改 |
|---|---|---|---|
| 1 | ALU 子運算碼：arith ADD=0 SUB=1 MUL=2 INC=3 DEC=4；cmp EQ=0 NE=1 LT=2 GE=3；logic AND=0 OR=1 XOR=2 NOT=3；shift SHL=0 SHR=1 | 6.3 只列名稱 | `isa.h` |
| 2 | 多位元組描述子在映像檔中為小端 | 未規定 | profile `big_endian` |
| 3 | ImageLayout 旁路欄位寬 16 位元，順序同 3.1 列舉 | v31 只說是 sideband，寬度由 profile | profile `layout_offset_bits` |
| 4 | DeviceMap 項目 1 位元組：bit7 = bus_type，bit6:0 = I2C 位址 | 只給位元寬度 | profile `device_map_entry_bytes`、`_bus_shift`、`_addr_shift` |
| 5 | 描述子表只輸出用到的項目（`--pad-tables` 可補滿） | 3.1 允許兩種 | `--pad-tables` |
| 6 | 延遲數值採規格 14.3／14.4 的**規劃數值**（MOV = 1+len、一位元組 CSR = 1+2、BR = 1…），CSR_ACCESS_CYC = 2、CSR_BYTE_CYC = 2 是我取的 | 7.4.5 要求實作提供最終表 | profile `[latency]` 鍵 |
| 7 | 普通 BR 的目標欄位 8 位元（0..255）；程式大小上限由 PC 寬度與容量決定，兩者是不同的限制 | v31 7.2 | profile `main_pc_width`、`peri_pc_width`、`max_code_bytes` |
| 10 | 實體 SRAM：深度 1024 word、Main 基底 0、Peri 基底 512、各 2048 位元組 | v31 只給玩具例 | profile `physical_sram_depth_words`、`*_image_base_word`、`*_capacity_bytes` |
| 11 | ALU 子運算碼數值可由 profile 重映射（兩版規格都沒定義數值） | 未規定 | profile `alu_*` |
| 8 | CSR 位址、service_id、DeviceMap 由使用者的 `.csr`/`.service`/`.devmap` 提供 | 規格只給名稱 | 你的 `.inc` |
| 9 | （已解決）v31 5.1 明定：常數池只能進 scratchpad，csrbuf 只是 MOV 來源；同區域只禁止重疊（是否容許不重疊的自拷貝仍需 RTL 測試確認，profile `same_region_mov`） | v31 5.1、5.2 | `lower.cpp` `movChecks`、profile |

## 11. 資料流檢查（W5xx）

規格 3.3.9、5.2、6.8、10.4、12.8 要求 validator 確認的事，在組譯時做。選項：`--dataflow off|warn|error`（預設 warn；
error 時代碼變成 E5xx 並使建置失敗）、`--no-warn W502`（可重複）、`--werror`。

**怎麼判斷**：「必然」分析 —— 一個事實只有在**每一條**到達該點的路徑上都成立才算成立。所以「某分支裡才載入 ALU 運算元」會被指出。
迴圈沒有問題（第一圈也必須先初始化）。

**程序呼叫**：分析永遠對「CALL 已展開」的程式做，與你選的呼叫模型無關（軟體返回是跳到每個呼叫點，會憑空產生路徑）。
選 `softret` 時，會在每個有 2 個以上呼叫點的程序返回處套用它造成的破壞：`ALU_OPA`／`ALU_OPB` 與比較旗標視為未載入。
因此「CMP 之後 CALL 再 `BR.COND ALU_CMP_TRUE`」在 inline 沒事，在 softret 會被指出（這正是兩種模型的真實差異）。

**規格要求「或實作宣告 reset 值可用」的部分**：profile 的 `scratchpad_reset_defined`、`csrbuf_reset_defined`、`peri_rx_reset_defined`，
或用 `.assume` 宣告某一段。

**跨 domain 的檢查**：從 Peri 的 `EXEC_PUB` 蒐集每個 service 的長度；Main 端用「`PERI_RESULT_META` 的 service_id 與常數比較、
`ALU_CMP_TRUE` 成立時」辨認處理流程，檢查該流程裡 MOV 結果槽的長度（W507），以及每個被發佈的 service 都有人依 id 分派（W510）。
辨認只認這個固定的寫法（META[0..1] → `ALU_OPA`，常數 → `ALU_OPB`，`CMP.EQ`）；別的寫法則不做長度與分派檢查，不會誤報。

**限制**：沒有做指標別名或值域分析 —— 暫存器索引都是編譯期常數所以不需要；分支條件只對 `PERI_RESULT_VALID` 與 service 比較做路徑細化，
其他條件兩條邊視為都可能。這是警告而不是證明：有「第一次才會走的初始化」之類的寫法時，用 `.assume` 或 `--no-warn` 表明意圖。

## 10. 目前尚未做的事

- （模擬器已完成，見 `docs/SIM.md`。）
- 以 timing 預測式（`*_COMMIT_ELIGIBLE`）自動產生 `CSR_WCET_*` 的值（目前只提供 `--wcet` 查詢）。

## 12. 服務契約與 Peri 流程檢查（規格 v31，階段 5）

這些檢查需要組譯器知道硬體沒有告訴它的事（CSR 位址由 profile 決定、感測器的線上位元組序由裝置決定），所以**由你在原始碼裡宣告**；沒宣告就不猜、不警告。診斷碼對應 v31 的 `E_*` 名稱，`asmc --explain 碼` 與 `asmc --list-codes` 可查，`validation.json` 的每筆診斷帶 `spec_name` 與 `fix`。

```
.service GYRO = 1, len=6, big     ; service_id 1；payload 6 位元組；線上高位元組先（big）或低位元組先（little）
.csrbit PERI_CTRL[3] = timer_start    ; 這個 CSR 位元寫 1 就啟動計時器（角色：timer_start / timer_clear / poll_arm / poll_clear）
.csrbit POLL_CTRL[1] = poll_arm
.csrbit POLL_CTRL[0] = poll_clear
```

- **W514 感測器位元組序**（`E_SENSOR_BYTE_ORDER`）：宣告 `big` 的服務，其 payload 以 2 位元組為一組（偶數偏移起）；若一組的兩個位元組**照原順序**直接 `MOV` 進 `ALU_OPA`／`ALU_OPB`，ALU 會看到位元組互換的值（ALU 運算元是小端）。修法：用兩個 1 位元組的 MOV 經 scratchpad 交換（低位元組到 `T`、高位元組到 `T+1`），再把 `T..T+1` 搬進 ALU。需要 handler 的 service id 已由比較判定。
- **W515 計時器流程**（`E_TIMER_START_FLOW`）：有宣告 `timer_start` 時，`BR.IF_TIMER_DONE`／`IF_TIMER_NOT_DONE` 之前每條路徑都必須寫過啟動位元（寫 1；`WRITE8` 或 `SET8`），且之後沒有 `timer_clear`。`timer_done = 0` 不代表計時器在跑。計時器應在等待迴圈**外**啟動一次。
- **W516 Poll**（`E_POLL_ARM_PRODUCER`）：有宣告 `poll_arm` 時，`BR.IF_READY`／`IF_NOT_READY` 之前每條路徑都必須「arm → 一次成功的 `EXEC_XFER READ` 且寫入 `PERI_RX_BUF[0]`」，中間不可 `poll_clear`。
- **W517 服務契約**：`EXEC_PUB` 的長度、Main handler 消耗的長度都必須等於 `len=N`。
- **CSR 讀回**（`E_CSR_READBACK_SOURCE`）：`MAIN_CSR_DATA_BUF` 只能由 `READ8`／`READ_BUF` 寫（E202），讀未寫的位元組是 W502。若 profile 有選用的 `csr_buf_valid`（`csr_buf_valid = true`），模擬器會逐位元組記錄讀回是否完成，從沒完成的位元組 `MOV` 出來會 fault（`fault_csrbuf_invalid`）。
