# 第 4 層：錯誤偵測——故意寫壞的映像

由 `make_faults.py` 產生（請改產生器，不要直接改這份）。結果 CSR 的基底：`RES = 0x600`。編號對應 `docs/HW_FAULT_DETECTION.md` 的各列。

## 每一份映像是什麼

先組譯一支**合法**的小程式（`f_<編號>.asm`，稱為載體），再改掉映像裡的一兩個位元組，讓其中一道指令（受測指令）變成那一列所說的不合法情形。
載體用兩個標記把受測指令夾住：

| 寫入 | 意思 |
|---|---|
| `RES+0 = 0xF0` | 下一道就是受測指令 |
| `RES+1 = 0x0F` | 受測指令之後的程式繼續執行了 |
| `RES_DONE = 0xA5` | 程式跑到結尾 |

**模型的行為**：執行到受測指令時報錯，那個 sequencer 停止——`RES+0` 有寫入，`RES+1` 與 `RES_DONE` 沒有（少數例外在表中註明）。
**請在硬體上觀察**：這三次寫入各有沒有出現；硬體有沒有回報錯誤、在哪一級（解碼／執行／PC 更新…）回報；之後 sequencer 是停住、繼續、還是重置。

模型只是我們目前的做法，**不是標準答案**：規格文件才是依據。硬體若不偵測某一項（照樣往下執行，`RES+1` 出現），請照實記錄，模擬器會跟著改。

## 檔案

每個案例在 `out/f_<編號>/`：

| 檔案 | 內容 |
|---|---|
| `f_<編號>.main.bin`（或 `.peri.bin`） | **改過的**邏輯映像 |
| `f_<編號>.eeprom.bin`、`.eeprom.json` | 改過的 EEPROM 映像與各區域位址 |
| `f_<編號>.sram.hex` | 改過的程式 SRAM 內容（32 位元的字） |
| `f_<編號>.*.layout.json` | 區段位置（沒有改） |
| `f_<編號>_carrier.*` | 未改動的載體（清單 `.lst` 可以對照原本是哪道指令）；載體本身會正常跑到結尾 |

EXEC_XFER 的案例（X 開頭）需要測試平台在位址 `0x2F` 有一個會回 ACK 的 I2C 裝置；其餘 Peri 的案例（P、R、U 開頭）不發出 I2C 交易。

## 各案例

| 編號 | 檢查 | 改了什麼 | 改動的位元組（邏輯位移：原值→新值；EEPROM 位址；SRAM 字與 lane） | 模型：錯誤碼 | 模型：停在 pc | 模型：`RES+1`／`RES_DONE` |
|---|---|---|---|---|---|---|
| **F3** | pc 超出程式（程式跑出結尾）（Main） | 最後那道 `JMP done` 的指令位元組改成一道 ALU 指令，程式因此沒有結尾、一路執行到程式碼之外 | 9：0x40→0xC0；EEPROM 0x0009；SRAM 字 2 lane 1 | `fault_pc_out_of_code` | 10 | 有／有 |
| **F4** | 描述子索引超出表格（Main） | MOV 指令位元組的索引改成 63（表格裡沒有那麼多筆） | 2：0x80→0xBF；EEPROM 0x0002；SRAM 字 0 lane 2 | `fault_desc_index` | 2 | 沒有／沒有 |
| **M1** | CsrOneByte 描述子保留位元非零（Main） | 描述子的 bit 12（保留）設為 1 | 17：0x06→0x16；EEPROM 0x0011；SRAM 字 4 lane 1 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **M2** | READ8 目的指標 bit 7:5 非零（Main） | 指令第二個位元組（目的指標）改成 0x20 | 3：0x00→0x20；EEPROM 0x0003；SRAM 字 0 lane 3 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **M3** | CsrMultiByte 的 csr_op 是保留值（Main） | 描述子的 csr_op（bit 15:14）改成 1 | 21：0x18→0x58；EEPROM 0x0015；SRAM 字 5 lane 1 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **M4** | CsrMultiByte 的 ptr + len 超過 32（Main） | 描述子改成 len = 16、ptr = 31（合計 47） | 20：0x00→0xFF；EEPROM 0x0014；SRAM 字 5 lane 0；21：0x18→0x19；EEPROM 0x0015；SRAM 字 5 lane 1 | `fault_range` | 2 | 沒有／沒有 |
| **B1** | MainBranchDesc 保留位元非零（Main） | 描述子的 bit 8（保留）設為 1 | 9：0x00→0x01；EEPROM 0x0009；SRAM 字 2 lane 1 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **B2** | event_safe 設在 CASCADE 描述子上（Main） | CASCADE_BEGIN 描述子的 event_safe（bit 9）設為 1 | 21：0x40→0x42；EEPROM 0x0015；SRAM 字 5 lane 1 | `fault_desc_reserved` | 4 | 沒有／沒有 |
| **B3** | BR.ERET 描述子的 target 非零（Main） | JMP 的描述子改成型別 3（BR.ERET），保留原本非零的 target | 9：0x00→0xC0；EEPROM 0x0009；SRAM 字 2 lane 1 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **B4** | 條件碼是保留值（Main） | 分支描述子的條件碼改成 0xF（沒有定義） | 9：0x00→0x3C；EEPROM 0x0009；SRAM 字 2 lane 1 | `fault_cond_undefined` | 2 | 沒有／沒有 |
| **B5** | 分支目標超出程式範圍（條件成立）（Main） | JMP 描述子的目標改成 255（程式沒有那麼長） | 8：0x03→0xFF；EEPROM 0x0008；SRAM 字 2 lane 0 | `fault_branch_target` | 2 | 沒有／沒有 |
| **B6** | 分支目標落在指令中間（Main） | JMP 描述子的目標改成下一道指令（兩個位元組的 EXEC_CSR）的第二個位元組 | 8：0x03→0x04；EEPROM 0x0008；SRAM 字 2 lane 0 | `fault_desc_index` | 4 | 沒有／沒有 |
| **B7** | cascade 進行中又遇到 CASCADE_BEGIN（Main） | cascade 區塊裡的那道 ALU 指令位元組改成分支指令，索引指向同一個 CASCADE_BEGIN 描述子 | 5：0xC0→0x40；EEPROM 0x0005；SRAM 字 1 lane 1 | `fault_nested_cascade` | 5 | 沒有／沒有 |
| **B8** | cascade block0 目標超出程式（Main） | CASCADE_BEGIN 描述子的目標改成 255 | 20：0x05→0xFF；EEPROM 0x0014；SRAM 字 5 lane 0 | `fault_branch_target` | 4 | 沒有／沒有 |
| **B9** | CASCADE_END 但沒有進行中的 cascade（Main） | JMP 的描述子改成型別 2（CASCADE_END） | 9：0x00→0x80；EEPROM 0x0009；SRAM 字 2 lane 1 | `fault_cascade_end_without_begin` | 2 | 沒有／沒有 |
| **B10** | CASCADE_END 的 index 和目前步數不符（Main） | CASCADE_END 描述子的 index 由 0 改成 1 | 23：0x80→0x84；EEPROM 0x0017；SRAM 字 5 lane 3 | `fault_cascade_step_mismatch` | 6 | 沒有／沒有 |
| **B11** | cascade 出口目標超出程式（Main） | CASCADE_END 描述子的目標改成 255 | 22：0x07→0xFF；EEPROM 0x0016；SRAM 字 5 lane 2 | `fault_branch_target` | 6 | 沒有／沒有 |
| **V1** | MainMoveDesc 保留位元非零（Main） | 描述子的 bit 5（保留）設為 1 | 8：0x03→0x23；EEPROM 0x0008；SRAM 字 2 lane 0 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **V2** | 區域代碼是保留值（Main） | 來源區域代碼改成 9 | 11：0x00→0x90；EEPROM 0x000B；SRAM 字 2 lane 3 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **V3** | 來源區域不能讀（ALU_OPA）（Main） | 來源區域改成 3（ALU_OPA） | 11：0x00→0x30；EEPROM 0x000B；SRAM 字 2 lane 3 | `fault_mov_region` | 2 | 沒有／沒有 |
| **V4** | 目的區域不能寫（ALU_OUT）（Main） | 目的區域改成 5（ALU_OUT） | 11：0x00→0x05；EEPROM 0x000B；SRAM 字 2 lane 3 | `fault_mov_region` | 2 | 沒有／沒有 |
| **V5** | 同一區域內來源與目的重疊（Main） | 目的位移由 8 改成 2（與來源 0..3 重疊） | 9：0x08→0x02；EEPROM 0x0009；SRAM 字 2 lane 1 | `fault_mov_overlap` | 2 | 沒有／沒有 |
| **V7** | MAIN_CONST_POOL 只能搬到 MAIN_SCRATCHPAD（Main） | 目的區域由 MAIN_SCRATCHPAD 改成 3（ALU_OPA），位移 0 | 11：0x70→0x73；EEPROM 0x000B；SRAM 字 2 lane 3 | `fault_mov_region` | 2 | 沒有／沒有 |
| **V8** | ALU 運算元不是 offset 0、長度 2（Main） | 長度由 2 改成 1 | 8：0x01→0x00；EEPROM 0x0008；SRAM 字 2 lane 0 | `fault_mov_range` | 2 | 沒有／沒有 |
| **V9** | 來源範圍超出區域大小（Main） | 來源位移由 0 改成 30（30 + 4 超過 32） | 10：0x00→0x1E；EEPROM 0x000A；SRAM 字 2 lane 2 | `fault_mov_range` | 2 | 沒有／沒有 |
| **V10** | 從 PERI_RESULT_SLOT 搬，但結果槽沒有資料（Main） | 不改任何位元組：這支程式本身合法（組譯器只給警告），沒有 Peri 發佈過結果 | （沒有改動） | `fault_slot_not_valid` | 2 | 沒有／沒有 |
| **L1** | 子運算碼沒有由 target profile 指定／arith_op 是保留值（Main） | arith_op 改成 31（沒有任何運算用這個編碼） | 28：0x00→0x1F；EEPROM 0x001C；SRAM 字 7 lane 0 | `fault_desc_reserved` | 4 | 沒有／沒有 |
| **L2** | logic_op 是保留值（Main） | logic_op 改成 7 | 28：0x80→0xB8；EEPROM 0x001C；SRAM 字 7 lane 0 | `fault_desc_reserved` | 4 | 沒有／沒有 |
| **L3** | LOGIC 描述子保留位元非零（Main） | 描述子的 bit 0（保留）設為 1 | 28：0x80→0x81；EEPROM 0x001C；SRAM 字 7 lane 0 | `fault_desc_reserved` | 4 | 沒有／沒有 |
| **L4** | EQ 設了 signed_mode（Main） | signed_mode（bit 5）設為 1 | 28：0x40→0x60；EEPROM 0x001C；SRAM 字 7 lane 0 | `fault_desc_reserved` | 4 | 沒有／沒有 |
| **L5** | 移位量不在 1..8（Main） | 移位量改成 0 | 28：0xC1→0xC0；EEPROM 0x001C；SRAM 字 7 lane 0 | `fault_desc_reserved` | 4 | 沒有／沒有 |
| **E1** | 不在事件處理中卻執行 BR.ERET（Main） | JMP 的描述子改成一個正確的 BR.ERET（型別 3，其餘為 0） | 8：0x03→0x00；EEPROM 0x0008；SRAM 字 2 lane 0；9：0x00→0xC0；EEPROM 0x0009；SRAM 字 2 lane 1 | `fault_bad_eret_context` | 2 | 沒有／沒有 |
| **P1** | Peri 使用 MultiByte CSR（Peri） | 指令位元組的 bit 5（MultiByte）設為 1 | 2：0x01→0x21；EEPROM 0x0002；SRAM 字 512 lane 2 | `fault_peri_csr_multibyte` | 2 | 沒有／沒有 |
| **P2** | Peri 使用 READ8（Peri） | 描述子的運算改成 3（READ8） | 15：0x06→0xC6；EEPROM 0x000F；SRAM 字 515 lane 3 | `fault_peri_csr_read` | 2 | 沒有／沒有 |
| **P3** | CsrOneByte 描述子保留位元非零（Peri） | 描述子的 bit 12（保留）設為 1 | 15：0x06→0x16；EEPROM 0x000F；SRAM 字 515 lane 3 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **R1** | PeriBranchDesc 保留位元非零（Peri） | 描述子的 bit 8（保留）設為 1 | 9：0x00→0x01；EEPROM 0x0009；SRAM 字 514 lane 1 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **R2** | BR.ERET 描述子的 target 非零（Peri） | JMP 的描述子改成型別 8（BR.ERET），保留原本非零的 target | 9：0x00→0x80；EEPROM 0x0009；SRAM 字 514 lane 1 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **R3** | 條件碼是保留值（Peri） | 分支型別改成 9（保留） | 9：0x00→0x90；EEPROM 0x0009；SRAM 字 514 lane 1 | `fault_cond_undefined` | 2 | 沒有／沒有 |
| **R4** | 分支目標超出程式範圍（條件成立）（Peri） | JMP 描述子的目標改成 255 | 8：0x03→0xFF；EEPROM 0x0008；SRAM 字 514 lane 0 | `fault_branch_target` | 2 | 沒有／沒有 |
| **U1** | PeriPublishDesc 保留位元非零（Peri） | 描述子的 bit 0（保留）設為 1 | 8：0x00→0x01；EEPROM 0x0008；SRAM 字 514 lane 0 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **U2** | service_id 為 0（Peri） | service_id 改成 0 | 9：0x60→0x00；EEPROM 0x0009；SRAM 字 514 lane 1 | `fault_publish_service` | 2 | 沒有／沒有 |
| **U3** | src + len 超過 PERI_RX_BUF（Peri） | 描述子改成 src = 31、len = 8（合計 39） | 8：0x00→0x9C；EEPROM 0x0008；SRAM 字 514 lane 0；9：0x60→0x6F；EEPROM 0x0009；SRAM 字 514 lane 1 | `fault_range` | 2 | 沒有／沒有 |
| **X1** | addr_len 為 3（Peri） | 描述子的 addr_len 改成 3 | 11：0x84→0x8C；EEPROM 0x000B；SRAM 字 514 lane 3 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **X2** | READ 目的指標 bit 7:5 非零（Peri） | 指令第二個位元組（目的指標）改成 0x20 | 3：0x00→0x20；EEPROM 0x0003；SRAM 字 512 lane 3 | `fault_desc_reserved` | 2 | 沒有／沒有 |
| **X3** | MultiByte WRITE（Peri） | MultiByte 描述子的 rw（bit 15）由讀改成寫 | 11：0x84→0x04；EEPROM 0x000B；SRAM 字 514 lane 3 | `fault_xfer_multibyte_write` | 2 | 沒有／沒有 |
| **X4** | MultiByte 的 ptr + len 超過 PERI_RX_BUF（Peri） | 描述子改成 len = 16、ptr = 31（合計 47） | 10：0x20→0xFF；EEPROM 0x000A；SRAM 字 514 lane 2；11：0x84→0x85；EEPROM 0x000B；SRAM 字 514 lane 3 | `fault_range` | 2 | 沒有／沒有 |
| **X5** | DeviceMap 沒有 dev_sel 指到的項目（Peri） | 描述子的裝置編號改成 7（DeviceMap 只有 1 筆） | 11：0x84→0xF4；EEPROM 0x000B；SRAM 字 514 lane 3 | `fault_dev_undefined` | 2 | 沒有／沒有 |
| **X6** | DeviceMap 項目的 bus_type 是保留值（Peri） | DeviceMap 第 0 筆的 bus_type（bit 7）設為 1 | 20：0x2F→0xAF；EEPROM 0x0014；SRAM 字 517 lane 0 | `xfer_fault_reserved_bus_type` | 2 | 沒有／沒有 |

**說明**

- **F3**：這一列的「受測指令」是程式的結尾：兩個標記與完成標記都會被寫入，之後才出錯。
- **B6**：**模型不檢查這個條件。** 模型裡出現的錯誤只是把那個立即值位元組（0x0F）當成指令去解的結果，不是因為偵測到「目標不在指令起點」。請記錄硬體實際的反應。
- **L1**：表格的 L1 與 L2（算術）在目前的 profile 下是同一種情形。

## 沒有納入的列

| 編號 | 檢查 | 原因 |
|---|---|---|
| F1 | opcode 沒有對應的處理 | 四個 opcode 都有定義，做不出這種映像 |
| F2 | 指令被程式結尾截斷 | 要改的是 layout 暫存器（程式碼長度），不是映像位元組；layout 由開機載入，測試平台的做法未知 |
| M5 | CSR 位址沒有對應的暫存器 | 取決於板子：請用任何一支測試，把結果 CSR 換成板子上不存在的位址觀察 |
| M6 | MOV 讀取 READ 沒有成功的 MAIN_CSR_DATA_BUF | 取決於硬體是否實作 csr_buf_valid，以及如何讓一次 CSR 讀取失敗 |
| V6 | profile 不允許同一區域內 MOV | 取決於硬體的選項；第 1 層的 c02_mov_const 已經測了「允許」的情形 |
| E2 | 儲存的返回 PC 超出程式 | 需要先派送事件；事件的來源由目標硬體定義 |
| E3 | 事件向量超出程式 | 事件向量是 sideband 設定，載入的方式未知 |

## 回報

請把結果填進 `faults_result_template.csv`（每列一個案例）：三個標記有沒有出現、硬體有沒有回報錯誤、哪一級、之後的行為。
同樣的資訊也可以直接填在 `docs/HW_FAULT_DETECTION.md` 的「硬體偵測級別」欄。

