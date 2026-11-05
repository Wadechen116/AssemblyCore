# EEPROM：從燒錄檔解碼執行

> **一句話**：硬體的 Main／Peri Decoder 在執行時，從 I2C EEPROM 讀出每一條指令的 opcode、描述子（Description Table）與常數（Const Pool），拼成完整的指令。模擬器照這個方式做：**它只執行 EEPROM 裡燒錄的 byte**，並把 Decoder 解出來的組合語言顯示出來，和原始碼對照。

## 1. 流程

```text
寫組合語言 (.asm)
   │  asmc --eeprom
   ▼
EEPROM 映像  PREFIX.eeprom.bin（燒錄用）＋ .eeprom.hex（Intel HEX）＋ .eeprom.json（各區域位址）
   │  燒錄到 EEPROM（或直接交給模擬器）
   ▼
模擬器／硬體：Decoder 從 EEPROM 取 opcode → 查描述表 → 取常數 → 執行
   │
   ▼
Debug：Decoder 面板／asmsim --decoded 列出「EEPROM 解出的指令」，和原始碼比對，不一致標紅
```

## 2. EEPROM 的六個區域

| 區域 | 內容 | 起始位址暫存器（模擬器訊號） | profile 設定 |
|---|---|---|---|
| `main.code` | Main 的程式碼（每條指令 1 byte opcode，加上內嵌 byte） | `main_code_addr` | `eeprom_main_code_addr` |
| `main.desc` | Main 的五個描述表（MOV、BR、CSR 單 byte、CSR 多 byte、ALU） | `main_desc_addr` | `eeprom_main_desc_addr` |
| `main.const` | Main 的 Const Pool | `main_const_addr` | `eeprom_main_const_addr` |
| `peri.code` | Peri 的程式碼 | `peri_code_addr` | `eeprom_peri_code_addr` |
| `peri.desc` | Peri 的五個描述表（XFER 單 byte、XFER 多 byte、PUB、BR、CSR 單 byte） | `peri_desc_addr` | `eeprom_peri_desc_addr` |
| `peri.devmap` | Peri 的 DeviceMap | `peri_devmap_addr` | `eeprom_peri_devmap_addr` |

- profile 的位址設 `-1`（預設）＝緊接在前一個區域之後。區域重疊或放不下 EEPROM 時，組譯器會拒絕並說明原因。
- 每個區域是該 domain 邏輯映像的一段，所以描述表的位移（ImageLayout）意義不變：邏輯位移 `o` 在 EEPROM 的位址是 `區域起點 + (o − 區域在映像裡的起點)`。
- 其他設定：`eeprom_capacity`（預設 65536 byte）、`eeprom_i2c_address`（0x50）、`eeprom_address_bytes`（2）、`eeprom_erased`（未燒錄的 byte 讀到 0xFF）。
- 這些都是**暫定值**，profile 群組 `eeprom`，等板子與 RTL 確認。

## 3. 暫存器

六個起始位址暫存器在模擬器裡是訊號，開機值等於 `.eeprom.json` 裡的位址。程式或環境可以改它們：

- 環境檔把暫存器綁到 CSR：`[csr MAIN_DESC_ADDR]`、`kind = value`、`bind = main_desc_addr`、`bytes = 2`，程式 `EXEC_CSR` 寫這個 CSR 就改了位址。
- `[inject]` 也可以直接設：`at 0 = signal main_const_addr=0x100`。
- **位址設錯，Decoder 就讀到別的 byte**，和硬體一樣：可能解出別的指令、讀到錯的常數，或發生錯誤。

## 4. EEPROM 也在 I2C 匯流排上

Peri 的 `EXEC_XFER` 讀寫 EEPROM 的 I2C 位址（預設 0x50）時，讀到的就是燒錄的 byte；**寫入會改變 EEPROM**，之後 Decoder 取到的也是新內容。每個快照（倒帶）保有自己的 EEPROM 內容。環境檔若在同一個位址定義了 `[device]`，以環境檔為準。

## 5. 命令列

```bash
asmc prog.asm --eeprom                                # 另外寫出 prog.eeprom.bin / .eeprom.hex / .eeprom.json
asmdis --main --eeprom prog.eeprom.bin               # 從燒錄檔反組譯（--check 再組譯一次比對）
asmsim prog.asm --eeprom prog.eeprom.bin --decoded   # 用燒錄檔執行，原始碼只用來對照；--decoded 列出解碼結果
asmsim --eeprom prog.eeprom.bin --cycles 100000      # 只有燒錄檔、沒有原始碼也能跑
```

`--decoded` 的每一行：PC、EEPROM 位址、byte、解出的指令、原始碼；和組譯器產生的不一樣時前面標 `!!`，下一行寫出原本應該是什麼。MOV 讀 Const Pool 時，後面附上實際讀到的常數 byte（`; const 34 12`）。

## 6. IDE

- **建置**：成功時除了 `.bin`，也寫出 `.eeprom.bin`／`.eeprom.hex`／`.eeprom.json`（照專案 profile 的 `eeprom_*` 位址），可直接燒錄。
- **除錯**：預設就是 Decoder 從 EEPROM 取指令（本次建置產生的映像）。
- **Debug → 從 EEPROM 映像除錯…**：選一份燒錄檔（例如從實際晶片讀回的），Decoder 改從那份 byte 取指令，原始碼只用來對照。**Debug → 使用建置產生的 EEPROM 除錯** 換回來。
- **Decoder 解碼面板**（View 選單，或除錯時開）：列出兩個 domain 的每一條指令——PC、EEPROM 位址、byte、**從 EEPROM 解出的指令**、原始碼；目前執行位置標 `M▶`／`P▶`。和組譯器產生的不同的行標紅，滑鼠移上去顯示「從 EEPROM 取到」與「組譯器產生的」。雙擊跳到原始碼那一行。
- 時間軸與事件：沒有原始碼時顯示解出的指令；有原始碼但取到的不一樣時，事件附註「fetched from the EEPROM: …」，追蹤檔該行後面標 `!!`。

## 7. 取指令的時間與共用的 I2C 匯流排

每條指令**執行前**，Decoder 先經 I2C 從 EEPROM 讀出它要的 byte：程式碼（opcode 與內嵌 byte）、每個描述子（以及 EXEC_XFER 的 DeviceMap 項目）、MOV 讀的常數。讀完才執行。

一次隨機讀取 N byte 的交易：裝置位址＋W、記憶體位址（`eeprom_address_bytes`，預設 2）、裝置位址＋R、N 個資料 byte，共 `2 + 2 + N` 個 byte 階段，每個階段 9 個 SCL。27 MHz／400 kHz 時一個階段約 607.5 個 ISA 週期，另加 `fetch.transaction_overhead`（預設 2），以及每個階段的 `i2c.ack_delay` 與 `i2c.stretch`。

| 指令 | 要讀的 byte | 讀法 A（`sequential`，預設） | 讀法 B（`per_byte`） |
|---|---|---|---|
| `ALU ADD` | 1 + 1 | 2 次讀取，6,080 週期 | 2 次，6,080 週期 |
| `JMP`、`BR.COND` | 1 + 2 | 2 次，6,687 週期 | 3 次，9,120 週期 |
| `EXEC_CSR WRITE8` | 2（含內嵌 byte）+ 2 | 2 次，7,294 週期 | 4 次，12,160 週期 |
| `MOV CONST(…, 2)` | 1 + 4 + 2 | 3 次，11,549 週期 | 7 次，21,280 週期 |

**一條匯流排**：Main 取指令、Peri 取指令、Peri 的 `EXEC_XFER` 共用同一個 I2C master。先到先用；同一週期同時要用時 Main 先，另一個等（時間軸標「等待共用資源：I2C 匯流排」）。`EXEC_XFER` 從發出到傳輸結束都佔著匯流排。

Decoder 解碼面板的「取指令週期」欄顯示每條指令讀 EEPROM 要多少週期；`asmsim --decoded` 也列出。

**影響**：一條指令從 1～3 週期變成數千到兩萬多週期。範例 14 改用規格 14 章的實際畫面時序（450,000 週期、消隱視窗 14,362 週期），可以看到「檢查視窗之後再做好幾條指令才提交」會落到視窗外。

## 8. 參數

| 參數 | 預設 | 意思 |
|---|---|---|
| `fetch.source` | `eeprom` | Decoder 從哪裡取指令。`eeprom`（硬體）：I2C EEPROM，經過六個位址暫存器；`sram`：直接用組譯後的映像（舊模型，EEPROM 內容不影響執行） |
| `fetch.read_mode` | `sequential` | 讀法 A `sequential`：每段連續的 byte 一次讀取；讀法 B `per_byte`：每個 byte 各讀一次；`instant`：不花匯流排時間（舊模型，方便和以前的結果比對） |
| `fetch.transaction_overhead` | 2 | 每次讀取交易額外的週期 |

設定位置：ISA Top 的「I2C device / EEPROM」方塊按右鍵 → 設定參數；積木右鍵「設定參數」的相關參數裡也有 `fetch.read_mode`。

## 9. 待確認

- 硬體實際用讀法 A 還是 B，或是更快的讀法（例如連續的 opcode 用「目前位址讀取」）。
- 各位址暫存器實際的 CSR 位址、DeviceMap 是否真的在 EEPROM、程式碼起點是否有自己的暫存器。
- EEPROM 容量、I2C 位址 0x50、記憶體位址 2 byte、每次交易的額外週期：目前都是暫定值。
