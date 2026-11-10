# 錯誤偵測對照表（給硬體團隊）

> 由 `docs/gen_hw_fault_table.py` 產生，請改那支程式，不要直接改這份文件。

每一項是指令的 byte 或執行時可能違反的規則。前幾欄是**目前軟體的行為**（規格出處、模擬器何時報錯、反組譯／組譯怎麼報），
最後兩欄請硬體團隊填：**RTL 實際在哪一級偵測**，以及備註。填好後模擬器會依此調整報錯的時機，讓它和 RTL 一致。

## 類別

| 類別 | 意思 | 項目數 |
|---|---|---|
| **A** 不合法的編碼 | bit 寫不成任何指令（保留位元、保留值、索引超出） | 30 |
| **B** 編碼合法，但違反規格 | 寫得出指令，但內容超出硬體能做的範圍（範圍、方向、目標） | 17 |
| **C** 執行當下的狀態 | 和 byte 無關，要看執行到那裡時的狀態 | 8 |

規格出處是 `docs/MicroISA_v31_assembly_compiler_handoff.md` 的章節。偵測級別的選項對應規格 §8 的 Frontend 狀態：

| 選項 | 說明 |
|---|---|
| dec | CMD_DEC（解碼級，執行前） |
| engine | 執行單元（engine，執行時） |
| pc | PC 更新／轉向時 |
| backend | CSR backend／I2C 控制器 |
| load | 載入／啟動時（sideband） |
| none | 不偵測（行為未定義） |
| other | 其他（寫在備註） |

## 需要優先回答的問題

- **F3**（pc 超出程式（程式跑出結尾），fault_pc_out_of_code）：和 F2 共用同一個錯誤碼；硬體是否區分「指令被截斷」與「跑出結尾」？
- **M5**（CSR 位址沒有對應的暫存器，fault_csr_unmapped）：硬體對未對應的 CSR 位址是回錯誤、忽略寫入、還是讀回固定值？
- **M6**（MOV 讀取 EXEC_CSR READ 沒有成功的 MAIN_CSR_DATA_BUF byte，fault_csrbuf_invalid）：硬體是否實作 csr_buf_valid？
- **B4**（條件碼是保留值，或 profile 定義的條件沒有給定義，fault_cond_undefined）：PROFILE_COND0 在硬體上由誰定義？沒定義時的行為？
- **B5**（分支目標超出程式範圍，fault_branch_target）：重點：硬體是在解碼時就檢查目標（不論跳不跳），還是只在跳躍、更新 PC 時檢查？
- **B6**（分支目標落在指令中間（不是指令起點），（無））：重點：硬體能否偵測？若不能，模擬器維持不檢查；若能，模擬器要新增錯誤碼。
- **R4**（分支目標超出程式範圍，fault_branch_target）：重點：同 B5，解碼時檢查還是跳躍時檢查？
- **E2**（儲存的返回 PC 超出程式，fault_branch_target）：錯誤碼與 B5 共用；實際上是狀態問題，硬體是否有獨立的錯誤？
- **E3**（事件向量超出程式，fault_event_vector）：事件向量是 sideband 設定；硬體在載入時檢查，還是派送時？

## 共通

| 編號 | 檢查 | 錯誤碼 | 類別 | 規格 | 模擬器何時報錯 | 反組譯／組譯 | 硬體偵測級別 | 備註 |
|---|---|---|---|---|---|---|---|---|
| F1 | opcode 沒有對應的處理（baseline 兩個 domain 的 4 個 opcode 都有定義，實際不會發生；保留給擴充） | `fault_opcode` | A | §3.1 | 解碼時 | D201 |  |  |
| F2 | 指令被程式結尾截斷（缺第二個 byte 或位址 byte） | `fault_pc_out_of_code` | A | §3.2 | 執行該指令時 | D201 |  |  |
| F3 | pc 超出程式（程式跑出結尾） | `fault_pc_out_of_code` | A | §3.2、§12 | 要取下一條指令時 | 組譯 E303（反組譯的規格階段） |  |  |
| F4 | 描述子索引超出表格 | `fault_desc_index` | A | §2.5 | 取描述子時 | D201 |  |  |

## Main EXEC_CSR

| 編號 | 檢查 | 錯誤碼 | 類別 | 規格 | 模擬器何時報錯 | 反組譯／組譯 | 硬體偵測級別 | 備註 |
|---|---|---|---|---|---|---|---|---|
| M1 | CsrOneByte 描述子保留位元非零 | `fault_desc_reserved` | A | §4.2 | 執行該指令時 | D201 |  |  |
| M2 | READ8 目的指標 bit 7:5 非零 | `fault_desc_reserved` | A | §4.2 | 執行該指令時 | D201 |  |  |
| M3 | CsrMultiByte 的 csr_op 是保留值（buffer RMW） | `fault_desc_reserved` | A | §4.3 | 執行該指令時 | D201 |  |  |
| M4 | CsrMultiByte 的 ptr + len 超過 32 byte | `fault_range` | B | §4.3 | 執行該指令時 | D205 |  |  |
| M5 | CSR 位址沒有對應的暫存器 | `fault_csr_unmapped` | C | §1、§9.2（CSR contract 由 target profile 定義） | 存取 CSR backend 時（環境檔開啟時才報） | 無法靜態判斷（取決於 profile／環境） |  |  |
| M6 | MOV 讀取 EXEC_CSR READ 沒有成功的 MAIN_CSR_DATA_BUF byte | `fault_csrbuf_invalid` | C | §4.3（csr_buf_valid，選用） | MOV 執行時（profile 開啟 csr_buf_valid 才檢查） | 無法靜態判斷 |  |  |

## Main BR

| 編號 | 檢查 | 錯誤碼 | 類別 | 規格 | 模擬器何時報錯 | 反組譯／組譯 | 硬體偵測級別 | 備註 |
|---|---|---|---|---|---|---|---|---|
| B1 | MainBranchDesc 保留位元非零 | `fault_desc_reserved` | A | §4.5 | 執行該指令時 | D201 |  |  |
| B2 | event_safe 設在 CASCADE 或 ERET 描述子上 | `fault_desc_reserved` | A | §4.5、§7 | 執行該指令時 | D201 |  |  |
| B3 | BR.ERET 描述子的 arg 或 target 非零 | `fault_desc_reserved` | A | §4.5、§7 | 執行該指令時 | D201 |  |  |
| B4 | 條件碼是保留值，或 profile 定義的條件沒有給定義 | `fault_cond_undefined` | A | §4.5、§9 | 執行該指令時 | D201（依模擬器當下的條件表） |  |  |
| B5 | 分支目標超出程式範圍 | `fault_branch_target` | B | §3.3、§4.5 | 只有條件成立、真的跳躍時才報 | D205（不論跳不跳）；組譯會拒絕 |  |  |
| B6 | 分支目標落在指令中間（不是指令起點） | `（無）` | B | §3.2、§3.3 | 不檢查：會把 inline byte 當 opcode 繼續執行 | 反組譯 D203（程式層級，Decoder 面板不顯示） |  |  |
| B7 | cascade 進行中又遇到 CASCADE_BEGIN | `fault_nested_cascade` | C | §4.6 | CASCADE_BEGIN 執行時 | 無法靜態判斷 |  |  |
| B8 | cascade block0 目標超出程式 | `fault_branch_target` | B | §4.6 | CASCADE_BEGIN 執行時 | D205 |  |  |
| B9 | CASCADE_END 但沒有進行中的 cascade | `fault_cascade_end_without_begin` | C | §4.6 | CASCADE_END 執行時 | 無法靜態判斷 |  |  |
| B10 | CASCADE_END 的 index 和目前步數不符 | `fault_cascade_step_mismatch` | C | §4.6 | CASCADE_END 執行時 | 無法靜態判斷 |  |  |
| B11 | cascade 出口目標超出程式 | `fault_branch_target` | B | §4.6 | CASCADE_END 執行時 | D205 |  |  |

## Main MOV

| 編號 | 檢查 | 錯誤碼 | 類別 | 規格 | 模擬器何時報錯 | 反組譯／組譯 | 硬體偵測級別 | 備註 |
|---|---|---|---|---|---|---|---|---|
| V1 | MainMoveDesc 保留位元非零 | `fault_desc_reserved` | A | §4.1 | 執行該指令時 | D201 |  |  |
| V2 | 區域代碼是保留值（9～15） | `fault_desc_reserved` | A | §4.1 | 執行該指令時 | D201 |  |  |
| V3 | 來源區域不能讀（ALU_OPA／ALU_OPB） | `fault_mov_region` | B | §4.1 | 執行該指令時（任何寫入之前） | D205；組譯 E202 |  |  |
| V4 | 目的區域不能寫（MAIN_CSR_DATA_BUF、結果槽、ALU_OUT、狀態、常數區、META） | `fault_mov_region` | B | §4.1 | 執行該指令時（任何寫入之前） | D205；組譯 E202 |  |  |
| V5 | 同一區域內來源與目的重疊 | `fault_mov_overlap` | B | §4.1 | 執行該指令時（任何寫入之前） | D205；組譯 E202 |  |  |
| V6 | profile 不允許同一區域內 MOV | `fault_mov_overlap` | B | §4.1、§9 | 執行該指令時 | D205 |  |  |
| V7 | MAIN_CONST_POOL 只能搬到 MAIN_SCRATCHPAD | `fault_mov_region` | B | §4.1 | 執行該指令時 | D205；組譯 E202 |  |  |
| V8 | ALU 運算元不是 offset 0、長度 2 | `fault_mov_range` | B | §4.1 | 執行該指令時 | D205；組譯 E202 |  |  |
| V9 | 來源或目的範圍超出區域大小 | `fault_mov_range` | B | §4.1（precheck 在任何寫入之前，不可部分寫入） | 執行該指令時（任何寫入之前） | D205；組譯 E202 |  |  |
| V10 | 從 PERI_RESULT_SLOT 搬，但結果槽沒有資料 | `fault_slot_not_valid` | C | §5.1 | MOV 執行時 | 無法靜態判斷 |  |  |

## Main ALU

| 編號 | 檢查 | 錯誤碼 | 類別 | 規格 | 模擬器何時報錯 | 反組譯／組譯 | 硬體偵測級別 | 備註 |
|---|---|---|---|---|---|---|---|---|
| L1 | 子運算碼沒有由 target profile 指定 | `fault_desc_reserved` | A | §4.4、§9 | 執行該指令時 | D201 |  |  |
| L2 | arith_op／logic_op 是保留值 | `fault_desc_reserved` | A | §4.4 | 執行該指令時 | D201 |  |  |
| L3 | CMP／LOGIC 描述子保留位元非零 | `fault_desc_reserved` | A | §4.4 | 執行該指令時 | D201 |  |  |
| L4 | EQ／NE 設了 signed_mode | `fault_desc_reserved` | A | §4.4 | 執行該指令時 | D201 |  |  |
| L5 | 移位量不在 1..8 | `fault_desc_reserved` | A | §4.4 | 執行該指令時 | D201 |  |  |

## Peri EXEC_CSR

| 編號 | 檢查 | 錯誤碼 | 類別 | 規格 | 模擬器何時報錯 | 反組譯／組譯 | 硬體偵測級別 | 備註 |
|---|---|---|---|---|---|---|---|---|
| P1 | Peri 使用 MultiByte CSR（baseline 不支援） | `fault_peri_csr_multibyte` | A | §4.2 | 執行該指令時 | D201 |  |  |
| P2 | Peri 使用 READ8（baseline 不支援） | `fault_peri_csr_read` | A | §4.2 | 執行該指令時 | D201 |  |  |
| P3 | CsrOneByte 描述子保留位元非零 | `fault_desc_reserved` | A | §4.2 | 執行該指令時 | D201 |  |  |

## Peri BR

| 編號 | 檢查 | 錯誤碼 | 類別 | 規格 | 模擬器何時報錯 | 反組譯／組譯 | 硬體偵測級別 | 備註 |
|---|---|---|---|---|---|---|---|---|
| R1 | PeriBranchDesc 保留位元非零 | `fault_desc_reserved` | A | §4.11 | 執行該指令時 | D201 |  |  |
| R2 | BR.ERET 描述子的 event_safe 或 target 非零 | `fault_desc_reserved` | A | §4.11、§7 | 執行該指令時 | D201 |  |  |
| R3 | 條件碼是保留值 | `fault_cond_undefined` | A | §4.11 | 執行該指令時 | D201 |  |  |
| R4 | 分支目標超出程式範圍 | `fault_branch_target` | B | §4.11 | 只有條件成立、真的跳躍時才報 | D205（不論跳不跳） |  |  |

## Peri EXEC_PUB

| 編號 | 檢查 | 錯誤碼 | 類別 | 規格 | 模擬器何時報錯 | 反組譯／組譯 | 硬體偵測級別 | 備註 |
|---|---|---|---|---|---|---|---|---|
| U1 | PeriPublishDesc 保留位元非零 | `fault_desc_reserved` | A | §4.10 | 執行該指令時 | D201 |  |  |
| U2 | service_id 為 0（保留） | `fault_publish_service` | A | §4.10 | 執行該指令時 | D201 |  |  |
| U3 | src + len 超過 PERI_RX_BUF（32 byte） | `fault_range` | B | §4.10 | 執行該指令時 | D205 |  |  |

## Peri EXEC_XFER

| 編號 | 檢查 | 錯誤碼 | 類別 | 規格 | 模擬器何時報錯 | 反組譯／組譯 | 硬體偵測級別 | 備註 |
|---|---|---|---|---|---|---|---|---|
| X1 | addr_len 為 3（保留） | `fault_desc_reserved` | A | §4.7、§4.8 | 執行該指令時 | D201 |  |  |
| X2 | READ 目的指標 bit 7:5 非零 | `fault_desc_reserved` | A | §4.7 | 執行該指令時 | D201 |  |  |
| X3 | MultiByte WRITE（baseline 不支援） | `fault_xfer_multibyte_write` | A | §4.8 | 執行該指令時 | D201 |  |  |
| X4 | MultiByte 的 ptr + len 超過 PERI_RX_BUF | `fault_range` | B | §4.8 | 執行該指令時 | D205 |  |  |
| X5 | DeviceMap 沒有 dev_sel 指到的項目 | `fault_dev_undefined` | B | §4.9 | 執行該指令時（讀 DeviceMap 時） | D205 |  |  |
| X6 | DeviceMap 項目的 bus_type 是保留值 | `xfer_fault_reserved_bus_type` | A | §4.9 | 執行該指令時（讀 DeviceMap 時） | D201；表格層級另報 D301 |  |  |

## 事件

| 編號 | 檢查 | 錯誤碼 | 類別 | 規格 | 模擬器何時報錯 | 反組譯／組譯 | 硬體偵測級別 | 備註 |
|---|---|---|---|---|---|---|---|---|
| E1 | 不在事件處理中卻執行 BR.ERET | `fault_bad_eret_context` | C | §7.3 | BR.ERET 執行時 | 無法靜態判斷 |  |  |
| E2 | 儲存的返回 PC 超出程式 | `fault_branch_target` | B | §7 | BR.ERET 執行時 | 無法靜態判斷（PC 是執行時存的） |  |  |
| E3 | 事件向量超出程式 | `fault_event_vector` | C | §7.3 | 事件派送時 | D204（向量不在指令起點） |  |  |
