# 模擬器實作與規格對照

建立日期：2026-11-20 ・ 對照的程式：分支 `blocks-toolbox-single-pieces`（`core/src/sim/`）

## 0. 這份文件是什麼

1. **第 1 節**記下目前還沒有答案的問題（硬體到底怎麼做）。
2. **第 2 節**逐項列出模擬器**現在實際怎麼做**，每項都是直接讀原始碼整理的，並標明程式位置，方便和規格文件一條一條比對。
3. **第 3 節**是所有硬體參數與預設值（由 `asmsim --list-params` 產生）。
4. **第 4 節**是確認模擬器與硬體一致的做法。

### 請注意

* 「規格」欄寫的是**程式註解引用的規格章節**（v31 規格書的章號）。這份文件**沒有**逐條重新對照規格原文——那正是它要請您做的事。發現不符請直接在該列註記。
* 專案的自動測試證明的是「組譯器、模擬器、反組譯器彼此一致」，**不是**「與硬體一致」。目前沒有任何項目對照過 RTL 或實體硬體。

### 圖例（「依據」欄）

| 標記 | 意思 |
|---|---|
| 規格 | 程式依規格文字實作（引用的章節見該列） |
| 使用者確認 | 您說明過硬體是這樣 |
| 假設 | 規格沒有寫，模擬器自己選了一種做法 |
| 規劃值 | 數值是暫定的，做成參數，等 RTL 提供 |
| 未實作 | 模擬器沒有做 |

---

## 1. 待確認的問題

### 1.1 這次討論中提出的

| # | 問題 | 模擬器目前的做法 | 為什麼重要 | 答覆 |
|---|---|---|---|---|
| Q1 | **Timer 是一組還是兩組？** Main 的 `MAIN_TIMER_DONE` 與 Peri 的 `IF_TIMER_DONE` 是不是同一顆計時器的同一個旗標？ | 兩組，各有自己的週期、啟動、完成旗標；只共用計數時脈（見 2.12） | 如果硬體只有一顆，一邊啟動會蓋掉另一邊正在等的計時（範例 14 的寫法會出錯），而模擬器不會顯示這個問題 | |
| Q1a | 若只有一顆：誰可以啟動？ | — | | |
| Q1b | 若只有一顆：完成時兩邊都看得到嗎？ | — | | |
| Q1c | 若只有一顆：兩邊同時啟動怎麼辦（後者覆蓋／前者優先／錯誤）？ | — | | |
| Q2 | **Main 與 Peri 的 PC 是分開的兩個嗎？** | 是，各一個，各自從 0 起算、各自前進（見 2.1） | 規格文字是「各 domain 自己的 byte PC」；請確認 RTL 也是 | |
| Q3 | **事件（event）與 Timer 有沒有關聯？** Timer 到時會不會觸發事件？ | 沒有關聯。Timer 只設完成旗標，程式用分支輪詢；事件只能由外部注入（見 2.16） | 規格說「不要發明 timer interrupt」；請確認 | |
| Q4 | **移位方向位元 `shift_dir`：0 是 SHL 還是 SHR？** | 0＝SHL、1＝SHR（寫死在 `core/src/isa.h`，沒有 profile 鍵） | 兩份規格說法不一致（交接文件說已明確，規格 6.3／6.7 抽出的文字只見名稱）；錯了左移右移會對調 | |
| Q5 | **硬體怎麼取指？** 每道指令都從 I2C EEPROM 讀，還是開機載入 SRAM 後從 SRAM 讀？ | 兩種都做了，預設是每道指令都讀 EEPROM（見 2.2） | 取指時間差上千倍；範例 14 在 400 kHz 下提交無法保證落在消隱視窗內 | |
| Q6 | **兩個 sequencer 搶共用資源時，硬體的規則與時間？** | 先到先得，同週期 Main 先（I2C 匯流排、CSR 仲裁器已由您確認；其餘是假設，見 2.15） | 等待時間目前不計入 WCET，所以就緒等級達不到 L2 | |
| Q7 | **怎麼確認模擬器與硬體一致？** | 尚未確認任何一項（見第 4 節的做法） | | |

### 1.2 先前已列出、仍未答覆的（完整內容在 `docs/PROJECT_STATUS.md` 第 7 節）

| 編號 | 主題 | 狀態 |
|---|---|---|
| 7.1 A1–A4 | FPGA 量測資料的來源、格式、時間基準（阻塞 S8 校準） | 未答覆 |
| 7.2 B1、B2 | CSR 仲裁器存在競爭；先到先得、同時 Main 先 | 已答覆（2026-11-05） |
| 7.2 B3–B9 | 仲裁器占用多久、交接延遲、`EXEC_XFER` 是否經過仲裁器、Data Xfer 的使用者、結果槽 valid 的時點、旗標到 BR 的延遲、Main 的觸發語意 | 未答覆 |
| 7.3 C1–C3 | 共用計時器（即上表 Q1a–Q1c） | 未答覆 |
| 7.4 D1–D6 | 編碼假設：ALU 子運算碼數值、描述子位元組序、DeviceMap 排列、ALU_STATUS 旗標位置、negative 取哪一位、`shift_dir` | 未答覆 |
| 7.5 E1–E5 | Frontend 各狀態週期、SRAM 延遲、開機載入、PC 何時前進、計時器的預除器／計數／相位 | 未答覆 |
| 7.6 F1 | 範例 14 的硬體方向（1 MHz／獨立匯流排／SRAM 取指） | 未答覆 |
| — | `docs/HW_FAULT_DETECTION.md`：55 列「這個錯誤硬體在哪一級偵測」 | 0 筆答覆（2026-11-20） |

---

## 2. 模擬器目前的實作（逐項）

### 2.1 整體結構與時間

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.1.1 | 兩個 sequencer：Main 與 Peri，各有一份程式、一個 PC、各自的暫存區 | 2 | 規格 | `impl.h`（`MainState`、`PeriState`） |
| 2.1.2 | PC 是自己那份程式碼的位元組位置，重置後為 0 | — | 規格 | `impl.cpp` `resetState` |
| 2.1.3 | 時間以 ISA 週期為單位的整數計數；每個 sequencer 記著「下一道指令何時可以開始」，排程器永遠先做時間最早的那件事 | — | 假設 | `run.cpp` `nextTime`、`runLoop` |
| 2.1.4 | 同一週期內的順序：計時事件（Timer 到時、傳輸完成、畫面結束…）先，然後 Main，然後 Peri | — | 假設（`sim.tie_order` 可對調 Main／Peri） | `run.cpp` `nextTime` |
| 2.1.5 | 例外：同一週期兩邊都要 I2C 匯流排時，一律 Main 先，不受 `sim.tie_order` 影響 | — | 使用者確認（2026-11-06） | `run.cpp` `nextTime` |
| 2.1.6 | **指令的效果在它開始的那個週期就生效**（暫存區、CSR 立刻改變），延遲只決定下一道指令何時開始。例外見 2.4.10（取用結果槽）、2.7.9（I2C 傳輸）、2.8.5（發佈） | — | 假設（規格未規定效果時點） | `main_core.cpp`、`peri_core.cpp` |
| 2.1.7 | 重置到第一道指令的時間：`reset.boot_cycles`（預設 0） | — | 規劃值 | `impl.cpp` `resetState` |
| 2.1.8 | 沒有該 domain 的程式時，那個 sequencer 視為停止 | — | 假設 | `impl.cpp` `resetState` |
| 2.1.9 | 「無條件跳到自己」（`BR ALWAYS` 目標＝自己，且沒有 `safe`）視為程式結束，該 sequencer 停止 | — | 假設（這是模擬器的慣例，硬體會一直空轉） | `main_core.cpp` `opMainBr`、`peri_core.cpp` `opPeriBr` |

### 2.2 指令格式與取指

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.2.1 | 每道指令第一個位元組：bit 7:6＝opcode，bit 5:0＝arg | 2 | 規格 | `main_core.cpp` `stepMain` |
| 2.2.2 | Main 的 opcode：0 EXEC_CSR、1 BR、2 MOV、3 ALU | 2 | 規格 | `impl.cpp` `registerOps` |
| 2.2.3 | Peri 的 opcode：0 EXEC_CSR、1 BR、2 EXEC_PUB、3 EXEC_XFER | 2 | 規格 | 同上 |
| 2.2.4 | arg 是描述子表的索引；`EXEC_CSR` 與 `EXEC_XFER` 的 bit 5 選 OneByte／MultiByte 表，bit 4:0 是索引 | 3、4、8 | 規格 | 各 `op*` 函式 |
| 2.2.5 | 取指來源由參數 `fetch.source` 決定，預設 **`eeprom`**：每執行一道指令前，解碼器先經 I2C 從 EEPROM 讀出它的程式碼位元組、描述子、（`EXEC_XFER` 的）DeviceMap 項目、（MOV 的）常數 | — | 使用者確認「程式存在 I2C EEPROM（0x50），每道指令都去讀」（2026-11-05） | `decode.cpp` `beginFetch`、`fetchCost` |
| 2.2.6 | EEPROM 分六個區域（Main 程式碼／描述子／常數，Peri 程式碼／描述子／DeviceMap），起始位址由六個暫存器決定 | — | 使用者確認；位址數值是規劃值（profile `eeprom_*`） | `eeprom.cpp`、`impl.cpp` `syncFetch` |
| 2.2.7 | 一次讀取的時間：`⌈9 × (2 + 位址位元組數 + n) × cpu_hz ÷ i2c_hz⌉ + 階段數 × (i2c.ack_delay + i2c.stretch) + fetch.transaction_overhead` | 14.4（I2C 位元組階段） | 公式依協定；每區域一次讀完、額外週期等是假設 | `decode.cpp` `fetchCost` |
| 2.2.8 | `fetch.read_mode`：`sequential`（每個區域一次讀取，預設）、`per_byte`（每個位元組一次）、`instant`（不花時間） | — | 假設 | 同上 |
| 2.2.9 | `fetch.source = frontend`（選用）：開機先把 ROM 載入一顆共用的 32-bit 程式 SRAM，之後每道指令走七個狀態 CMD_HOLD → OP_GET → DESC_GET → DEC → FIRE → EXEC（錯誤時 CMD_FAULT） | v31 3.1、Frontend-001 | 狀態順序依規格；各狀態週期數、SRAM 延遲、兩個 Frontend 共用讀取埠的方式是規劃值 | `frontend.cpp` |
| 2.2.10 | Frontend：SRAM 以兩倍 ISA 時脈運作；有一個字的 opcode 緩衝（同一個字裡的程式碼位元組不重讀） | v31 3.1 | 規格／規劃值 | `frontend.cpp` `planFrontend` |
| 2.2.11 | `fetch.source = sram`：直接用組譯結果，取指不花時間 | — | 這是早期的簡化模型，不是硬體 | — |
| 2.2.12 | PC 超出程式碼範圍：fault `fault_pc_out_of_code` | — | 假設 | `impl.cpp` `fetchOk` |

### 2.3 Main `EXEC_CSR`（規格第 4 章）

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.3.1 | 指令長度 2 個位元組；第二個位元組是立即值 | 4 | 規格 | `main_core.cpp` `opMainCsr` |
| 2.3.2 | OneByte 描述子的運算：0 WRITE8（寫入立即值）、1 SET8（OR）、2 CLR8（AND NOT）、3 READ8 | 4.2 | 規格 | 同上、`impl.cpp` `csrWrite` |
| 2.3.3 | READ8：立即值是目的指標（bit 7:5 必須為 0），讀到的 1 個位元組放進 `MAIN_CSR_DATA_BUF[指標]` | 4.2 | 規格 | 同上 |
| 2.3.4 | MultiByte：位址＝`(page << 8) \| 立即值`；`WRITE_SP`（op 0）把 `MAIN_SCRATCHPAD[ptr..]` 的 len 個位元組依序寫到連續位址；`READ_BUF`（op 3）把連續位址讀進 `MAIN_CSR_DATA_BUF[ptr..]` | v31 4.3 | 規格 | 同上 |
| 2.3.5 | MultiByte 的 op 1、2（緩衝區 RMW）：fault `fault_desc_reserved` | 4.3 | 規格（baseline 保留） | 同上 |
| 2.3.6 | `ptr + len > 32`：fault `fault_range` | 4.3 | 規格 | 同上 |
| 2.3.7 | 描述子保留位元不為 0：fault `fault_desc_reserved` | — | 規格 | 同上 |
| 2.3.8 | 讀回有效位元（profile `csr_buf_valid`，預設關）：READ 開始時該範圍變無效，讀完才有效 | v31 4.3（選用） | 規劃值（是否實作由 RTL 決定） | 同上 |
| 2.3.9 | 延遲：OneByte＝`1 + csr_access`（SET8／CLR8 是 `1 + 2 × csr_access`）；MultiByte＝`1 + len × csr_byte + csr_done` | 14.3 | 規劃值（預設 csr_access＝2、csr_byte＝2、csr_done＝1） | `latency.h` |

### 2.4 Main `MOV`（規格第 5 章）

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.4.1 | 區域編號：0 SCRATCHPAD、1 CSR_DATA_BUF、2 PERI_RESULT_SLOT、3 ALU_OPA、4 ALU_OPB、5 ALU_OUT、6 ALU_STATUS、7 CONST_POOL、8 PERI_RESULT_META | 5.1 | 規格 | `core/src/isa.h` |
| 2.4.2 | 可當來源：0、1、2、5、6、7、8；可當目的：0、3、4 | v31 5.1 | 規格 | `main_core.cpp` `opMainMov` |
| 2.4.3 | `MAIN_CSR_DATA_BUF` 不能當目的；`MAIN_CONST_POOL` 只能搬到 `MAIN_SCRATCHPAD` | v31 5.1 | 規格 | 同上 |
| 2.4.4 | 目的是 `ALU_OPA`／`ALU_OPB` 時必須 offset＝0、len＝2 | 5 | 規格 | 同上 |
| 2.4.5 | 區域大小：SCRATCHPAD、CSR_DATA_BUF、RESULT_SLOT、RESULT_META 各 32；ALU_OUT、ALU_STATUS 各 4；ALU_OPA／OPB 各 2；CONST_POOL 依映像 | 5.1 | 規格 | 同上 |
| 2.4.6 | 同一區域內：範圍重疊一律 fault `fault_mov_overlap`；不重疊是否允許看 profile `same_region_mov`（預設允許） | v31 5.2 | 重疊禁止是規格；不重疊是否允許是規劃值 | 同上 |
| 2.4.7 | 所有檢查都通過才開始複製，出錯不會留下寫到一半的結果 | 5 | 規格 | 同上 |
| 2.4.8 | 多位元組資料一律小端：`ALU_OPA`＝位元組 0 ＋（位元組 1 << 8） | 5、6 | 規格 | 同上 |
| 2.4.9 | 從結果槽讀但槽內沒有有效結果（或正在被取用）：fault `fault_slot_not_valid` | 12 | 假設（規格只說前置檢查失敗不清 valid） | 同上 |
| 2.4.10 | 從結果槽讀＝「取用」：資料在開始時就複製，槽在這道指令的延遲結束時才清除 valid | 12 | 假設（清除的確切時點） | `run.cpp` `SlotConsumeDone` |
| 2.4.11 | `PERI_RESULT_META` 的內容：位元組 0＝service id，位元組 4＝valid，位元組 5＝overrun，其餘 0 | 12 | 假設（位元組排列） | `impl.cpp` `regionBytes` |
| 2.4.12 | `ALU_STATUS` 讀出的 4 個位元組：位元組 0＝旗標（見 2.5.10），其餘 0 | 6.8 | 假設（位元組排列） | `impl.cpp` `aluStatusByte` |
| 2.4.13 | 延遲：`mov_setup + len × mov_byte`；取用結果槽是 `slot_consume_setup + len × mov_byte` | 14.3 | 規劃值（預設 1 + len、2 + len） | `latency.h` |

### 2.5 Main `ALU`（規格第 6 章）

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.5.1 | 運算元是 16 位元的 `ALU_OPA`、`ALU_OPB`；結果 `ALU_OUT` 是 32 位元 | 6 | 規格 | `main_core.cpp` `opMainAlu` |
| 2.5.2 | 描述子 1 個位元組：bit 7:6＝類別（0 ARITH、1 CMP、2 LOGIC、3 SHIFT） | 6.3 | 規格 | `codec.h` |
| 2.5.3 | ARITH 的子運算編碼：ADD 0、SUB 1、MUL 2、INC 3、DEC 4 | 6.3 只列名稱 | **假設**（profile `alu_arith_*` 可改） | `asmcore.h` `AluCodes` |
| 2.5.4 | CMP：EQ 0、NE 1、LT 2、GE 3；LOGIC：AND 0、OR 1、XOR 2、NOT 3 | 6.3 只列名稱 | **假設**（profile 可改） | 同上 |
| 2.5.5 | SHIFT：bit 5＝signed、bit 4＝方向（**0 SHL、1 SHR**）、bit 3:0＝位數（1..8，其餘 fault） | 6.3、6.7 | 欄位是規格；方向的數值是**假設**（見 Q4） | `isa.h` `ShiftDir` |
| 2.5.6 | ARITH 以精確值計算（不做 16 位元回繞）：ADD／SUB 得 17 位元精確結果，MUL 得 32 位元乘積；signed 模式下運算元當作有號 16 位元 | 6.4 | 規格 | `opMainAlu` |
| 2.5.7 | INC／DEC 作用在 `ALU_OPA`（＋1／−1），不看 `ALU_OPB` | 6.4 | 假設 | 同上 |
| 2.5.8 | CMP 只改 `cmp_true`，不改 `ALU_OUT` 與其他旗標；EQ／NE 要求 signed＝0（否則 fault）；LT／GE 依 signed 模式比較 | 6.5、6.8 | 規格 | 同上 |
| 2.5.9 | LOGIC：16 位元運算，NOT 只用 `ALU_OPA`；結果高 16 位元為 0 | 6.6 | 規格 | 同上 |
| 2.5.10 | SHL：精確 24 位元結果（signed 時先符號延伸）；SHR：16 位元右移，signed 為算術右移 | 6.7 | 規格 | 同上 |
| 2.5.11 | 旗標在 `ALU_STATUS` 位元組 0 的位置：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 | 6.8 只列旗標 | **假設** | `impl.cpp` `aluStatusByte` |
| 2.5.12 | 非 CMP 的運算重新計算 zero、negative、carry、borrow、overflow_s、ov16；`cmp_true` 保持到下一次 CMP | 6.8 | 假設 | `opMainAlu` |
| 2.5.13 | zero＝`ALU_OUT` 的 32 位元全為 0 | 6.8 | 規格 | 同上 |
| 2.5.14 | negative：ARITH 是「signed 模式或減法（SUB、DEC），且精確結果 < 0」；LOGIC 是結果的 bit 15；SHIFT 是結果 < 0 | 6.8 | LOGIC 取 bit 15 是假設 | 同上 |
| 2.5.15 | carry_u：無號 ADD／INC 結果 > 0xFFFF；borrow_u：無號 SUB／DEC 結果 < 0；overflow_s：有號 ADD／SUB／INC／DEC 超出 −32768..32767（MUL 不設） | 6.8 | 規格（INC／DEC 比照 ADD／SUB 是假設） | 同上 |
| 2.5.16 | ov16：結果無法無損放進 16 位元（依當次有號／無號判斷）；SHL 以精確 24 位元值判斷；SHR 與 LOGIC 一律 0 | 6.8 | 規格 | 同上 |
| 2.5.17 | 延遲：`latency.alu`（預設 1），MUL 是 `latency.mul`（預設 2） | 14.3 | 規劃值 | `latency.h` |

### 2.6 Main `BR`、`CASCADE`（規格第 7 章）

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.6.1 | 描述子型別：0 BR.COND、1 CASCADE_BEGIN、2 CASCADE_END、3 BR.ERET | 7.2、v31 16.5 | 規格 | `main_core.cpp` `opMainBr` |
| 2.6.2 | 分支目標是 8 位元的位元組位置（0..255）；成立且目標超出程式碼：fault `fault_branch_target` | 7.2 | 規格 | 同上 |
| 2.6.3 | 條件不成立：前進到下一個位元組（BR 長度 1） | 7 | 規格 | 同上 |
| 2.6.4 | 條件表（Main）：見下表 | 7.3 | 規格（運算式是模擬器對文字的解讀，環境檔可覆寫） | `impl.cpp` `buildDefaultConditions` |
| 2.6.5 | 條件 0xE、0xF 沒有定義：用到會 fault `fault_cond_undefined` | 7.3 | 規格（保留／profile 定義） | 同上 |
| 2.6.6 | CASCADE_BEGIN：記下選擇值（描述子的 arg）、步數歸 0，跳到目標；已在 cascade 中再 BEGIN：fault `fault_nested_cascade` | 7.6 | 規格 | `opMainBr` |
| 2.6.7 | CASCADE_END：描述子的索引必須等於目前步數，否則 fault；步數等於選擇值時結束並跳到目標，否則步數＋1、繼續下一個位元組 | 7.6 | 規格 | 同上 |
| 2.6.8 | `event_safe`（bit 9）只能用在 BR.COND；CASCADE 與 ERET 的描述子設了就 fault | v31 16 | 規格 | 同上 |
| 2.6.9 | BR 看到的旗標可以延遲 `flag.sample_delay_main` 個週期（預設 0＝立即） | — | 規劃值 | `impl.h` `evalCondition` |
| 2.6.10 | 延遲：BR.COND `latency.br_cond`（預設 1，成立與否相同）；CASCADE `latency.cascade`（預設 1） | 14.3 | 規劃值 | `latency.h` |

**Main 條件表（2.6.4）**

| 編碼 | 名稱 | 模擬器判斷的運算式 |
|---|---|---|
| 0x0 | ALWAYS | 恆真 |
| 0x1 | PERI_RESULT_VALID | 結果槽 valid |
| 0x2 | PERI_RESULT_OVERRUN | 結果槽 overrun |
| 0x3 | PERI_ERROR | `peri.done` 且 `peri.error` |
| 0x4 | PERI_NO_ERROR | `peri.done` 且非 `peri.error` |
| 0x5 | ALU_CMP_TRUE | `cmp_true` |
| 0x6 | ALU_OUT_OV16 | `ov16` |
| 0x7 | MAIN_TIMER_DONE | Main 計時器完成旗標 |
| 0x8 | FRAME_END_EVT | 畫面結束旗標（每個畫面結束時設為 1，由 CSR 動作清除） |
| 0x9 | VBLANK_OPEN | 目前在垂直消隱視窗內 |
| 0xA | HBLANK_OPEN | 目前在水平消隱視窗內 |
| 0xB | PGL_COMMIT_ELIGIBLE | `pgl_pending` 且在垂直消隱內且「視窗剩餘週期 ≥ `wcet_pgl`」 |
| 0xC | AE_COMMIT_ELIGIBLE | `ae_pending` 且在垂直消隱內且「剩餘 ≥ `wcet_ae`」 |
| 0xD | LINE_COMMIT_ELIGIBLE | `line_pending` 且在水平消隱內且「剩餘 ≥ `wcet_line`」 |

### 2.7 Peri `EXEC_XFER` 與 DeviceMap（規格第 8、9 章）

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.7.1 | OneByte：長度 2；描述子給讀寫、裝置編號、位址長度、暫存器位址；第二個位元組在 READ 時是 `PERI_RX_BUF` 的目的指標（bit 7:5 須為 0），在 WRITE 時是要寫的資料 | 8 | 規格 | `peri_core.cpp` `opPeriXfer` |
| 2.7.2 | MultiByte：只有 READ；描述子給 len、ptr；暫存器位址在指令後面的 inline 位元組（1 或 2 個，程式順序＝線上順序） | 8.5 | 規格 | 同上 |
| 2.7.3 | MultiByte WRITE：fault `fault_xfer_multibyte_write` | 8.5 | 規格（baseline 不支援） | 同上 |
| 2.7.4 | 位址長度 3：fault（保留）；`ptr + len > 32`：fault | 8 | 規格 | 同上 |
| 2.7.5 | 裝置編號查 DeviceMap；項目不在映像內：fault `fault_dev_undefined` | 9 | 規格 | 同上 |
| 2.7.6 | DeviceMap 項目：1 個位元組，bit 7＝bus_type、bit 6:0＝7 位元 I2C 位址 | 9.1 只給寬度 | **假設**（profile `device_map_*` 可改） | `codec.h` `unpackDev` |
| 2.7.7 | bus_type 不是 0（I2C）：fault `xfer_fault_reserved_bus_type` | 9 | SPI **未實作** | `opPeriXfer` |
| 2.7.8 | 時間：`xfer_issue + ⌈9 × 階段數 × cpu_hz ÷ i2c_hz⌉`，另加 `i2ch.overhead`、每階段的 `i2c.ack_delay + i2c.stretch`、讀取且有位址時的 `i2c.turnaround`。階段數：讀＝（有位址時 1 ＋ 位址長度）＋ 1 ＋ 資料長度；寫＝1 ＋ 位址長度 ＋ 資料長度 | 14.4 | 階段數依規格；START／STOP 不另計是假設；其餘是規劃值 | `latency.h`、`impl.h` `xferExtra` |
| 2.7.9 | Peri 在傳輸期間停住；**資料在傳輸結束的那個週期才寫進 `PERI_RX_BUF`** | 8、14.3 | 假設（寫入時點） | `peri_core.cpp` `xferComplete` |
| 2.7.10 | 沒有裝置回應（位址上沒有裝置，或被設定成 NACK）：設 `peri.error`（保持住），`PERI_RX_BUF` 不變，指令照常結束、PC 前進 | 11.5.3 | 設 error 是規格；其餘是假設 | 同上 |
| 2.7.11 | 傳輸到程式 EEPROM 自己的位址時，由 EEPROM 回應（寫入會改變之後取到的指令） | — | 使用者說明（共用匯流排） | `impl.cpp` `eepromDevice` |
| 2.7.12 | 從 EEPROM 取指時，`EXEC_XFER` 與兩個解碼器的取指共用同一條 I2C 匯流排，先到先得 | — | 使用者確認（2026-11-06） | `peri_core.cpp` `stepPeri` |

### 2.8 Peri `EXEC_PUB` 與結果槽（規格第 10、12 章）

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.8.1 | 描述子給 service id、來源指標、長度；service id 0：fault `fault_publish_service`；`src + len > 32`：fault | 10 | 規格 | `peri_core.cpp` `opPeriPub` |
| 2.8.2 | 結果槽只有一格：payload 32 個位元組＋service id＋valid＋overrun | 12 | 規格（FIFO **未實作**） | `impl.h` |
| 2.8.3 | 槽內已有結果（valid 或正在被取用）：**保留舊的、丟掉這一筆**，設 overrun | 12.8、13.7 | 規格（Keep Old Drop New） | `opPeriPub` |
| 2.8.4 | overrun 保持到被 CSR 動作 `overrun_clear` 清除 | 12 | 清除方式是假設 | `builtin.cpp` |
| 2.8.5 | 發佈在指令開始時複製資料並設 valid；Main 可以在 `slot.copy_extra + slot.valid_delay` 個週期之後看到（預設 0＝立即） | 12 | 假設／規劃值 | `opPeriPub`、`impl.h` `slotVisible` |
| 2.8.6 | Main 在同一週期才清掉槽：這次發佈延後 1 個週期（不算 overrun） | 12.6（最早 T+1） | 假設 | `peri_core.cpp` `stepPeri` |
| 2.8.7 | 延遲：`pub_setup + len × pub_byte + slot.copy_extra`（預設 2 + len） | 14.3 | 規劃值 | `latency.h` |

### 2.9 Peri `EXEC_CSR`、`BR`、Poll、Error／Done（規格第 4、11 章）

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.9.1 | Peri 的 `EXEC_CSR` 只有 OneByte 的 WRITE8／SET8／CLR8；MultiByte：fault `fault_peri_csr_multibyte`；READ8：fault `fault_peri_csr_read` | 4.1、4.2 | 規格 | `peri_core.cpp` `opPeriCsr` |
| 2.9.2 | Peri BR 的型別就是條件：0 ALWAYS、1 IF_READY、2 IF_NOT_READY、3 IF_TIMEOUT、4 IF_ERROR、5 IF_NO_ERROR、6 IF_TIMER_DONE、7 IF_TIMER_NOT_DONE、8 BR.ERET；9 以上 fault | 11.2、v31 16.5 | 規格 | `opPeriBr`、`buildDefaultConditions` |
| 2.9.3 | Poll：`poll.arm` 被設之後，下一次**成功且寫入 `PERI_RX_BUF[0]`** 的 READ 會評估：`PERI_RX_BUF[0] AND poll_mask` 不為 0 → `poll.ready`＝1、`poll.not_ready`＝0；否則相反。評估後解除 arm | 11.3、11.4 | 假設（評估時機與互斥） | `peri_core.cpp` `xferComplete` |
| 2.9.4 | `poll.timeout` 不會自動設定，只能由 CSR 動作 `poll_timeout_set` 或測試腳本設定 | 11 | 假設（規格沒說誰設） | `builtin.cpp` |
| 2.9.5 | `peri.error` 由 NACK 設定，保持到被清除；`peri.done`、`peri.error` 也可由 CSR 動作設定／清除 | 11.5 | 規格／假設 | 同上 |
| 2.9.6 | CSR 動作 `service_reset`：一次清除 poll.ready、poll.not_ready、poll.timeout、poll.arm、peri.done、peri.error | 13.2 | 假設（一個位元清全部） | 同上 |
| 2.9.7 | BR 看到的旗標可以延遲 `flag.sample_delay_peri` 個週期（預設 0） | — | 規劃值 | `impl.h` |

### 2.10 CSR 的模型

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.10.1 | **模擬器不知道任何 CSR 位址的意義**。每個 CSR 的行為由專案的環境檔（`[csr 名稱]`）定義；位址取自程式的 `.csr` 定義 | 規格只給名稱 | 假設（位址與位元是板子／RTL 的事） | `env.cpp` |
| 2.10.2 | CSR 種類：`ram`（存什麼讀什麼）、`value`（對應到一個信號，可多位元組小端）、`action`（寫 1 觸發動作）、`status`（各位元由運算式算出，唯讀）、`const` | — | 假設 | `impl.cpp` `csrRead8`、`csrWrite` |
| 2.10.3 | 環境檔沒有定義的位址當成 RAM（`sim.unmapped_csr = fault` 可改成 fault） | — | 假設 | 同上 |
| 2.10.4 | `action` 種類：寫入或 SET 的位元為 1 就觸發；CLR 不觸發；讀回是 0 | — | 假設 | `csrWrite` |
| 2.10.5 | 不檢查是 Main 還是 Peri 在存取（兩邊可以讀寫同一個位址） | — | 假設 | 同上 |
| 2.10.6 | 兩邊同時要存取 CSR：共用一個仲裁器，見 2.15 | — | 使用者確認 | `run.cpp` |

### 2.11 內建的 CSR 動作

環境檔把 CSR 的位元接到這些動作上（`bit3 = timer_start` 之類）。**哪個位址的哪個位元做什麼，完全由環境檔決定。**

| 動作 | 效果 |
|---|---|
| `poll_clear` | 清 poll.ready、poll.not_ready、poll.timeout |
| `poll_arm` | 設 poll.arm |
| `poll_timeout_set` | 設 poll.timeout |
| `done_set`／`done_clear` | 設／清 peri.done |
| `error_set`／`error_clear` | 設／清 peri.error |
| `service_reset` | 見 2.9.6 |
| `timer_start`／`timer_clear` | 啟動／清除 **Peri 的計時器**（旗標 `timer.done`） |
| `main_timer_start`／`main_timer_clear` | 啟動／清除 **Main 的計時器**（旗標 `main_timer.done`） |
| `frame_end_clear` | 清畫面結束旗標 |
| `overrun_clear` | 清結果槽 overrun |
| `set:名稱`／`clr:名稱` | 直接設／清任一信號 |

### 2.12 計時器（對應 Q1）

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.12.1 | **有兩個獨立的計時器**：Peri 的（週期信號 `timer_period`、完成旗標 `timer.done`）與 Main 的（`main_timer_period`、`main_timer.done`）。一邊啟動、清除都不影響另一邊 | 7.3 的 `MAIN_TIMER_DONE`、11.2 的 `IF_TIMER_DONE` | **假設**（依兩個不同的名稱做成兩顆；見 Q1） | `builtin.cpp` `startTimer` |
| 2.12.2 | 兩個計時器數同一個時脈 `clock.timer_hz`＝13.5 MHz（ISA 時脈除以 2，即每 2 個 ISA 週期一個 tick） | — | 使用者確認（2026-11-04） | `builtin.cpp` `timerDoneAt` |
| 2.12.3 | 啟動：完成旗標立刻清為 0；週期 N 個 tick 之後設為 1 | 11 | 規格（硬體計時＋BR 輪詢） | `startTimer` |
| 2.12.4 | 週期為 0 就啟動：不計時，只在紀錄裡留一則提示 | — | 假設 | 同上 |
| 2.12.5 | 計時中再啟動一次：重新開始，前一次的到期作廢 | 11 | 假設 | 同上（generation 計數） |
| 2.12.6 | `timer_clear`：清完成旗標，並使進行中的計時作廢 | — | 假設 | `builtin.cpp` |
| 2.12.7 | **計時器到時不會觸發事件，也不會改變程式流程**；程式必須自己用 BR 輪詢完成旗標 | 「不要發明 timer interrupt」 | 規格 | — |
| 2.12.8 | 完成旗標為 0 不代表計時器已啟動（沒有「計時器有效」旗標） | 11 | 規格 | — |
| 2.12.9 | 預除器 `timer.prescaler`：`free_running`（預設；tick 的格線從重置起固定，啟動後第一個 tick 起算）或 `restart`（從啟動時起算） | — | 規劃值 | `timerDoneAt` |
| 2.12.10 | 計數 `timer.count`：`n`（預設，N 個 tick）或 `n_plus_1`；相位 `timer.phase`（預設 0）；啟動延遲、完成延遲各自可設（預設 0） | — | 規劃值 | 同上 |

### 2.13 影像時序（規格 7.4、第 14 章）

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.13.1 | 畫面長度、垂直消隱起點與長度、行長度、水平消隱起點與長度：全部由參數／環境檔設定；**預設都是 0，即沒有影像時序**（VBLANK／HBLANK 恆為關） | 14 | 由專案設定 | `impl.cpp` `buildSignals` |
| 2.13.2 | `vblank_open`＝（目前週期 mod 畫面長度）落在 [起點, 起點＋長度)；`hblank_open` 同理（對行長度） | 14 | 假設（純由時間算出，沒有獨立的時序產生器） | 同上 |
| 2.13.3 | 視窗剩餘週期＝起點＋長度−（目前位置），不在視窗內時為 0 | 7.4 | 假設 | 同上 |
| 2.13.4 | 每個畫面結束時把畫面結束旗標設為 1 | 7.3 | 假設（設定時點） | `run.cpp` `FrameEnd` |
| 2.13.5 | `pgl_pending`、`ae_pending`、`line_pending` 與 `wcet_pgl`、`wcet_ae`、`wcet_line` 是可設定的信號（初值來自參數 `frame.wcet_*`） | 7.4 | 假設（誰設 pending 規格沒說） | `impl.cpp` |

### 2.14 延遲數值（規格第 14 章）

預設值全部是規格 14.3／14.4 的**規劃數值**，不是簽核值。

| 指令 | 週期數（預設） |
|---|---|
| BR.COND | 1 |
| CASCADE_BEGIN／END | 1 |
| ALU（ADD、SUB、CMP、LOGIC、SHIFT、INC、DEC） | 1 |
| ALU MUL | 2 |
| MOV | 1 + len |
| MOV 取用結果槽 | 2 + len |
| EXEC_CSR OneByte（WRITE8、READ8） | 1 + 2 ＝ 3 |
| EXEC_CSR OneByte（SET8、CLR8） | 1 + 2 × 2 ＝ 5 |
| EXEC_CSR MultiByte | 1 + len × 2 + 1 |
| EXEC_PUB | 2 + len |
| EXEC_XFER | 2 + I2C 位元組階段數 × 9 個 SCL 脈衝（27 MHz／400 kHz：每脈衝 67.5 週期） |
| 事件派送／BR.ERET | 2／2 |

另外每道指令可再加 `main.fetch_cycles`／`peri.fetch_cycles`（預設 0），以及 2.2 的取指時間。Main 與 Peri 的 CSR 存取週期可分開設（`csr.access_main`、`csr.access_peri`，預設−1＝與 `latency.csr_access` 相同）。

### 2.15 兩個 sequencer 共用的資源（對應 Q6）

| # | 資源 | 誰會用到 | 模擬器的規則 | 依據 |
|---|---|---|---|---|
| 2.15.1 | I2C 匯流排 | 兩個解碼器的 EEPROM 取指、Peri 的 `EXEC_XFER` | 先到先得；同週期 Main 先；不插隊；`EXEC_XFER` 占用到傳輸結束 | 使用者確認（2026-11-06） |
| 2.15.2 | CSR 仲裁器 | 兩邊的 `EXEC_CSR` | 先到先得；同週期依 `csr.arbiter_policy`（預設 `main_first`）；占用時間＝max(1, 指令延遲−1)；交接延遲 `csr.arbiter_penalty`（預設 0） | 規則是使用者確認（2026-11-05）；占用時間與交接延遲是假設 |
| 2.15.3 | Status Flag 單元 | 兩邊「會讀旗標」的 BR（條件不是 ALWAYS、也不是純 ALU 旗標者） | 先到先得；同週期依 `flag.arbiter_policy`（預設 `main_first`）；占用 `flag.access_cycles`（預設 1） | 是一個共用單元：使用者確認；規則與時間：使用者決定預設、其餘假設 |
| 2.15.4 | Data Xfer | Peri 的 `EXEC_PUB`、Main 從結果槽的 MOV | **預設不模擬競爭**（`datapath.contention = false`） | 假設 |
| 2.15.5 | 程式 SRAM 的讀取埠（Frontend 模式） | 兩個 Frontend | `shared`：每個 SRAM 週期一次讀取，先到先得，同時 Main 先；或 `slots`：每個 ISA 週期各有一個時槽 | 規劃值 |
| 2.15.6 | 等待的做法 | — | 資源被占用時，指令**不開始**，等輪到它再試；等待中的一方被排為「下一位」，後到的排在它後面 | 假設 |
| 2.15.7 | 計時器 | — | **不視為共用資源**（兩顆各自獨立，見 2.12） | 假設（Q1） |

### 2.16 硬體事件（規格 v31 第 16 章）

| # | 模擬器目前的做法 | 規格 | 依據 | 程式位置 |
|---|---|---|---|---|
| 2.16.1 | Main、Peri 各有一套：pending（64 個事件 id）、是否在處理程序中、返回用的 PC | 16 | 規格 | `impl.h` `EventState` |
| 2.16.2 | **事件只能由外部注入**（環境檔 `[inject]`、測試腳本、除錯器的注入命令）。模擬器裡沒有任何硬體來源會自己產生事件——計時器、畫面結束、NACK 都不會 | 16 | 假設（事件來源由目標硬體定義） | `run.cpp` `raiseEvent` |
| 2.16.3 | 同一個 id 在 pending 期間再次發生：合併成一次 | 16 | 假設 | 同上 |
| 2.16.4 | 派送時機：只在一道**帶 `event_safe` 的一般 BR 執行完之後**；此時 PC 已是分支決定的下一個位置，它就是返回位置 | 16.3 | 規格 | `run.cpp` `eventCheckpoint` |
| 2.16.5 | 不派送的情況：已在處理程序中（不巢狀）；Main 正在 cascade 中；該事件沒有向量（事件繼續 pending） | 16 | 規格 | 同上 |
| 2.16.6 | 多個事件同時 pending：id 最小的先 | 16 | 假設（優先順序由 profile 定義） | 同上 |
| 2.16.7 | 向量在程式碼之外：fault `fault_event_vector` | 16 | 規格 | 同上 |
| 2.16.8 | `BR.ERET`：回到儲存的返回 PC（全寬度，不是 8 位元）；不在處理程序中執行：fault `fault_bad_eret_context` | 16.5 | 規格 | `run.cpp` `doEret` |
| 2.16.9 | ERET 本身不會再派送另一個事件；pending 的事件要等下一個 `event_safe` 的 BR | 16 | 規格 | 同上 |
| 2.16.10 | 延遲：派送 `latency.event_dispatch + event.dispatch_extra`（預設 2）；ERET `latency.eret + event.eret_extra`（預設 2） | 16.9 | 規劃值 | `latency.h` |
| 2.16.11 | 「無條件跳到自己」如果帶 `event_safe`，不算程式結束，而是在等事件 | — | 假設 | `opMainBr` |

### 2.17 Fault

發生 fault 時：那個 sequencer 停止（`sim.fault_stops = all` 時兩邊都停），記下代碼、PC、週期。**硬體實際在哪一級偵測、偵測後怎麼反應，尚未確認**（`docs/HW_FAULT_DETECTION.md`）。

| 代碼 | 什麼時候 |
|---|---|
| `fault_pc_out_of_code` | PC 超出程式碼，或指令缺第二個位元組／位址位元組 |
| `fault_opcode` | opcode 沒有對應的處理 |
| `fault_desc_index` | 描述子索引超出它的表 |
| `fault_desc_reserved` | 描述子保留位元不為 0、保留的型別／運算／區域、READ 指標 bit 7:5 不為 0、ALU 編碼未指定、移位位數不在 1..8、EQ／NE 用了 signed |
| `fault_range` | `ptr + len` 超出 32（CSR MultiByte、EXEC_XFER、EXEC_PUB） |
| `fault_mov_region` | 區域不能當來源／目的；常數池搬到 scratchpad 以外 |
| `fault_mov_range` | 範圍超出區域；ALU 運算元不是 offset 0、len 2 |
| `fault_mov_overlap` | 同一區域內範圍重疊（或 profile 不允許同區域搬移） |
| `fault_csrbuf_invalid` | 讀到未完成 READ 的 `MAIN_CSR_DATA_BUF` 位元組（`csr_buf_valid` 開啟時） |
| `fault_slot_not_valid` | 從沒有有效結果的結果槽 MOV |
| `fault_cond_undefined` | 分支條件沒有定義 |
| `fault_branch_target` | 分支目標、cascade 目標或返回 PC 超出程式碼 |
| `fault_nested_cascade` | cascade 中再 CASCADE_BEGIN |
| `fault_cascade_end_without_begin` | 沒有 cascade 卻 CASCADE_END |
| `fault_cascade_step_mismatch` | CASCADE_END 的索引與目前步數不符 |
| `fault_bad_eret_context` | 不在處理程序中執行 BR.ERET |
| `fault_event_vector` | 事件向量超出程式碼 |
| `fault_peri_csr_multibyte` | Peri 用了 MultiByte 的 EXEC_CSR |
| `fault_peri_csr_read` | Peri 用了 READ8 |
| `fault_publish_service` | 發佈 service id 0 |
| `fault_xfer_multibyte_write` | EXEC_XFER MultiByte WRITE |
| `fault_dev_undefined` | DeviceMap 項目不在映像內 |
| `xfer_fault_reserved_bus_type` | DeviceMap 的 bus_type 不是 I2C |
| `fault_csr_unmapped` | 存取未定義的 CSR（只在 `sim.unmapped_csr = fault` 時） |

### 2.18 重置後的狀態

| 項目 | 值 | 依據 |
|---|---|---|
| 兩個 PC | 0 | 規格 |
| `MAIN_SCRATCHPAD`、`MAIN_CSR_DATA_BUF`、`PERI_RX_BUF`、結果槽 payload | 全 0 | 假設（硬體可能是未定值；組譯器的資料流檢查另外把它們當成「未初始化」） |
| `ALU_OPA`、`ALU_OPB`、`ALU_OUT`、所有 ALU 旗標 | 0 | 假設 |
| 結果槽 valid、overrun；poll、timer、done、error 各旗標 | 0 | 假設 |
| 事件 pending、處理程序狀態 | 無 | 假設 |
| `ram` 種類的 CSR | 環境檔的 `reset` 值（預設 0） | 由專案設定 |
| cascade 狀態 | 未啟動 | 假設 |

### 2.19 沒有實作的

| 項目 | 現況 |
|---|---|
| SPI | DeviceMap 的 bus_type 非 I2C 一律 fault |
| EXEC_XFER MultiByte WRITE | fault（規格 baseline 也不支援） |
| FIFO 結果槽 | 只有一格 |
| 共用一顆計時器 | 沒有（Q1；原訂的 `timer.shared` 實驗尚未做） |
| 事件的硬體來源 | 沒有，只能注入 |
| I2C 的 START／STOP、仲裁失敗、時脈延展的細節 | 只以每位元組的額外週期參數概括 |
| 把「等另一個 sequencer」的時間算進 WCET | 沒有（模擬時會等，但組譯器的 WCET 不計） |
| 匯入 FPGA／RTL 擷取資料並自動比對（S8） | 沒有 |
| 對照 RTL／硬體的驗證 | 沒有做過任何一項 |

---

## 3. 所有硬體參數與預設值

由 `asmsim --list-params` 產生（2026-11-20）。每個參數的說明與「在 FPGA 上怎麼量」見 `docs/SIM_PARAMS.md`。

| 參數 | 預設 | 單位 | 範圍／選項 | 預設值的來源 |
|---|---|---|---|---|
| `latency.br_cond` | 1 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.cascade` | 1 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.alu` | 1 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.mul` | 2 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.mov_setup` | 1 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.mov_byte` | 1 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.slot_consume_setup` | 2 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.csr_access` | 2 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.csr_byte` | 2 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.csr_done` | 1 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.pub_setup` | 2 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.pub_byte` | 1 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.event_dispatch` | 2 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.eret` | 2 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `latency.xfer_issue` | 2 | ISA cycles | 0..100000 | 規格 14.3／14.4 的規劃數值 |
| `clock.cpu_hz` | 27000000 | Hz | 1000..1000000000 | 規格 14.1 |
| `clock.i2c_hz` | 400000 | Hz | 1000..10000000 | 規格 14.4 |
| `clock.timer_hz` | 13500000 | Hz | 1000..1000000000 | 使用者確認（2026-11-04） |
| `timing.frame_cycles` | 0 | ISA cycles | 0..2147483647 | 由專案設定（預設 0＝沒有影像時序） |
| `timing.vblank_start` | 0 | ISA cycles | 0..2147483647 | 由專案設定（預設 0＝沒有影像時序） |
| `timing.vblank_len` | 0 | ISA cycles | 0..2147483647 | 由專案設定（預設 0＝沒有影像時序） |
| `timing.line_cycles` | 0 | ISA cycles | 0..2147483647 | 由專案設定（預設 0＝沒有影像時序） |
| `timing.hblank_start` | 0 | ISA cycles | 0..2147483647 | 由專案設定（預設 0＝沒有影像時序） |
| `timing.hblank_len` | 0 | ISA cycles | 0..2147483647 | 由專案設定（預設 0＝沒有影像時序） |
| `frame.wcet_pgl` | 0 | ISA cycles | 0..2147483647 | 由專案設定（預設 0＝沒有影像時序） |
| `frame.wcet_ae` | 0 | ISA cycles | 0..2147483647 | 由專案設定（預設 0＝沒有影像時序） |
| `frame.wcet_line` | 0 | ISA cycles | 0..2147483647 | 由專案設定（預設 0＝沒有影像時序） |
| `sim.speed` | 1000 | ISA cycles per second | 0..1000000000 | 模擬器的設定，不是硬體 |
| `sim.tie_order` | main_first | — | main_first／peri_first | 模擬器的設定，不是硬體 |
| `sim.unmapped_csr` | ram | — | ram／fault | 模擬器的設定，不是硬體 |
| `sim.fault_stops` | domain | — | domain／all | 模擬器的設定，不是硬體 |
| `fetch.source` | eeprom | — | eeprom／sram／frontend | 規劃值，待 RTL |
| `boot.load` | i2c | — | instant／i2c | 規劃值，待 RTL |
| `frontend.sram_latency` | 1 | SRAM cycles | 1..16 | 規劃值，待 RTL |
| `frontend.sram_policy` | shared | — | shared／slots | 規劃值，待 RTL |
| `frontend.hold_cycles` | 0 | ISA cycles | 0..1000 | 規劃值，待 RTL |
| `frontend.dec_cycles` | 1 | ISA cycles | 0..1000 | 規劃值，待 RTL |
| `frontend.fire_cycles` | 1 | ISA cycles | 0..1000 | 規劃值，待 RTL |
| `fetch.read_mode` | sequential | — | sequential／per_byte／instant | 規劃值，待 RTL |
| `fetch.transaction_overhead` | 2 | ISA cycles | 0..100000 | 規劃值，待 RTL |
| `main.fetch_cycles` | 0 | ISA cycles | 0..1000 | 規劃值，待 RTL |
| `peri.fetch_cycles` | 0 | ISA cycles | 0..1000 | 規劃值，待 RTL |
| `csr.access_main` | -1 | ISA cycles | -1..100000 | 規劃值，待 RTL |
| `csr.access_peri` | -1 | ISA cycles | -1..100000 | 規劃值，待 RTL |
| `reset.boot_cycles` | 0 | ISA cycles | 0..1000000 | 規劃值，待 RTL |
| `event.dispatch_extra` | 0 | ISA cycles | 0..100000 | 規劃值，待 RTL |
| `event.eret_extra` | 0 | ISA cycles | 0..100000 | 規劃值，待 RTL |
| `timer.main.clock_div` | 0 | ISA cycles per tick | 0..65535 | 規劃值，待 RTL |
| `timer.main.start_latency` | 0 | ISA cycles | 0..1000000 | 規劃值，待 RTL |
| `timer.main.done_latency` | 0 | ISA cycles | 0..1000000 | 規劃值，待 RTL |
| `timer.peri.clock_div` | 0 | ISA cycles per tick | 0..65535 | 規劃值，待 RTL |
| `timer.peri.start_latency` | 0 | ISA cycles | 0..1000000 | 規劃值，待 RTL |
| `timer.peri.done_latency` | 0 | ISA cycles | 0..1000000 | 規劃值，待 RTL |
| `timer.prescaler` | free_running | — | free_running／restart | 規劃值，待 RTL |
| `timer.count` | n | — | n／n_plus_1 | 規劃值，待 RTL |
| `timer.phase` | 0 | ISA cycles | 0..65535 | 規劃值，待 RTL |
| `i2ch.overhead` | 0 | ISA cycles | 0..1000000 | 規劃值，待 RTL |
| `i2c.ack_delay` | 0 | ISA cycles per byte | 0..1000 | 規劃值，待 RTL |
| `i2c.stretch` | 0 | ISA cycles per byte | 0..1000000 | 規劃值，待 RTL |
| `i2c.turnaround` | 0 | ISA cycles | 0..1000000 | 規劃值，待 RTL |
| `csr.arbiter_policy` | main_first | — | none／main_first／peri_first／round_robin | 使用者確認（2026-11-05） |
| `csr.arbiter_penalty` | 0 | ISA cycles | 0..100000 | 規劃值，待 RTL |
| `datapath.contention` | false | — | true／false | 規劃值，待 RTL |
| `datapath.contention_cycles` | 0 | ISA cycles | 0..100000 | 規劃值，待 RTL |
| `slot.copy_extra` | 0 | ISA cycles | 0..100000 | 規劃值，待 RTL |
| `slot.valid_delay` | 0 | ISA cycles | 0..100000 | 規劃值，待 RTL |
| `flag.arbiter_policy` | main_first | — | none／main_first／peri_first／round_robin | 使用者決定（2026-11-04） |
| `flag.access_cycles` | 1 | ISA cycles | 1..100000 | 規劃值，待 RTL |
| `flag.arbiter_penalty` | 0 | ISA cycles | 0..100000 | 規劃值，待 RTL |
| `flag.sample_delay_main` | 0 | ISA cycles | 0..100000 | 規劃值，待 RTL |
| `flag.sample_delay_peri` | 0 | ISA cycles | 0..100000 | 規劃值，待 RTL |

---

## 4. 怎麼確認模擬器與硬體一致

唯一的辦法是讓同一份程式在兩邊跑，比對看得到的結果。不必等實體晶片，RTL 模擬就可以開始。由便宜到貴分四層：

| 層 | 要回答的問題 | 做法 | 會確認第 2 節的哪些項目 |
|---|---|---|---|
| 1 編碼 | 我們產生的位元組，硬體解出來是不是同一道指令？ | 每種運算各做一次、把結果寫到 CSR 的短程式；在 RTL 模擬看 CSR 被寫入的值 | 2.5.3–2.5.5、2.5.11、2.7.6，以及描述子位元組序、表格位置（`PROJECT_STATUS.md` 7.4 的 D1–D6） |
| 2 行為 | 同一支程式，兩邊做的事順序與內容一樣嗎？ | 比對 CSR 寫入、I2C 交易、結果槽的發佈與取用（模擬器：`asmsim --events ev.csv` 或 `--vcd run.vcd`） | 2.3–2.9、2.12（含 Q1：讓兩邊各啟動長度不同的計時，看兩個完成旗標）、2.16 |
| 3 時間 | 每道指令花的週期數一樣嗎？ | 依各參數的量測方法（`asmsim --list-params`）在 FPGA 上量，填進 `.simparams` 並標記 `status = measured` | 2.2.7、2.14、2.15、第 3 節全部 |
| 4 出錯 | 不合法的指令，硬體在哪一級發現、怎麼反應？ | 填 `docs/HW_FAULT_DETECTION.md` | 2.17 |

目前專案**沒有**現成的第 1 層測試程式，也沒有自動比對兩邊結果的工具。
