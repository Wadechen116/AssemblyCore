# 17_alu_busy

一個**不會結束**的迴圈，讓 ALU 一直在工作：每一圈做 9 次 ALU 運算（INC、ADD、SUB、MUL、XOR、AND、SHR、SHL、CMP.GE），
其餘幾乎都是把運算元搬進 ALU、把結果搬出來的 MOV。這個範例是用來**看**的：在「模擬器」頁的架構圖上觀察 ALU 被頻繁使用的樣子。

實際執行的指令中，ALU 運算約佔 27%，MOV 約佔 66%（3000 個週期內：1269 道指令，ALU 341 道、MOV 842 道）。

## 程式在算什麼

| 每一圈 | 運算 | 結果放在 `MAIN_SCRATCHPAD` |
|---|---|---|
| `n = n + 1` | `ALU INC` | `[0..1]` |
| `sum = sum + n` | `ALU ADD` | `[2..3]` |
| `diff = sum - n` | `ALU SUB` | `[4..5]` |
| `sq = n * n` | `ALU MUL` | `[6..7]` |
| `mix = sum ^ sq` | `ALU XOR` | `[8..9]` |
| `low = mix & 0x00FF` | `ALU AND` | `[10..11]` |
| `half = sum >> 1` | `ALU SHR 1` | `[12..13]` |
| `quad = n << 2` | `ALU SHL 2` | `[14..15]` |
| `n >= 8 ?` | `ALU CMP.GE` | 只設定旗標，成立就開始新的一輪 |

每 8 圈為一輪：`n` 與 `sum` 歸零，輪數（`[16..17]`）加 1，並用 `EXEC_CSR WRITE_SP` 寫到 CSR `LAPS`（`0x150`）。
`[18..19]` 是常數要進 `ALU_OPB` 之前的中繼位置（常數不能直接 MOV 進 ALU）。

## 試試看

1. 建置：零警告。開啟「模擬器」頁（工具列 **Simulator**），按 **F5** 開始。
2. 專案附的 `alu_busy.simparams` 把速度設成每秒 40 個週期（約每秒 15 道指令），並讓取指不花匯流排時間，所以畫面上看得出每一道指令。
3. **架構圖**：
   - 每次 `ALU` 運算，**ALU** 與 **Main Status Flag** 兩個方塊的外框會亮一下，`OUT` 那一列的數值跟著變。
   - 每次 `MOV … -> ALU_OPA／ALU_OPB` 與 `MOV ALU_OUT … ->`，**ALU** 和 **MAIN_SCRATCHPAD** 一起亮：運算元進去、結果出來。
   - 每 8 圈一次的 `EXEC_CSR WRITE_SP`，**CSR Arbiter** 和 **CSR** 才亮一下，可以對照出 ALU 有多頻繁。
4. 想看得更清楚：按 **Pause** 後用 **Step Into** 一道一道走；或把速度調成 10（工具列的 Speed）。想看整體節奏：速度調成 1000。
5. **組譯清單**的「次數」欄：跑一陣子後，9 道 `ALU` 指令的次數幾乎相同，`lap` 之後那幾道是它們的八分之一。
6. **Memory** 面板選 `scratch`：前 16 個位元組每一圈都在變。**監看**面板加 `scratch[2..3]`（`sum`）與 `csr[0x150]`（輪數）。
7. **Decoder** 面板勾「Frontend 模型」再按「前進 1 週期」：看 `ALU` 指令如何經過七個狀態，以及它只需要讀一個位元組的描述子。
8. 改改看：把 `ALU CMP.GE` 前的 `MOV CONST(8, 2)` 改成 `MOV CONST(3, 2)`，一輪變成 3 圈，CSR 方塊亮得更頻繁。
   拿掉任何一行 `MOV ALU_OUT …`，下一個 ALU 會在結果被搬走前覆寫它 → 警告 **W504**。

## 預期

`asmsim -I ../common alu_busy.asm --scenario alu_busy.scn` → `PASS (10 checks)`：跑滿三輪後，
`diff = 28`、`sq = 64`、`mix = 100`、`low = 100`、`half = 18`、`quad = 32`（最後一圈 `n = 8`、`sum = 36`），而且程式沒有停下來。
