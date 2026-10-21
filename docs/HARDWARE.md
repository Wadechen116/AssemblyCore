# 硬體設計器（Hardware Designer）

規格只**命名**很多東西，卻沒有給數字或意義：CSR 的位址與每個位元做什麼、I2C 裝置的位址與暫存器、服務（service）的資料內容與位元組序、常數。
這些是「這塊板子」的事實，原本散落在三個地方，而且必須手動保持一致：

| 以前 | 內容 |
|---|---|
| `defs.inc` | `.csr` `.csrbit` `.service` `.devmap` `.const` `.equ`（給組譯器） |
| 環境檔 `*.cfg` | `[csr]` 種類與動作、`[device]` 暫存器值、`[sim]` `[timing]`（給模擬器） |
| 腦袋 | 兩者要一致 |

**硬體描述（`.achw`）是唯一的事實來源**，兩個檔案都由它產生，所以不會不一致：

```
board.achw ──asmhw gen / 設計器──►  board.gen.inc      （組譯器定義）
                                └►  board.gen.env.cfg  （模擬器環境）
```

## 在 IDE 裡使用

**File → Hardware**

| 項目 | 作用 |
|---|---|
| New Hardware Description… | 在目前專案新增一份描述：建立 `.achw` 與兩個產生檔，專案加入 `hardware = …`，產生的定義檔排在 sources **第一個**，環境檔設為專案的環境 |
| Open Hardware Designer | 開啟專案的描述（也可以在專案樹的 Hardware 群組雙擊） |
| Adopt Existing Definitions… | **接手既有專案**：選現有的 `defs.inc`（與環境檔），讀進來產生描述。產生檔會寫回**原來的檔名**，所以專案裡所有 `.include "defs.inc"` 都不必改 |

設計器（一個頁籤）左邊是元件樹，右邊是選到的元件的屬性表單，下方是**問題清單**與兩個**預覽**（產生的定義與環境）：

| 元件 | 你填的 | 產生 |
|---|---|---|
| 板子 | 名稱、說明、產生檔的檔名 | — |
| 常數 | `.equ` 具名數值、`.const`（1 或 2 位元組） | `.equ`、`.const` |
| CSR 暫存器 | 名稱、位址、種類（ram／value／action／status／const）、重置值、訊號綁定、**每個位元**的模擬動作與組譯器角色 | `.csr`、`.csrbit`、`[csr]` |
| I2C 裝置 | 名稱、7 位元位址、DeviceMap 項目、暫存器內容表 | `.devmap`、`[device]` |
| 服務 | 名稱、id、資料位元組數、線上位元組序 | `.service … , len=, big/little` |
| 模擬器環境 | `[sim]` `[timing]` `[latency]` `[inject]`…（一行一個 `key = value`） | 環境檔對應區段 |
| 組譯器行 | 設計器不認得的行（例如 `.include`） | 原封不動寫進定義檔 |

重點行為：

- **即時驗證**：每次修改都檢查並列在問題清單（雙擊一筆會跳到該元件）。訊息用白話說明（例如「address 0x302 is already taken by csr PERI_CTRL」「the service id must be 1..15」）。
- **安全網**：模型沒有檢查的規則，設計器會把產生的檔案真的交給組譯器與模擬器跑一次，它們拒絕的內容也會列出來。所以不會產生工具自己不接受的檔案。
- **不用存檔就生效**：修改會立刻把新的定義放進工作區（當作未存檔的緩衝區），下一次建置與除錯就用新的；存檔（Ctrl+Shift+S）才寫入磁碟。
- 關閉頁籤時有未存檔變更會詢問；選「放棄」會把描述與兩個產生檔都還原成磁碟上的內容。
- 產生檔第一行註明由哪份描述產生、請勿手改。**要改請改描述。**

## 目標 profile（Target profile）

設計器樹最下面的「Target profile」編輯專案 `profile =` 指定的檔案（沒有的話有按鈕可以建立）。重點：

- 表單由「每個鍵的說明表」產生（`asmcore/profile_info.h`），每個群組一個頁籤：描述子表深度、PC 寬度、SRAM 配置、映像佈局與對齊、描述子位元組序、DeviceMap 排列、ALU 子運算碼、softret、重置狀態、事件、延遲數字。範圍由程式碼決定，不會填出載入器拒絕的值。
- **只改那一行**：保留檔案的註解、順序與縮排；重複的鍵改最後一個（載入器採用的那個）；沒有的鍵附加在檔尾。修改立即成為工作區的未存檔緩衝區，下一次建置／除錯就用新值。
- **缺口清單**：規格沒有給、也沒有人確認過的數值（ALU 編號、描述子位元組序、SRAM 配置、DeviceMap 排列、映像佈局、PC 寬度、事件、延遲、兩項選用硬體功能）是「規劃值」。工具照樣能用它們，但頁面頂端明講「結果只能當規劃參考」。硬體負責人確認某群組後，勾選該頁籤的確認框，profile 多一行 `confirmed = alu, placement`。
- 命令列：`asmc --profile-gaps` 列出缺口、原因與涵蓋的鍵；`asmc --artifacts` 的 `manifest.json` 記錄 `unconfirmed`。
- 可確認的群組：`pc_width`、`same_region`、`placement`、`layout`、`descriptor_endian`、`device_map`、`alu`、`events`、`latency`。

## `.achw` 檔案格式

與環境檔同風格（`[區段 名稱]` 加 `key = value`，`;` 註解）。**設計器寫檔時不保留註解**；用 `description =` 欄位寫說明（會帶進產生檔的註解）。

```
[hardware]
name = IMU board
description = ...
include_file = defs.inc          ; 產生的定義檔名稱（相對於描述檔）。省略 = NAME.gen.inc
env_file = imu_env.cfg           ; 產生的環境檔名稱。省略 = NAME.gen.env.cfg

[equ PERI_DONE]
value = 0x01

[const K_A]
width = 2                        ; 1 = db，2 = dw
values = 300, 25

[csr PERI_CTRL]
addr = 0x302
kind = action                    ; ram | value | action | status | const
bit0 = done_set                  ; action：寫 1 時模擬器做的動作；status：運算式
bit3 = timer_start
role3 = timer_start              ; 組譯器角色（.csrbit）：timer_start timer_clear poll_arm poll_clear
; ram：reset = 5    value：bind = timer_period 與 bytes = 2    const：const = 0x44
; simulated = false              ; 只是組譯器用的名稱（例如 2 位元組 CSR 的第二個位元組），環境檔不產生它

[device imu]
addr = 0x68
index = 0                        ; DeviceMap 項目 0..7（省略 = 不在 DeviceMap）
symbol = DEV_IMU                 ; EXEC_XFER dev=DEV_IMU 用的名稱
reg 0x3a = 0x00, 0x00, 0x01      ; 其餘鍵同環境檔（docs/SIM.md 第 4 節）
block 0x43 = 0x00 0x0A 0x00 0x14
; simulated = false              ; 只有 DeviceMap 項目，環境檔不產生（模擬器回 NACK）

[service SID_IMU_GYRO_RAW]
id = 1
len = 6
wire = big                       ; big = 高位元組先；little = 低位元組先

[sim]                            ; [sim] [timing] [latency] [bus] [signal] [condition] [inject] 原樣傳到環境檔
tie_order = main peri

[raw_inc]                        ; 設計器不認得的組譯器行
line = .include "other.inc"
```

## 命令列：`asmhw`

```
asmhw gen    board.achw [-o PREFIX]     產生定義檔與環境檔（檔名依 include_file／env_file，否則 board.gen.inc / board.gen.env.cfg）
asmhw check  board.achw                 驗證；並用真的組譯器與模擬器檢查產生的檔案
asmhw import --inc defs.inc [--env env.cfg] -o board.achw     由既有檔案建立描述（不遺失任何內容）
```
結束碼：0 正常、1 描述有錯誤、2 用法／讀寫錯誤。可放進建置腳本；`sample/check_samples.py` 就用它確認「專案裡的產生檔就是現在描述會產生的」。

## 驗證規則（白話）

| 範圍 | 規則 |
|---|---|
| 名稱 | CSR、服務、常數、`.equ`、DeviceMap 名稱共用組譯器的**同一個**名稱空間；必須是識別字且不重複 |
| CSR | 位址 0x000..0xFFF 且不重複（`value` 種類佔 `bytes` 個位址）；種類正確；`value` 要有 `bind`、bytes 1..4；`const` 與重置值要能放進一個位元組；動作名稱存在；角色是四種之一；角色與模擬動作不一致會警告 |
| 裝置 | I2C 位址 7 位元且不重複；DeviceMap 項目 0..7 且不重複；種類存在；暫存器位址與值是數字、值 0..255；`nack` 等只能 true／false；沒有 DeviceMap 項目會警告 |
| 服務 | id 1..15 且不重複；長度 1..32；沒宣告長度或多位元組卻沒宣告位元組序會警告（組譯器就無法檢查 W517／W514） |
| 常數 | 至少一個值；值放得進寬度；總量不超過 256 位元組 |

## 限制

- 描述只涵蓋「板子」的事實；程式裡用到的事件向量（`.vector`）屬於程式，不在描述裡。
- 環境檔的 `[signal]`、`[condition]`、`[inject]` 等以原文區段保存，設計器用文字方塊編輯，沒有逐欄表單。
- profile 的說明與缺口文字目前只有英文（來自核心）；介面其餘部分有繁體中文。
- 設計器不會改你的 `.asm`：接手既有專案時，產生檔寫回原檔名，原始碼完全不變。
