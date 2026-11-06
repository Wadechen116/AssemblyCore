# 15_eeprom_images　測試用的 EEPROM 燒錄檔

範例 14（`../14_main_peri_sim`）燒進 EEPROM 的映像：一份正確的，加上 8 份**故意放了錯誤**的。用來測試反組譯與驗證能不能把問題找出來。

每份燒錄檔都是三個檔案一組，要放在一起：

| 副檔名 | 內容 |
|---|---|
| `.eeprom.bin` | 燒錄的 byte（燒錄器用這個；`good` 另附 `.eeprom.hex` Intel HEX） |
| `.eeprom.json` | 對照表：六個區域各在 EEPROM 的哪裡、描述表的位移。**反組譯一定要有它** |
| `.eeprom.sym` | 名稱：標籤、CSR、服務、裝置。沒有它也能反組譯，只是變成數字和 `L_xxxx` |

重新產生：`python3 make_images.py`（用 `../../build` 的 asmc；`--bin-dir DIR` 指定別的）。每份該得到什麼結果寫在 `images.txt`，`../check_samples.py` 會自動核對。

## 兩種檢查

- **反組譯檢查**（「這是一個符合規格的程式嗎？」）：只需要燒錄檔。三件事：
  1. **解碼**：每個 byte 都是合法的指令編碼（保留位元為 0、索引在表內、跳躍目標落在指令開頭…）
  2. **規格**：反組譯的結果交給組譯器，套用本專案文件定義的所有規則（`docs/ASM_SYNTAX.md` 的錯誤 E… 與警告 W…、profile 的限制、資料流檢查）
  3. **往返一致**：反組譯的結果再組譯，必須得到逐 byte 相同的燒錄檔
- **驗證燒錄檔**（「這是我寫的那個程式嗎？」）：燒錄檔和原始碼（範例 14）組譯的結果逐 byte 比對，再逐條比對指令。

## 檔案與預期結果

### 正確（good）

| 檔案 | 內容 | 反組譯檢查 | 驗證（對範例 14） |
|---|---|---|---|
| `good` | 範例 14 原樣 | ✔ 通過，名稱還原，組回同一份 | ✔ 一致 |
| `good_no_names` | 同上，但**沒有 `.eeprom.sym`** | ✔ 通過，標籤是 `L_0000`、CSR 是 `0x120` | ✔ 一致 |

### 錯誤（bad）

| 檔案 | 故意放的錯 | 反組譯檢查會說 | 驗證（對範例 14） |
|---|---|---|---|
| `bad_reserved_bits` | Main BR 描述子 0 的保留位元 [8] 設成 1 | ✘ 解碼錯誤 **D201**（保留位元不為 0） | ✘ |
| `bad_table_index` | pc 3 的 MOV 改成用描述子 63（表裡只有 11 筆） | ✘ 解碼錯誤：**descriptor is outside its table** | ✘ |
| `bad_branch_target` | `JMP start` 的目標改成 0x11（一條 EXEC_CSR 的第二個 byte） | ✘ 解碼錯誤 **D203**（跳躍目標不是指令開頭），規格錯誤 E303 | ✘ |
| `bad_runs_off_end` | Main 最後一條 `JMP` 改成 `ALU` | ✘ 規格錯誤 **E303**（程式會跑出結尾） | ✘ |
| `bad_unreachable` | `JMP drop` 改成 `JMP start` | ✘ 規格警告 **W402**（`drop` 永遠跑不到），而且組回的是另一份燒錄檔 | ✘ |
| `bad_constant` | 常數 `GYRO_BIAS` 從 0 改成 5 | ✔ **通過**：它仍是一個合法、符合規格的程式 | ✘ **只有這裡抓得到** |
| `bad_map` | 對照表把 Peri 程式碼的位址記偏 2 byte（燒錄的 byte 是對的） | ✘ 解碼錯誤（讀到錯位的 byte：descriptor outside its table…） | ✘ 對照表位址和程式不同 |
| `bad_truncated` | 最後 20 byte 沒有燒進去 | ✘ 13 個解碼錯誤（讀到未燒錄的 0xFF），規格錯誤 **E302** | ✘ 燒錄檔太短 |

`bad_constant` 是刻意放的：反組譯檢查只能確認「這是一個正確的程式」，無法知道它「應該是哪個程式」；要確認燒進去的就是你寫的，要做**驗證燒錄檔**。

## 怎麼測

**IDE**（`AssemblyCoreIDE.exe`）：

- **反組譯**：Build →「反組譯 EEPROM 燒錄檔…」，選一個 `.eeprom.bin` →「反組譯」。上方顯示 ✔／✘，下方列出每一項檢查結果（解碼錯誤、規格錯誤／警告、往返一致）；點規格錯誤那一行會跳到反組譯文字的那一行。可勾選「使用符號檔的名稱」「加上位址與 byte 註解」「嚴格（警告也算失敗）」；「另存為 .asm 並開啟…」存成原始碼。
- **驗證**：先開範例 14 的專案（`14_main_peri_sim.acproj`）並建置，Build →「驗證 EEPROM 燒錄檔…」，選一個 `.eeprom.bin` →「驗證」。列出不一致的指令和 byte；雙擊指令那一行跳到原始碼。原始碼有修改時會先自動建置。

**命令列**：

```bash
asmdis --eeprom bad_runs_off_end.eeprom.bin --check            # 反組譯檢查；不通過時結束碼 1
asmdis --eeprom good.eeprom.bin -o good.asm                      # 反組譯成原始碼（有 .eeprom.sym 就還原名稱；--no-names 不用）
asmsim ../14_main_peri_sim/main_peri.asm -I ../common --verify-eeprom bad_constant.eeprom.bin   # 驗證；不一致時結束碼 1
```
