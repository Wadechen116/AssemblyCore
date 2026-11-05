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

## 7. 參數

| 參數 | 預設 | 意思 |
|---|---|---|
| `fetch.source` | `eeprom` | Decoder 從哪裡取指令。`eeprom`（硬體）：I2C EEPROM，經過六個位址暫存器；`sram`：直接用組譯後的映像（舊模型，EEPROM 內容不影響執行） |

設定位置：ISA Top 的「I2C device / EEPROM」方塊按右鍵 → 設定參數。

## 8. 尚未建模

- **從 EEPROM 取指令的時間**：目前取指令仍不花 I2C 時間（每條指令照舊的週期數）。下一步依讀法 A（每區一次連續讀）／B（每 byte 一次）計算 I2C 週期，並和 Peri 的 `EXEC_XFER` 共用匯流排排隊。
- 各位址暫存器實際的 CSR 位址、DeviceMap 是否真的在 EEPROM、程式碼起點是否有自己的暫存器：都待硬體確認，目前做成可設定。
