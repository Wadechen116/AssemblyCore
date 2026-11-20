# 一致性測試：預期結果（第 1 層編碼、第 2 層行為）

由 `make_conformance.py` 產生（請改產生器，不要直接改這份）。結果 CSR 的基底：`RES = 0x600`；每支測試最後寫入 `RES_DONE = 0x67F`，值 `0xA5`。

第 1 層的預期值依規格 v31 的文字獨立算出（不是抄模擬器的輸出）；第 2 層的預期行為依規格的敘述逐支寫定。本專案的模擬器已跑過全部程式，結果相同。

「取決於」欄有內容的列，代表那個預期值建立在某個前提或假設上；與硬體不同時先看那一欄。

---

# 第 1 層：編碼

硬體解出來的指令，是不是工具編進去的那一道？只看最後的值。

## c01_csr_branch

EXEC_CSR 單位元組的三種寫入，以及無條件分支

**檢查的對象**：指令位元組的 opcode／arg 切分；CsrOneByteDesc（運算、位址）與它的位元組序；立即值位元組；BranchDesc 的目標

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x5A` | WRITE8 直接寫入 |  |
| `0x601` | RES+1 | `0xFF` | 先 WRITE8 0xF0，再 SET8 0x0F（讀出後 OR） | 結果 CSR 必須是「寫什麼讀回什麼」的一般暫存器 |
| `0x602` | RES+2 | `0xC3` | 先 WRITE8 0xFF，再 CLR8 0x3C（讀出後 AND NOT） | 結果 CSR 必須是「寫什麼讀回什麼」的一般暫存器 |
| `0x604` | RES+4 | `0x0D` | 分支目標之後的那道指令 |  |
| `0x603` | RES+3 | **不可被寫入** | 被 JMP 跳過：這裡如果被寫入，表示分支沒有跳到它的目標 | |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**結果不同時怎麼判讀**

- 寫到了別的位址：16 位元的 CsrOneByteDesc 被以相反的位元組序讀取（D2），或位址欄位不在工具放的位置。
- RES+1 = 0x0F 或 RES+2 = 0x00：SET8／CLR8 沒有先讀暫存器。RES+1 = 0xF0 且 RES+2 = 0xFF：SET8 與 CLR8 的運算碼對調了。

## c02_mov_const

區域之間的 MOV、常數池、EXEC_CSR WRITE_SP

**檢查的對象**：MainMoveDesc（區域、位移、長度）與它的位元組序；常數池的位置；多位元組寫入的位元組順序

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x34` | dw 常數 0x1234，低位元組在前（位元組 0） |  |
| `0x601` | RES+1 | `0x12` | dw 常數 0x1234，低位元組在前（位元組 1） |  |
| `0x604` | RES+4 | `0x11` | 四個 db 常數，依寫的順序（位元組 0） |  |
| `0x605` | RES+5 | `0x22` | 四個 db 常數，依寫的順序（位元組 1） |  |
| `0x606` | RES+6 | `0x33` | 四個 db 常數，依寫的順序（位元組 2） |  |
| `0x607` | RES+7 | `0x44` | 四個 db 常數，依寫的順序（位元組 3） |  |
| `0x608` | RES+8 | `0x22` | MAIN_SCRATCHPAD[5..6] 複製到 [10..11]（位元組 0） | 硬體必須允許同一區域內不重疊的 MOV（profile same_region_mov） |
| `0x609` | RES+9 | `0x33` | MAIN_SCRATCHPAD[5..6] 複製到 [10..11]（位元組 1） | 硬體必須允許同一區域內不重疊的 MOV（profile same_region_mov） |
| `0x60C` | RES+12 | `0xFE` | 0xCAFE 經過 ALU_OPA 與 ALU_OUT：位元組 0 是低位元組（位元組 0） |  |
| `0x60D` | RES+13 | `0xCA` | 0xCAFE 經過 ALU_OPA 與 ALU_OUT：位元組 0 是低位元組（位元組 1） |  |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**結果不同時怎麼判讀**

- RES+0..1 = 12 34：常數池或 ALU 區域是大端。
- RES+8..9 = 11 22 或 33 44：MainMoveDesc 的來源位移與目的位移不在工具放的位置。
- 每一格都不對：32 位元的 MainMoveDesc 被以相反的位元組序讀取（D2），或 main_const_pool_offset 與 layout 不符。

## c03_alu_arith

ALU 的 ADD／SUB／MUL／INC／DEC

**檢查的對象**：arith_op 的編碼數值（D1）；signed_mode；精確結果（不做 16 位元回繞）

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x0A` | ADD 無號，OPA = 7、OPB = 3 → 10（位元組 0） |  |
| `0x601` | RES+1 | `0x00` | ADD 無號，OPA = 7、OPB = 3 → 10（位元組 1） |  |
| `0x602` | RES+2 | `0x00` | ADD 無號，OPA = 7、OPB = 3 → 10（位元組 2） |  |
| `0x603` | RES+3 | `0x00` | ADD 無號，OPA = 7、OPB = 3 → 10（位元組 3） |  |
| `0x604` | RES+4 | `0x04` | SUB 無號，OPA = 7、OPB = 3 → 4（位元組 0） |  |
| `0x605` | RES+5 | `0x00` | SUB 無號，OPA = 7、OPB = 3 → 4（位元組 1） |  |
| `0x606` | RES+6 | `0x00` | SUB 無號，OPA = 7、OPB = 3 → 4（位元組 2） |  |
| `0x607` | RES+7 | `0x00` | SUB 無號，OPA = 7、OPB = 3 → 4（位元組 3） |  |
| `0x608` | RES+8 | `0x15` | MUL 無號，OPA = 7、OPB = 3 → 21（位元組 0） |  |
| `0x609` | RES+9 | `0x00` | MUL 無號，OPA = 7、OPB = 3 → 21（位元組 1） |  |
| `0x60A` | RES+10 | `0x00` | MUL 無號，OPA = 7、OPB = 3 → 21（位元組 2） |  |
| `0x60B` | RES+11 | `0x00` | MUL 無號，OPA = 7、OPB = 3 → 21（位元組 3） |  |
| `0x60C` | RES+12 | `0x08` | INC 無號，OPA = 7、OPB = 3 → 8（位元組 0） |  |
| `0x60D` | RES+13 | `0x00` | INC 無號，OPA = 7、OPB = 3 → 8（位元組 1） |  |
| `0x60E` | RES+14 | `0x00` | INC 無號，OPA = 7、OPB = 3 → 8（位元組 2） |  |
| `0x60F` | RES+15 | `0x00` | INC 無號，OPA = 7、OPB = 3 → 8（位元組 3） |  |
| `0x610` | RES+16 | `0x06` | DEC 無號，OPA = 7、OPB = 3 → 6（位元組 0） |  |
| `0x611` | RES+17 | `0x00` | DEC 無號，OPA = 7、OPB = 3 → 6（位元組 1） |  |
| `0x612` | RES+18 | `0x00` | DEC 無號，OPA = 7、OPB = 3 → 6（位元組 2） |  |
| `0x613` | RES+19 | `0x00` | DEC 無號，OPA = 7、OPB = 3 → 6（位元組 3） |  |
| `0x614` | RES+20 | `0x01` | ADD 有號，OPA = 0xFFFE、OPB = 3 → 0x00000001（位元組 0） |  |
| `0x615` | RES+21 | `0x00` | ADD 有號，OPA = 0xFFFE、OPB = 3 → 0x00000001（位元組 1） |  |
| `0x616` | RES+22 | `0x00` | ADD 有號，OPA = 0xFFFE、OPB = 3 → 0x00000001（位元組 2） |  |
| `0x617` | RES+23 | `0x00` | ADD 有號，OPA = 0xFFFE、OPB = 3 → 0x00000001（位元組 3） |  |
| `0x618` | RES+24 | `0xFB` | SUB 有號，OPA = 0xFFFE、OPB = 3 → 0xFFFFFFFB（位元組 0） |  |
| `0x619` | RES+25 | `0xFF` | SUB 有號，OPA = 0xFFFE、OPB = 3 → 0xFFFFFFFB（位元組 1） |  |
| `0x61A` | RES+26 | `0xFF` | SUB 有號，OPA = 0xFFFE、OPB = 3 → 0xFFFFFFFB（位元組 2） |  |
| `0x61B` | RES+27 | `0xFF` | SUB 有號，OPA = 0xFFFE、OPB = 3 → 0xFFFFFFFB（位元組 3） |  |
| `0x61C` | RES+28 | `0xFA` | MUL 有號，OPA = 0xFFFE、OPB = 3 → 0xFFFFFFFA（位元組 0） |  |
| `0x61D` | RES+29 | `0xFF` | MUL 有號，OPA = 0xFFFE、OPB = 3 → 0xFFFFFFFA（位元組 1） |  |
| `0x61E` | RES+30 | `0xFF` | MUL 有號，OPA = 0xFFFE、OPB = 3 → 0xFFFFFFFA（位元組 2） |  |
| `0x61F` | RES+31 | `0xFF` | MUL 有號，OPA = 0xFFFE、OPB = 3 → 0xFFFFFFFA（位元組 3） |  |
| `0x620` | RES+32 | `0x01` | ADD 無號，OPA = 0xFFFE、OPB = 3 → 0x00010001（位元組 0） |  |
| `0x621` | RES+33 | `0x00` | ADD 無號，OPA = 0xFFFE、OPB = 3 → 0x00010001（位元組 1） |  |
| `0x622` | RES+34 | `0x01` | ADD 無號，OPA = 0xFFFE、OPB = 3 → 0x00010001（位元組 2） |  |
| `0x623` | RES+35 | `0x00` | ADD 無號，OPA = 0xFFFE、OPB = 3 → 0x00010001（位元組 3） |  |
| `0x624` | RES+36 | `0xFC` | SUB 無號 3 − 7 → 精確的 −4，以 32 位元表示（位元組 0） | 規格說「精確的有號 17 位元差」；如何延伸到 ALU_OUT[31:0] 是我們的解讀（符號延伸） |
| `0x625` | RES+37 | `0xFF` | SUB 無號 3 − 7 → 精確的 −4，以 32 位元表示（位元組 1） | 規格說「精確的有號 17 位元差」；如何延伸到 ALU_OUT[31:0] 是我們的解讀（符號延伸） |
| `0x626` | RES+38 | `0xFF` | SUB 無號 3 − 7 → 精確的 −4，以 32 位元表示（位元組 2） | 規格說「精確的有號 17 位元差」；如何延伸到 ALU_OUT[31:0] 是我們的解讀（符號延伸） |
| `0x627` | RES+39 | `0xFF` | SUB 無號 3 − 7 → 精確的 −4，以 32 位元表示（位元組 3） | 規格說「精確的有號 17 位元差」；如何延伸到 ALU_OUT[31:0] 是我們的解讀（符號延伸） |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**結果不同時怎麼判讀**

- OPA = 7、OPB = 3 時五種運算的結果都不同（ADD = 10、SUB = 4、MUL = 21、INC = 8、DEC = 6）。某一格出現的是另一種運算的值，就知道硬體把那個編碼當成哪一種運算。
- 「ADD 有號」那一格（RES+20 起）是 01 00 01 00：signed_mode 沒有作用（那是無號的 0xFFFE + 3）。

## c04_alu_cmp

ALU 的 CMP.EQ／NE／LT／GE，以及 BR.COND ALU_CMP_TRUE

**檢查的對象**：cmp_op 的編碼數值（D1）；比較的 signed_mode；分支條件 ALU_CMP_TRUE

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x00` | CMP.EQ 無號，OPA = 5、OPB = 9（成立寫 1，不成立寫 0） |  |
| `0x601` | RES+1 | `0x01` | CMP.NE 無號，OPA = 5、OPB = 9（成立寫 1，不成立寫 0） |  |
| `0x602` | RES+2 | `0x01` | CMP.LT 無號，OPA = 5、OPB = 9（成立寫 1，不成立寫 0） |  |
| `0x603` | RES+3 | `0x00` | CMP.GE 無號，OPA = 5、OPB = 9（成立寫 1，不成立寫 0） |  |
| `0x604` | RES+4 | `0x01` | CMP.EQ 無號，OPA = 9、OPB = 9（成立寫 1，不成立寫 0） |  |
| `0x605` | RES+5 | `0x00` | CMP.NE 無號，OPA = 9、OPB = 9（成立寫 1，不成立寫 0） |  |
| `0x606` | RES+6 | `0x00` | CMP.LT 無號，OPA = 9、OPB = 9（成立寫 1，不成立寫 0） |  |
| `0x607` | RES+7 | `0x01` | CMP.GE 無號，OPA = 9、OPB = 9（成立寫 1，不成立寫 0） |  |
| `0x608` | RES+8 | `0x00` | CMP.EQ 無號，OPA = 9、OPB = 5（成立寫 1，不成立寫 0） |  |
| `0x609` | RES+9 | `0x01` | CMP.NE 無號，OPA = 9、OPB = 5（成立寫 1，不成立寫 0） |  |
| `0x60A` | RES+10 | `0x00` | CMP.LT 無號，OPA = 9、OPB = 5（成立寫 1，不成立寫 0） |  |
| `0x60B` | RES+11 | `0x01` | CMP.GE 無號，OPA = 9、OPB = 5（成立寫 1，不成立寫 0） |  |
| `0x60C` | RES+12 | `0x01` | CMP.LT 有號，OPA = 0xffff、OPB = 1（成立寫 1，不成立寫 0） |  |
| `0x60D` | RES+13 | `0x00` | CMP.LT 無號，OPA = 0xffff、OPB = 1（成立寫 1，不成立寫 0） |  |
| `0x60E` | RES+14 | `0x00` | CMP.GE 有號，OPA = 0xffff、OPB = 1（成立寫 1，不成立寫 0） |  |
| `0x60F` | RES+15 | `0x01` | CMP.GE 無號，OPA = 0xffff、OPB = 1（成立寫 1，不成立寫 0） |  |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**結果不同時怎麼判讀**

- 無號比較時，以 (OPA, OPB) = (5, 9)、(9, 9)、(9, 5) 三組運算元，每種比較各有自己的結果樣式（EQ = 0 1 0、NE = 1 0 1、LT = 1 0 0、GE = 0 1 1）：把同一種比較的三格連起來看，就知道硬體實際做的是哪一種。
- OPA = 0xFFFF、OPB = 1：有號是 −1 < 1（LT 成立），無號是 65535 > 1（LT 不成立）。有號與無號的答案相同：signed_mode 不在工具放的位置。
- 全部是 0 或全部是 1：分支條件 ALU_CMP_TRUE 的編碼（0x5）與硬體不同。

## c05_alu_logic

ALU 的 AND／OR／XOR／NOT

**檢查的對象**：logic_op 的編碼數值（D1）

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x0F` | AND，OPA = 0x0F0F、OPB = 0x00FF → 0x000F（高 16 位元為 0）（位元組 0） |  |
| `0x601` | RES+1 | `0x00` | AND，OPA = 0x0F0F、OPB = 0x00FF → 0x000F（高 16 位元為 0）（位元組 1） |  |
| `0x602` | RES+2 | `0x00` | AND，OPA = 0x0F0F、OPB = 0x00FF → 0x000F（高 16 位元為 0）（位元組 2） |  |
| `0x603` | RES+3 | `0x00` | AND，OPA = 0x0F0F、OPB = 0x00FF → 0x000F（高 16 位元為 0）（位元組 3） |  |
| `0x604` | RES+4 | `0xFF` | OR，OPA = 0x0F0F、OPB = 0x00FF → 0x0FFF（高 16 位元為 0）（位元組 0） |  |
| `0x605` | RES+5 | `0x0F` | OR，OPA = 0x0F0F、OPB = 0x00FF → 0x0FFF（高 16 位元為 0）（位元組 1） |  |
| `0x606` | RES+6 | `0x00` | OR，OPA = 0x0F0F、OPB = 0x00FF → 0x0FFF（高 16 位元為 0）（位元組 2） |  |
| `0x607` | RES+7 | `0x00` | OR，OPA = 0x0F0F、OPB = 0x00FF → 0x0FFF（高 16 位元為 0）（位元組 3） |  |
| `0x608` | RES+8 | `0xF0` | XOR，OPA = 0x0F0F、OPB = 0x00FF → 0x0FF0（高 16 位元為 0）（位元組 0） |  |
| `0x609` | RES+9 | `0x0F` | XOR，OPA = 0x0F0F、OPB = 0x00FF → 0x0FF0（高 16 位元為 0）（位元組 1） |  |
| `0x60A` | RES+10 | `0x00` | XOR，OPA = 0x0F0F、OPB = 0x00FF → 0x0FF0（高 16 位元為 0）（位元組 2） |  |
| `0x60B` | RES+11 | `0x00` | XOR，OPA = 0x0F0F、OPB = 0x00FF → 0x0FF0（高 16 位元為 0）（位元組 3） |  |
| `0x60C` | RES+12 | `0xF0` | NOT，OPA = 0x0F0F、OPB = 0x00FF → 0xF0F0（高 16 位元為 0）（位元組 0） |  |
| `0x60D` | RES+13 | `0xF0` | NOT，OPA = 0x0F0F、OPB = 0x00FF → 0xF0F0（高 16 位元為 0）（位元組 1） |  |
| `0x60E` | RES+14 | `0x00` | NOT，OPA = 0x0F0F、OPB = 0x00FF → 0xF0F0（高 16 位元為 0）（位元組 2） |  |
| `0x60F` | RES+15 | `0x00` | NOT，OPA = 0x0F0F、OPB = 0x00FF → 0xF0F0（高 16 位元為 0）（位元組 3） |  |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**結果不同時怎麼判讀**

- 四種運算的結果都不同（AND = 0x000F、OR = 0x0FFF、XOR = 0x0FF0、NOT = 0xF0F0）：某一格出現的是另一種運算的值，就知道硬體用的是哪個編碼。

## c06_alu_shift

ALU 的 SHL／SHR

**檢查的對象**：方向位元 shift_dir（D6：0 是 SHL 還是 SHR？）；位數欄位；signed_mode；SHL 保留精確的 24 位元結果

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x02` | 0x8001 SHL 1 位，無號 → 0x00010002（位元組 0） |  |
| `0x601` | RES+1 | `0x00` | 0x8001 SHL 1 位，無號 → 0x00010002（位元組 1） |  |
| `0x602` | RES+2 | `0x01` | 0x8001 SHL 1 位，無號 → 0x00010002（位元組 2） |  |
| `0x603` | RES+3 | `0x00` | 0x8001 SHL 1 位，無號 → 0x00010002（位元組 3） |  |
| `0x604` | RES+4 | `0x00` | 0x8001 SHR 1 位，無號 → 0x00004000（位元組 0） |  |
| `0x605` | RES+5 | `0x40` | 0x8001 SHR 1 位，無號 → 0x00004000（位元組 1） |  |
| `0x606` | RES+6 | `0x00` | 0x8001 SHR 1 位，無號 → 0x00004000（位元組 2） |  |
| `0x607` | RES+7 | `0x00` | 0x8001 SHR 1 位，無號 → 0x00004000（位元組 3） |  |
| `0x608` | RES+8 | `0x02` | 0x8001 SHL 1 位，有號 → 0xFFFF0002（位元組 0） |  |
| `0x609` | RES+9 | `0x00` | 0x8001 SHL 1 位，有號 → 0xFFFF0002（位元組 1） |  |
| `0x60A` | RES+10 | `0xFF` | 0x8001 SHL 1 位，有號 → 0xFFFF0002（位元組 2） |  |
| `0x60B` | RES+11 | `0xFF` | 0x8001 SHL 1 位，有號 → 0xFFFF0002（位元組 3） |  |
| `0x60C` | RES+12 | `0x00` | 0x8001 SHR 1 位，有號 → 0xFFFFC000（位元組 0） |  |
| `0x60D` | RES+13 | `0xC0` | 0x8001 SHR 1 位，有號 → 0xFFFFC000（位元組 1） |  |
| `0x60E` | RES+14 | `0xFF` | 0x8001 SHR 1 位，有號 → 0xFFFFC000（位元組 2） |  |
| `0x60F` | RES+15 | `0xFF` | 0x8001 SHR 1 位，有號 → 0xFFFFC000（位元組 3） |  |
| `0x610` | RES+16 | `0x00` | 0x8001 SHL 8 位，無號 → 0x00800100（位元組 0） |  |
| `0x611` | RES+17 | `0x01` | 0x8001 SHL 8 位，無號 → 0x00800100（位元組 1） |  |
| `0x612` | RES+18 | `0x80` | 0x8001 SHL 8 位，無號 → 0x00800100（位元組 2） |  |
| `0x613` | RES+19 | `0x00` | 0x8001 SHL 8 位，無號 → 0x00800100（位元組 3） |  |
| `0x614` | RES+20 | `0x80` | 0x8001 SHR 8 位，無號 → 0x00000080（位元組 0） |  |
| `0x615` | RES+21 | `0x00` | 0x8001 SHR 8 位，無號 → 0x00000080（位元組 1） |  |
| `0x616` | RES+22 | `0x00` | 0x8001 SHR 8 位，無號 → 0x00000080（位元組 2） |  |
| `0x617` | RES+23 | `0x00` | 0x8001 SHR 8 位，無號 → 0x00000080（位元組 3） |  |
| `0x618` | RES+24 | `0x08` | 0x8001 SHL 3 位，無號 → 0x00040008（位元組 0） |  |
| `0x619` | RES+25 | `0x00` | 0x8001 SHL 3 位，無號 → 0x00040008（位元組 1） |  |
| `0x61A` | RES+26 | `0x04` | 0x8001 SHL 3 位，無號 → 0x00040008（位元組 2） |  |
| `0x61B` | RES+27 | `0x00` | 0x8001 SHL 3 位，無號 → 0x00040008（位元組 3） |  |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**結果不同時怎麼判讀**

- RES+0 起是 00 40 00 00、RES+4 起是 02 00 01 00（兩格對調）：shift_dir 的方向相反——硬體的 0 是 SHR（D6）。
- RES+0 起是 02 00 00 00：SHL 被截成 16 位元；規格要保留精確的 24 位元結果（這裡 bit 16 應為 1）。
- RES+16 起與 RES+24 起（位數 8 與 3）不對、而 RES+0 正確：位數欄位的讀法不同。

## c07_alu_status

ALU_STATUS 的各個旗標

**檢查的對象**：ALU_STATUS[0] 的哪一個位元是哪一個旗標（D4）；邏輯運算結果的 negative 取哪一位（D5）

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x02` | AND，OPA = 0x0000、OPB = 0x0000：zero（結果為 0） | 各旗標的「位置」是假設（規格只列旗標、沒有給位元）：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 |
| `0x601` | RES+1 | `0x04` | OR，OPA = 0x8000、OPB = 0x0000：negative（邏輯結果的 bit 15） | 各旗標的「位置」是假設（規格只列旗標、沒有給位元）：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 |
| `0x602` | RES+2 | `0x40` | SHL 1 unsigned，OPA = 0x8000、OPB = 0x0000：ov16（0x10000 放不進 16 位元） | 各旗標的「位置」是假設（規格只列旗標、沒有給位元）：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 |
| `0x603` | RES+3 | `0x48` | ADD unsigned，OPA = 0xFFFF、OPB = 0x0001：carry_u + ov16（0x10000：16 位元的進位） | 各旗標的「位置」是假設（規格只列旗標、沒有給位元）：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 |
| `0x604` | RES+4 | `0x54` | SUB unsigned，OPA = 0x0003、OPB = 0x0007：negative + borrow_u + ov16（3 − 7 發生借位） | 各旗標的「位置」是假設（規格只列旗標、沒有給位元）：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 |
| `0x605` | RES+5 | `0x60` | ADD signed，OPA = 0x7FFF、OPB = 0x0001：overflow_s + ov16（32767 + 1 超出有號 16 位元的範圍） | 各旗標的「位置」是假設（規格只列旗標、沒有給位元）：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 |
| `0x606` | RES+6 | `0x00` | ADD unsigned，OPA = 0x0001、OPB = 0x0001：沒有旗標（結果是 2，沒有特別的狀況） | 各旗標的「位置」是假設（規格只列旗標、沒有給位元）：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 |
| `0x607` | RES+7 | `0x01` | CMP.EQ unsigned，OPA = 0x0005、OPB = 0x0005：cmp_true（相等；前一道 ADD 的旗標保留——比較只改 cmp_true） | 各旗標的「位置」是假設（規格只列旗標、沒有給位元）：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 |
| `0x608` | RES+8 | `0x01` | ADD unsigned，OPA = 0x0001、OPB = 0x0001：cmp_true（cmp_true 保持到下一次比較） | 各旗標的「位置」是假設（規格只列旗標、沒有給位元）：bit0 cmp_true、1 zero、2 negative、3 carry_u、4 borrow_u、5 overflow_s、6 ov16 |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**結果不同時怎麼判讀**

- RES+0、RES+1、RES+2 各只有一個旗標（zero、negative、ov16）：那一格被設起來的位元，就是硬體放那個旗標的位置。
- carry_u＝RES+3 扣掉 ov16 的位元；overflow_s＝RES+5 扣掉 ov16；borrow_u＝RES+4 扣掉 negative 與 ov16。
- RES+8 沒有 cmp_true 的位元：硬體在其他運算時會清掉 cmp_true（工具的假設是保持到下一次比較）。

## c08_cascade

CASCADE_BEGIN／CASCADE_END

**檢查的對象**：MainBranchDesc 的型別 1 與 2；累進的區塊：選擇值 k 會執行區塊 0 到區塊 k

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0xB0` | 區塊 0 |  |
| `0x601` | RES+1 | `0xB1` | 區塊 1（選到的最後一個區塊） |  |
| `0x603` | RES+3 | `0xEE` | 離開的目標 |  |
| `0x602` | RES+2 | **不可被寫入** | 區塊 2 在選到的區塊之後：這裡被寫入表示 cascade 沒有在選擇值的地方結束 | |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**結果不同時怎麼判讀**

- 只有 RES+1 被寫入（RES+0 沒有）：硬體只跳到區塊 k——那是 switch，不是規格的累進式 cascade。

## c09_csr_read

EXEC_CSR READ8／READ_BUF 與 MAIN_CSR_DATA_BUF

**檢查的對象**：CsrOneByteDesc 的 READ8 與目的指標；CsrMultiByteDesc（頁、長度、指標）；從 MAIN_CSR_DATA_BUF 的 MOV

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x3C` | READ8 把 RES+16 讀進 MAIN_CSR_DATA_BUF[2]，再寫出來 | 結果 CSR 必須是「寫什麼讀回什麼」的一般暫存器 |
| `0x604` | RES+4 | `0xA1` | READ_BUF 把 RES+20..22 讀進 MAIN_CSR_DATA_BUF[5..7]，再寫出來（位元組 0） | 結果 CSR 必須是「寫什麼讀回什麼」的一般暫存器 |
| `0x605` | RES+5 | `0xA2` | READ_BUF 把 RES+20..22 讀進 MAIN_CSR_DATA_BUF[5..7]，再寫出來（位元組 1） | 結果 CSR 必須是「寫什麼讀回什麼」的一般暫存器 |
| `0x606` | RES+6 | `0xA3` | READ_BUF 把 RES+20..22 讀進 MAIN_CSR_DATA_BUF[5..7]，再寫出來（位元組 2） | 結果 CSR 必須是「寫什麼讀回什麼」的一般暫存器 |
| `0x610` | RES+16 | `0x3C` | 準備被讀回的值 |  |
| `0x614` | RES+20 | `0xA1` | 準備被讀回的三個值（位元組 0） |  |
| `0x615` | RES+21 | `0xA2` | 準備被讀回的三個值（位元組 1） |  |
| `0x616` | RES+22 | `0xA3` | 準備被讀回的三個值（位元組 2） |  |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**結果不同時怎麼判讀**

- RES+4..6 的順序不同：多位元組讀取不是依位址遞增、照緩衝區的順序放。

## c10_peri_xfer_pub

Peri：EXEC_XFER、EXEC_PUB，以及 Main 取用結果

**檢查的對象**：DeviceMap 的排列與索引（D3）；XferOneByte／XferMultiByte 描述子；PeriPublishDesc；PERI_RESULT_META；結果槽

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x03` | PERI_RESULT_META[0]＝這次發佈的 service id | PERI_RESULT_META 的位元組排列是假設（位元組 0 為 service id） |
| `0x604` | RES+4 | `0xC3` | 發佈出來的 PERI_RX_BUF[0..4]：先是單位元組讀取，再是四位元組讀取（位元組 0） |  |
| `0x605` | RES+5 | `0xD0` | 發佈出來的 PERI_RX_BUF[0..4]：先是單位元組讀取，再是四位元組讀取（位元組 1） |  |
| `0x606` | RES+6 | `0xD1` | 發佈出來的 PERI_RX_BUF[0..4]：先是單位元組讀取，再是四位元組讀取（位元組 2） |  |
| `0x607` | RES+7 | `0xD2` | 發佈出來的 PERI_RX_BUF[0..4]：先是單位元組讀取，再是四位元組讀取（位元組 3） |  |
| `0x608` | RES+8 | `0xD3` | 發佈出來的 PERI_RX_BUF[0..4]：先是單位元組讀取，再是四位元組讀取（位元組 4） |  |
| `0x620` | RES+32 | `0x9E` | Peri 的 EXEC_CSR WRITE8 |  |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**I2C 匯流排上應該看到的交易（依序）**

| 方向 | 裝置位址 | 暫存器 | 資料 |
|---|---|---|---|
| 寫入 | `0x2A` | 暫存器 0x30 | 資料 0x77 |
| 讀取 | `0x2A` | 暫存器 0x10 | 1 個位元組，裝置回 0xC3 |
| 讀取 | `0x2A` | 暫存器 0x20 | 4 個位元組，裝置回 0xD0 0xD1 0xD2 0xD3 |

**結果不同時怎麼判讀**

- 測試平台需要一個位址為 0x2A 的 I2C 裝置：暫存器 0x10 讀到 0xC3，暫存器 0x20..0x23 讀到 0xD0 0xD1 0xD2 0xD3；寫入暫存器 0x30 要回 ACK。
- 交易是對位址 0x55 發出的：用的是 DeviceMap 第 0 筆而不是第 1 筆。完全是另一個位址：項目的位元排列不同（D3）。
- RES+4 起是 D0 D1 D2 D3 C3 或整個位移：讀取的目的指標不在工具放的位置。
- 這支測試 Main 與 Peri 同時執行，是十支裡唯一用到 Peri、I2C 與結果槽的。

---

# 第 2 層：行為

同一支程式，兩邊做的事、做的**順序**是否相同？除了最後的值，還要看「哪個位址先被寫入」與 I2C 匯流排上的交易。只比順序，不比時間。

標示「用到板子自己的 CSR」的測試（`b01`、`b02`、`b05`）會寫計時器、poll、done／error 這些暫存器；規格沒有給它們的位址與位元，
**請先把 `conf_board.inc` 改成實際的數值再重新產生**（目前是範例值）。

## b01_timers

Main 的計時器與 Peri 的計時器同時計時

**檢查的對象**：MAIN_TIMER_DONE 與 IF_TIMER_DONE 是兩顆計時器還是一顆（Q1）；啟動會清除完成旗標；計時器可以再次啟動

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x11` | Main 第一次短計時結束 | 用到板子自己的 CSR（計時器、poll、done／error）：先把 conf_board.inc 改成實際的位址與位元 |
| `0x601` | RES+1 | `0x12` | Main 第二次短計時結束 | 用到板子自己的 CSR（計時器、poll、done／error）：先把 conf_board.inc 改成實際的位址與位元 |
| `0x608` | RES+8 | `0x21` | Peri 兩次長計時結束 | 用到板子自己的 CSR（計時器、poll、done／error）：先把 conf_board.inc 改成實際的位址與位元 |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**寫入的先後順序（必須成立）**

| 先 | 後 | 原因 |
|---|---|---|
| `RES+0` | `RES+1` | Main 的兩次短計時依序完成 |
| `RES+1` | `RES+8` | 兩顆獨立的計時器：Peri 的長計時不受 Main 啟動自己的計時器影響，所以 Peri 的標記在 Main 兩次短計時之後才出現 |
| `RES+8` | `RES_DONE` | Main 之後又等了三次長計時，一定比 Peri 晚結束 |

**結果不同時怎麼判讀**

- **RES+8 比 RES+1（甚至 RES+0）早出現：硬體只有一顆計時器**——Main 啟動計時器時，把 Peri 正在等的那一次蓋掉了，Peri 跟著 Main 的短計時一起「完成」。這是 Q1 的答案。
- RES+8 一直沒有出現：一顆共用的計時器，而且完成旗標只有啟動它的一方看得到（或被另一方的啟動清掉）。
- RES+0 與 RES+1 幾乎同時出現：再次啟動時完成旗標沒有被清除（工具的假設是啟動會清除）。
- 時間上的參考（不列入判定）：兩顆獨立時，RES+8 約在開始後 2 × 65000 個 tick 出現，RES+0 約在 2000 個 tick 之後。

## b02_poll_ready

Peri 輪詢狀態暫存器直到 ready 位元，再讀資料

**檢查的對象**：Poll 單元：遮罩、清除、arm、在成功讀進 PERI_RX_BUF[0] 時評估；BR.IF_READY

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0xE0` | ready 之後讀到並發佈的兩個位元組（位元組 0） | 用到板子自己的 CSR（計時器、poll、done／error）：先把 conf_board.inc 改成實際的位址與位元 |
| `0x601` | RES+1 | `0xE1` | ready 之後讀到並發佈的兩個位元組（位元組 1） | 用到板子自己的 CSR（計時器、poll、done／error）：先把 conf_board.inc 改成實際的位址與位元 |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**I2C 匯流排上應該看到的交易（依序）**

| 方向 | 裝置位址 | 暫存器 | 資料 |
|---|---|---|---|
| 讀取 | `0x2B` | 暫存器 0x05（狀態） | 1 個位元組，裝置回 0x00（尚未就緒） |
| 讀取 | `0x2B` | 暫存器 0x05（狀態） | 1 個位元組，裝置回 0x00 |
| 讀取 | `0x2B` | 暫存器 0x05（狀態） | 1 個位元組，裝置回 0x01（就緒） |
| 讀取 | `0x2B` | 暫存器 0x40 | 2 個位元組，裝置回 0xE0 0xE1 |

**結果不同時怎麼判讀**

- 測試平台需要位址 0x2B 的 I2C 裝置：暫存器 0x05 前兩次讀到 0x00、第三次起讀到 0x01；暫存器 0x40、0x41 讀到 0xE0、0xE1。
- 狀態只被讀了一次或兩次就去讀資料：Poll 的評估不是「讀到的位元組 AND 遮罩」，或 ready 在 arm 時就被設起來。
- 狀態一直被讀、永遠不讀資料：遮罩或 arm 沒有生效（POLL_MASK／POLL_CTRL 的位址或位元與 conf_board.inc 不符），或評估的不是 PERI_RX_BUF[0]。

## b03_slot_overrun

Main 取用之前 Peri 發佈了兩次：保留第一筆

**檢查的對象**：結果槽：保留舊的、丟掉新的（Keep Old Drop New）；overrun 旗標；PERI_RESULT_OVERRUN

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x01` | 結果槽裡的 service id：第一次發佈的（1） | PERI_RESULT_META 的位元組排列是假設（位元組 0 為 service id） |
| `0x601` | RES+1 | `0xA1` | 結果槽裡的資料：第一次發佈的 |  |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**I2C 匯流排上應該看到的交易（依序）**

| 方向 | 裝置位址 | 暫存器 | 資料 |
|---|---|---|---|
| 讀取 | `0x2C` | 暫存器 0x01 | 1 個位元組，裝置回 0xA1 |
| 讀取 | `0x2C` | 暫存器 0x02 | 1 個位元組，裝置回 0xA2 |

**結果不同時怎麼判讀**

- 測試平台需要位址 0x2C 的 I2C 裝置：暫存器 0x01 讀到 0xA1，暫存器 0x02 讀到 0xA2。
- RES+0 = 2、RES+1 = 0xA2：硬體用新的結果蓋掉舊的（規格是保留舊的、丟掉新的）。
- RES_DONE 一直沒有出現：第二次發佈沒有設 overrun，Main 一直在等 PERI_RESULT_OVERRUN。

## b04_nack_error

沒有裝置回應的傳輸會設起 Peri 的 error 旗標

**檢查的對象**：NACK → error 旗標（保持住）；BR.IF_ERROR／IF_NO_ERROR；NACK 之後 sequencer 繼續執行

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x0C` | 傳輸之前：error 旗標是清的 |  |
| `0x601` | RES+1 | `0xE1` | 沒有回應的傳輸之後：error 旗標被設起 |  |
| `0x602` | RES+2 | `0xE2` | 再查一次：error 旗標保持住 |  |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**寫入的先後順序（必須成立）**

| 先 | 後 | 原因 |
|---|---|---|
| `RES+0` | `RES+1` | 依程式順序 |
| `RES+1` | `RES+2` | 依程式順序 |

**I2C 匯流排上應該看到的交易（依序）**

| 方向 | 裝置位址 | 暫存器 | 資料 |
|---|---|---|---|
| 讀取 | `0x2D` | 暫存器 0x00 | 沒有裝置回應（NACK） |

**結果不同時怎麼判讀**

- 測試平台在位址 0x2D **不可以**有裝置。這支只有 Peri 程式，完成標記由 Peri 寫入。
- RES+1 = 0x0E：NACK 沒有設 error 旗標。RES+1 沒有被寫入、程式停住：NACK 被當成 fault，sequencer 停了（工具的假設是照常往下執行）。
- RES+2 = 0x02：error 旗標沒有保持住（被分支讀取清掉）。

## b05_done_error

Peri 先回報一次成功的 service、再回報一次失敗的；Main 以 PERI_NO_ERROR／PERI_ERROR 分支

**檢查的對象**：PERI_NO_ERROR＝done 且沒有 error，PERI_ERROR＝done 且有 error；service reset 會清掉兩個旗標

| 位址 | 位移 | 預期值 | 意義 | 取決於 |
|---|---|---|---|---|
| `0x600` | RES+0 | `0x0A` | 第一個 service：done 且沒有 error → PERI_NO_ERROR | 用到板子自己的 CSR（計時器、poll、done／error）：先把 conf_board.inc 改成實際的位址與位元 |
| `0x601` | RES+1 | `0xEB` | 第二個 service：done 且有 error → PERI_ERROR | 用到板子自己的 CSR（計時器、poll、done／error）：先把 conf_board.inc 改成實際的位址與位元 |
| `0x67F` | RES_DONE | `0xA5` | 測試的最後一次寫入 | |

**寫入的先後順序（必須成立）**

| 先 | 後 | 原因 |
|---|---|---|
| `RES+0` | `RES+1` | 先成功的那一次，再失敗的那一次 |

**I2C 匯流排上應該看到的交易（依序）**

| 方向 | 裝置位址 | 暫存器 | 資料 |
|---|---|---|---|
| 讀取 | `0x2B` | 暫存器 0x40 | 1 個位元組，裝置回 0xE0 |
| 讀取 | `0x2D` | 暫存器 0x00 | 沒有裝置回應（NACK） |

**結果不同時怎麼判讀**

- 測試平台需要位址 0x2B 的 I2C 裝置（暫存器 0x40 讀到 0xE0），而位址 0x2D **不可以**有裝置。
- RES+0 = 0xEE：第一個 service 就被當成有 error（error 旗標在 reset 後不是清的，或 PERI_ERROR 不看 done）。
- RES+1 一直沒有出現：NACK 沒有設 error，或第二次的 done 沒有生效。RES+0 一直沒有出現：PERI_CTRL 的 done 位元與 conf_board.inc 不符。

