# 第 3 層：時間——量測程式與模型的預測值

由 `make_timing.py` 產生（請改產生器，不要直接改這份）。結果 CSR 的基底：`RES = 0x600`。

**這一層沒有「正確答案」**：模型裡的週期數是等 RTL 提供的規劃值。下表是模型以初始（規劃）參數的預測，用來和硬體量到的數字對照，不是合格標準。

## 怎麼量

每個量測項目是三個標記之間的兩段：`標記 k`、N1 道指令、`標記 k+1`、N2 道指令、`標記 k+2`。標記是對 `RES + k` 的 CSR 寫入。

```
一道指令的時間 = （第二段 − 第一段） ÷ （N2 − N1）
```

兩段相減會消掉標記本身的時間，所以不必知道 CSR 寫入在指令的哪個時刻出現在匯流排上，只要兩個標記用同樣的方式觀察。

一道指令的時間＝**取指**＋**執行**。取指的部分取決於硬體怎麼取指（`docs/SIM_VS_SPEC.md` 1.2），所以下表對三種情形各給一欄：

- `eeprom`：從 I2C EEPROM 取指（出廠預設）
- `frontend`：從程式 SRAM 取指（規格 v31）
- `no_fetch`：不計取指（只有執行週期）

「只有執行」那一欄就是規格 14.3／14.4 的規劃公式；硬體量到的「一道指令的時間」減掉取指的部分，就是該填進參數的執行週期。

## t01_alu

ALU 運算

| 項目 | 標記 | N1／N2 | 一道指令：`eeprom` | 一道指令：`frontend` | 一道指令：`no_fetch` | 執行的公式 | 相關參數 |
|---|---|---|---|---|---|---|---|
| ALU ADD（ADD／SUB／CMP／LOGIC／SHIFT 同一類）（`alu`，Main） | 0 → 1 → 2 | 4／12 | 6081 | 5 | 1 | latency.alu | latency.alu |
| ALU MUL（`mul`，Main） | 3 → 4 → 5 | 4／12 | 6082 | 6 | 2 | latency.mul | latency.mul |

兩段的預測長度（週期）：

| 項目 | 段 | `eeprom` | `frontend` | `no_fetch` |
|---|---|---|---|---|
| `alu` | 第一段：標記 0 → 1 | 31621 | 27 | 7 |
| `alu` | 第二段：標記 1 → 2 | 80269 | 67 | 15 |
| `mul` | 第一段：標記 3 → 4 | 31625 | 31 | 11 |
| `mul` | 第二段：標記 4 → 5 | 80281 | 79 | 27 |

## t02_mov

MOV 1 個與 8 個位元組

| 項目 | 標記 | N1／N2 | 一道指令：`eeprom` | 一道指令：`frontend` | 一道指令：`no_fetch` | 執行的公式 | 相關參數 |
|---|---|---|---|---|---|---|---|
| MOV 1 個位元組（`mov1`，Main） | 0 → 1 → 2 | 4／12 | 7904 | 6 | 2 | latency.mov_setup + 1 × latency.mov_byte | latency.mov_setup, latency.mov_byte |
| MOV 8 個位元組（`mov8`，Main） | 3 → 4 → 5 | 4／12 | 7911 | 13 | 9 | latency.mov_setup + 8 × latency.mov_byte | latency.mov_setup, latency.mov_byte |

兩段的預測長度（週期）：

| 項目 | 段 | `eeprom` | `frontend` | `no_fetch` |
|---|---|---|---|---|
| `mov1` | 第一段：標記 0 → 1 | 38913 | 31 | 11 |
| `mov1` | 第二段：標記 1 → 2 | 102145 | 79 | 27 |
| `mov8` | 第一段：標記 3 → 4 | 38941 | 59 | 39 |
| `mov8` | 第二段：標記 4 → 5 | 102229 | 163 | 111 |

## t03_branch

分支：不成立與成立

| 項目 | 標記 | N1／N2 | 一道指令：`eeprom` | 一道指令：`frontend` | 一道指令：`no_fetch` | 執行的公式 | 相關參數 |
|---|---|---|---|---|---|---|---|
| BR.COND 不成立（`br_not_taken`，Main） | 0 → 1 → 2 | 4／12 | 6688 | 5 | 1 | latency.br_cond | latency.br_cond |
| BR.COND 成立（JMP 到下一行）（`br_taken`，Main） | 3 → 4 → 5 | 4／12 | 6688 | 5 | 1 | latency.br_cond | latency.br_cond |

兩段的預測長度（週期）：

| 項目 | 段 | `eeprom` | `frontend` | `no_fetch` |
|---|---|---|---|---|
| `br_not_taken` | 第一段：標記 0 → 1 | 34049 | 27 | 7 |
| `br_not_taken` | 第二段：標記 1 → 2 | 87553 | 67 | 15 |
| `br_taken` | 第一段：標記 3 → 4 | 34049 | 27 | 7 |
| `br_taken` | 第二段：標記 4 → 5 | 87553 | 67 | 15 |

## t04_csr

EXEC_CSR 各種形式

| 項目 | 標記 | N1／N2 | 一道指令：`eeprom` | 一道指令：`frontend` | 一道指令：`no_fetch` | 執行的公式 | 相關參數 |
|---|---|---|---|---|---|---|---|
| EXEC_CSR WRITE8（`csr_write8`，Main） | 0 → 1 → 2 | 4／12 | 7297 | 7 | 3 | 1 + latency.csr_access | latency.csr_access（或 csr.access_main） |
| EXEC_CSR SET8（讀改寫）（`csr_set8`，Main） | 3 → 4 → 5 | 4／12 | 7299 | 9 | 5 | 1 + 2 × latency.csr_access | latency.csr_access |
| EXEC_CSR READ8（`csr_read8`，Main） | 6 → 7 → 8 | 4／12 | 7297 | 7 | 3 | 1 + latency.csr_access | latency.csr_access |
| EXEC_CSR WRITE_SP 1 個位元組（`csr_write_sp1`，Main） | 9 → 10 → 11 | 4／12 | 7298 | 8 | 4 | 1 + 1 × latency.csr_byte + latency.csr_done | latency.csr_byte, latency.csr_done |
| EXEC_CSR WRITE_SP 8 個位元組（`csr_write_sp8`，Main） | 12 → 13 → 14 | 4／12 | 7312 | 22 | 18 | 1 + 8 × latency.csr_byte + latency.csr_done | latency.csr_byte, latency.csr_done |

兩段的預測長度（週期）：

| 項目 | 段 | `eeprom` | `frontend` | `no_fetch` |
|---|---|---|---|---|
| `csr_write8` | 第一段：標記 0 → 1 | 36485 | 35 | 15 |
| `csr_write8` | 第二段：標記 1 → 2 | 94861 | 91 | 39 |
| `csr_set8` | 第一段：標記 3 → 4 | 36493 | 43 | 23 |
| `csr_set8` | 第二段：標記 4 → 5 | 94885 | 115 | 63 |
| `csr_read8` | 第一段：標記 6 → 7 | 36485 | 35 | 15 |
| `csr_read8` | 第二段：標記 7 → 8 | 94861 | 91 | 39 |
| `csr_write_sp1` | 第一段：標記 9 → 10 | 36489 | 39 | 19 |
| `csr_write_sp1` | 第二段：標記 10 → 11 | 94873 | 103 | 51 |
| `csr_write_sp8` | 第一段：標記 12 → 13 | 36545 | 95 | 75 |
| `csr_write_sp8` | 第二段：標記 13 → 14 | 95041 | 271 | 219 |

## t05_peri

Peri：EXEC_CSR、分支、EXEC_PUB

| 項目 | 標記 | N1／N2 | 一道指令：`eeprom` | 一道指令：`frontend` | 一道指令：`no_fetch` | 執行的公式 | 相關參數 |
|---|---|---|---|---|---|---|---|
| Peri EXEC_CSR WRITE8（`peri_csr_write8`，Peri） | 16 → 17 → 18 | 4／12 | 7297 | 7 | 3 | 1 + latency.csr_access | latency.csr_access（或 csr.access_peri） |
| Peri BR 不成立（`peri_br_not_taken`，Peri） | 19 → 20 → 21 | 4／12 | 6688 | 5 | 1 | latency.br_cond | latency.br_cond |
| EXEC_PUB 1 個位元組（`pub1`，Peri） | 22 → 23 → 24 | 2／6 | 6690 | 7 | 3 | latency.pub_setup + 1 × latency.pub_byte | latency.pub_setup, latency.pub_byte, slot.copy_extra |
| EXEC_PUB 8 個位元組（`pub8`，Peri） | 25 → 26 → 27 | 2／6 | 6697 | 14 | 10 | latency.pub_setup + 8 × latency.pub_byte | latency.pub_setup, latency.pub_byte, slot.copy_extra |

兩段的預測長度（週期）：

| 項目 | 段 | `eeprom` | `frontend` | `no_fetch` |
|---|---|---|---|---|
| `peri_csr_write8` | 第一段：標記 16 → 17 | 36485 | 35 | 15 |
| `peri_csr_write8` | 第二段：標記 17 → 18 | 94861 | 91 | 39 |
| `peri_br_not_taken` | 第一段：標記 19 → 20 | 34049 | 27 | 7 |
| `peri_br_not_taken` | 第二段：標記 20 → 21 | 87553 | 67 | 15 |
| `pub1` | 第一段：標記 22 → 23 | 20677 | 21 | 9 |
| `pub1` | 第二段：標記 23 → 24 | 47437 | 49 | 21 |
| `pub8` | 第一段：標記 25 → 26 | 20691 | 35 | 23 |
| `pub8` | 第二段：標記 26 → 27 | 47479 | 91 | 63 |

## t06_xfer

EXEC_XFER：I2C 交易

| 項目 | 標記 | N1／N2 | 一道指令：`eeprom` | 一道指令：`frontend` | 一道指令：`no_fetch` | 執行的公式 | 相關參數 |
|---|---|---|---|---|---|---|---|
| EXEC_XFER 讀 1 個位元組（1 個位址位元組）（`xfer_read1`，Peri） | 16 → 17 → 18 | 2／4 | 12766 | 2436 | 2432 | latency.xfer_issue ＋ 4 個位元組階段 × 9 個 SCL 脈衝 | latency.xfer_issue, clock.i2c_hz, i2ch.overhead, i2c.ack_delay, i2c.stretch, i2c.turnaround |
| EXEC_XFER 讀 4 個位元組（1 個位址位元組）（`xfer_read4`，Peri） | 19 → 20 → 21 | 2／4 | 14589 | 4259 | 4255 | latency.xfer_issue ＋ 7 個位元組階段 × 9 個 SCL 脈衝 | latency.xfer_issue, clock.i2c_hz, i2ch.overhead, i2c.ack_delay, i2c.stretch, i2c.turnaround |
| EXEC_XFER 寫 1 個位元組（1 個位址位元組）（`xfer_write1`，Peri） | 22 → 23 → 24 | 2／4 | 12159 | 1829 | 1825 | latency.xfer_issue ＋ 3 個位元組階段 × 9 個 SCL 脈衝 | latency.xfer_issue, clock.i2c_hz, i2ch.overhead, i2c.ack_delay, i2c.stretch, i2c.turnaround |

兩段的預測長度（週期）：

| 項目 | 段 | `eeprom` | `frontend` | `no_fetch` |
|---|---|---|---|---|
| `xfer_read1` | 第一段：標記 16 → 17 | 32829 | 4879 | 4867 |
| `xfer_read1` | 第二段：標記 17 → 18 | 58361 | 9751 | 9731 |
| `xfer_read4` | 第一段：標記 19 → 20 | 36475 | 8525 | 8513 |
| `xfer_read4` | 第二段：標記 20 → 21 | 65653 | 17043 | 17023 |
| `xfer_write1` | 第一段：標記 22 → 23 | 31615 | 3665 | 3653 |
| `xfer_write1` | 第二段：標記 23 → 24 | 55933 | 7323 | 7303 |

## t07_timers

Main 與 Peri 計時器的 tick　（用到板子自己的 CSR：先改 `conf_board.inc`）

**Main 計時器**：標記 0（啟動，週期 20000 tick）→ 1（完成）；標記 2（啟動，週期 60000 tick）→ 3（完成）。

```
每個 tick 的週期數 = （（標記 2 → 3） − （標記 0 → 1）） ÷ （60000 − 20000）
```

| | `eeprom` | `frontend` | `no_fetch` |
|---|---|---|---|
| 週期 20000 tick 的等待（週期數） | 69905 | 40011 | 40006 |
| 週期 60000 tick 的等待（週期數） | 150149 | 120011 | 120004 |
| 每個 tick 的週期數 | 2.006 | 2.000 | 2.000 |

模型的值是 2（13.5 MHz＝27 MHz ÷ 2）。量到的等待會多出最多一圈輪詢迴圈的時間，所以取指越慢誤差越大；兩段相減再除以 40000 之後誤差很小。相關參數：`clock.timer_hz`、`timer.prescaler`、`timer.count`、`timer.phase`、`timer.main.*`。

**Peri 計時器**：標記 16（啟動，週期 20000 tick）→ 17（完成）；標記 18（啟動，週期 60000 tick）→ 19（完成）。

```
每個 tick 的週期數 = （（標記 18 → 19） − （標記 16 → 17）） ÷ （60000 − 20000）
```

| | `eeprom` | `frontend` | `no_fetch` |
|---|---|---|---|
| 週期 20000 tick 的等待（週期數） | 69298 | 40023 | 40003 |
| 週期 60000 tick 的等待（週期數） | 148935 | 120021 | 120005 |
| 每個 tick 的週期數 | 1.991 | 2.000 | 2.000 |

模型的值是 2（13.5 MHz＝27 MHz ÷ 2）。量到的等待會多出最多一圈輪詢迴圈的時間，所以取指越慢誤差越大；兩段相減再除以 40000 之後誤差很小。相關參數：`clock.timer_hz`、`timer.prescaler`、`timer.count`、`timer.phase`、`timer.peri.*`。

## t08_csr_contended

Peri 不停寫 CSR 時，Main 的 EXEC_CSR

| 項目 | 標記 | N1／N2 | 一道指令：`eeprom` | 一道指令：`frontend` | 一道指令：`no_fetch` | 執行的公式 | 相關參數 |
|---|---|---|---|---|---|---|---|
| EXEC_CSR WRITE8（Peri 同時不停寫 CSR）（`csr_write8_contended`，Main） | 0 → 1 → 2 | 4／12 | 14208.6 | 7 | 4 | 與 t04 的 csr_write8 比較：多出來的就是等 CSR 仲裁器（以及等共用匯流排取指）的時間 | csr.arbiter_policy, csr.arbiter_penalty；取指方式 |

兩段的預測長度（週期）：

| 項目 | 段 | `eeprom` | `frontend` | `no_fetch` |
|---|---|---|---|---|
| `csr_write8_contended` | 第一段：標記 0 → 1 | 71726 | 35 | 20 |
| `csr_write8_contended` | 第二段：標記 1 → 2 | 185395 | 91 | 52 |

