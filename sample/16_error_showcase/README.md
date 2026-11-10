# 16_error_showcase　故意出錯的程式

讓 IDE 的三個地方一次呈現各類問題：**問題面板**（總覽，雙擊跳到那一行）、**編輯器**（那一行的波浪底線與行號旁的標記）、
**Decoder 面板**（解出的指令、顏色與原因）。

## 為什麼要附一份改壞的燒錄檔

問題分三類：

| 類別 | 意思 | 從哪裡來 |
|---|---|---|
| **A** 不合法的編碼 | bit 寫不成任何指令 | 燒進去的 byte 壞了 |
| **B** 編碼合法，但違反規格 | 寫得出指令，但超出硬體能做的範圍 | 燒進去的 byte 壞了 |
| **C** 執行當下的狀態 | 和 byte 無關，看執行到那裡時的狀態 | 程式的流程 |

組譯器會拒絕 A、B 類的寫法（例如 `MOV MAIN_SCRATCHPAD[31..32]` 會報 E202），所以它們**不可能從原始碼產生**，
只會出現在燒進去的 byte 和原始碼不一致的時候（燒錯檔、bit 翻轉、燒錄器出錯）。因此這個範例附一份故意改壞的燒錄檔
`burned/showcase_bad.eeprom.bin`，專案檔用 `debug_eeprom` 指定除錯時讀它。

## 會看到的問題

| 行 | 原始碼 | 建置 | 燒錄檔裡被改壞的地方 | 執行時 |
|---|---|---|---|---|
| 26 | `MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA` | W502 警告（沒寫過就讀） | | |
| 27 | `MOV PERI_RESULT_SLOT[0..1] -> MAIN_SCRATCHPAD[2..3]` | W505 警告（沒先檢查結果槽） | | **Main 錯誤 C**：結果槽沒有資料 |
| 28 | `ALU ADD unsigned` | W501 警告（ALU_OPB 沒載入） | 描述子改成沒有指定的子運算碼 → **解碼 A** | |
| 30 | `MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB` | W402 警告（永遠跑不到） | 來源位移改成 31（範圍 31..32）→ **解碼 B** | |
| 36 | `EXEC_XFER READ dev=0, len=2, ptr=0, ext=[0x3B]` | | ptr 改成 31（31 + 2 > 32）→ **解碼 B** | **Peri 錯誤 B** |

## 怎麼操作

1. 開啟 `16_error_showcase.acproj`。輸出面板會說「這個專案從燒錄檔 burned/showcase_bad.eeprom.bin 除錯」。
   建置後，問題面板列出 4 個**建置**警告，編輯器那幾行有黃色波浪底線。
2. 按 **F5** 開始除錯。程式還沒開始跑，解碼器就找到燒錄內容的問題：
   - 問題面板自動打開，多了「解碼 A」1 筆、「解碼 B」2 筆，雙擊跳到第 28、30、36 行；
   - 編輯器這三行有紅色波浪底線，滑鼠移過去顯示原因；
   - Decoder 面板：A 那一行紅色、B 那兩行橘色，後面附上原因。
3. 程式執行後停在 **Main 第 27 行**：「執行錯誤 C」（結果槽沒有資料），Decoder 面板那一行標成「M✖ C」。
4. 再按 **F5**，停在 **Peri 第 36 行**：「執行錯誤 B」（`fault_range`），Decoder 面板標成「P✖ B」。
   兩個錯誤在同一個週期發生，Main 先報（共用匯流排同時要用時 Main 優先）。
5. 想看沒有改壞的情況：除錯 →「使用建置產生的 EEPROM 除錯」，再按 F5。只剩建置警告與 Main 的錯誤 C。

## 檔案

| 檔案 | 內容 |
|---|---|
| `showcase.asm` | 原始碼（每個故意的錯誤都有註解） |
| `16_error_showcase.acproj` | 專案；`debug_eeprom` 指定改壞的燒錄檔 |
| `showcase.scn` | 情境：用建置的燒錄檔執行，確認 Main 錯誤 C（`sample/check_samples.py` 會跑） |
| `burned/showcase_bad.eeprom.bin` `.json` `.sym` | 改壞的燒錄檔、對照表、名稱 |
| `burned/showcase_bad.scn` | 情境：用改壞的燒錄檔執行，確認 Main 錯誤 C、Peri 錯誤 B |
| `make_bad_image.py` | 重新產生改壞的燒錄檔，並驗證解碼與執行的結果都如預期（改了原始碼後要重跑） |

命令列也看得到同樣的結果：

```bash
asmsim showcase.asm --eeprom burned/showcase_bad.eeprom.bin --decoded      # 每條指令解出什麼，A／B 問題與原因
asmdis --eeprom burned/showcase_bad.eeprom.bin --check                     # 反組譯檢查：D201（A）、D205（B）
```
