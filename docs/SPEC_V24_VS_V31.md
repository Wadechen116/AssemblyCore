# 規格 v24 與 v31 的結構性差異

比較對象：`isa_seq_v1.6_r10_detailed_spec_v24_zhTW.docx`（本專案至今的依據）與 `MicroISA_Spec_v31.pdf`（70 頁），
輔以 `MicroISA_v31_assembly_compiler_handoff.md`。下面每一列都已回到兩份規格的原文核對過；章節號是 v31 的。
「本專案現況」指 AssemblyCore 目前的實作（組譯器、模擬器、反組譯器、IDE），不一定等於 v24 規格本身。

## A. v31 改了、會讓現有實作出錯的地方

| # | 主題 | v24 | v31 | 本專案現況 | 影響 |
|---|---|---|---|---|---|
| A1 | 程式儲存 | MainProgramStorage / PeriProgramStorage 兩塊獨立 latch 陣列 | **同一顆** 32-bit 同步 SRAM（`f_SRAM = 2 × f_ISA`），Main／Peri 各佔一個不重疊的 logical image window；各自 logical byte 0 起算，以 `IMAGE_BASE_WORD` 轉成實體 word 位址；兩個 image 不可共用最後一個 word，各自補 padding（§3.1） | 兩份獨立的 byte 映像檔，沒有實體 SRAM 概念 | **新增**輸出：每個 image 的 32-bit LE word 打包、實體 word 位址、`shared_program_sram_words.hex`、boot 載入計畫、佔用範圍與保留視窗的不重疊證明 |
| A2 | ImageLayout | 「tail／sideband latch region，非 executable、不可由 PC 定址」，六個 offset 欄位 | 同樣是旁路 latch，但明確：**不併入 logical image 的位元組**，不得當成一般 SRAM byte section；欄位寬度與載入介面由 target profile 提供 | 我把六個 offset **附加在映像檔尾端**（欄寬預設 2 byte） | 影像檔去掉尾端；layout 改成獨立輸出（`*.layout.json`）；反組譯器與模擬器改由旁路資料取得 layout（`.bin` 單檔需另附 layout） |
| A3 | MOV 的區域規則 | `MAIN_CONST_POOL` **只能** MOV 到 `MAIN_CSR_DATA_BUF`；`MAIN_CSR_DATA_BUF` 可被 MOV 寫入（CSR 暫存） | `MAIN_CONST_POOL` **只能** MOV 到 `MAIN_SCRATCHPAD`；`MAIN_CSR_DATA_BUF` **只是 MOV 來源**，只有 `EXEC_CSR READ8 / READ_BUF` 會寫它，MOV 不能寫（§5.1） | 完全照 v24：常數搬進 csrbuf；csrbuf 可當 MOV 目的 | **語意相反**。所有範例、測試、範本裡「常數 → csrbuf → ALU」的寫法都要改成「常數 → scratchpad → ALU」；組譯器的區域檢查與資料流規則要改 |
| A4 | CSR 多位元組寫入 | `WRITE_BUF`：資料來源是 `MAIN_CSR_DATA_BUF`，`ptr` 指向 csrbuf；寫 CSR 前要先把資料 MOV 進 csrbuf | `WRITE_SP`：資料來源是 `MAIN_SCRATCHPAD`，`ptr` 指向 scratchpad；`READ_BUF` 不變（寫入 csrbuf）。`csr_op` 編碼不變（00 與 11）（§4.3） | 助記符 `WRITE_BUF`，來源 csrbuf | 助記符改名、來源改為 scratchpad；資料流檢查（W502）的「被提交的緩衝區」改為 scratchpad |
| A5 | softret 的呼叫／返回序列 | （v24 本身沒有 CALL/RET） | 同左，但 csrbuf 不能被 MOV 寫入 | 返回編號用「常數 → csrbuf 暫存 → scratchpad」兩個 MOV，並保留 `ret_stage_base` 兩個 csrbuf 位元組 | 因 A3 失效。可簡化為一個 `MOV CONST → scratchpad[slot]`，不再需要 csrbuf 暫存與其保留區（E306 的 csrbuf 部分） |
| A6 | 分支描述子與事件 | Main：`[15:14]` type、`[13:10]` arg、`[9:8]` 保留、`[7:0]` target；type 11 保留。Peri：`[15:12]` type、`[11:8]` 保留 | Main：`[9]` 變成 **`event_safe`**，`[8]` 保留；type 11 = **`BR.ERET`**。Peri：`[11]` = **`event_safe`**，`[10:8]` 保留；type 8 = **`BR.ERET`**（§7.2、§11.2、§16.5） | 沒有 event_safe 位元，ERET 為非法 | 編碼器、解碼器、`codec.h` 要改；`event_safe` 是描述子的一部分，**不同 safe 值不可共用描述子**；組譯語法要能表示它（例如 `BR.COND cond, label, safe`）與 `BR.ERET` |
| A7 | 硬體事件派送 | **完全沒有**（v24 全文找不到 ERET / event_safe / Event Pending） | 新增整章 §16：每個 domain 各有 pending、`handler_active`、全寬 `resume_pc`、事件向量；**只有**在 `event_safe=1` 的一般 BR 成功退休、且所有 runtime guard 成立時才派送；handler 不可巢狀；`BR.ERET` 回到全寬 resume PC；沒有 context bank，ALU／scratchpad／RX 不會被自動保存 | 無此功能 | **全新功能**：語法（向量、handler）、驗證（向量落在 opcode 邊界、ERET 只在 handler、Cascade 內不可 event_safe、handler 寫入集合與前景 live-out 不衝突）、模擬器（pending 注入、派送、ERET）、事件延遲不可宣稱有界的條件 |
| A8 | PC 寬度與分支目標 | 描述子 `target_pc_byte` 為 8 bit；我據此把程式碼上限定為 256 bytes | 8-bit 分支目標**不等於**完整 PC 寬度；PC 寬度（及 resume PC 寬度）由 target profile 決定；普通 BR 目標 > 255 必須報 `BR_TARGET_UNREPRESENTABLE`，**不可截斷、不可自創長跳躍**（§2、handoff §3.3） | 程式碼大小硬限 256 | 程式碼上限改由 profile（容量、PC 寬度）決定；「目標 > 255」變成獨立錯誤 |
| A9 | Frontend 與時序 | 簡略（全文只 2 處提到 Frontend）；§14 以引擎週期估計 | 規範七態 Frontend（`CMD_HOLD → OP_GET → DESC_GET → DEC → FIRE → EXEC`、`CMD_FAULT`）、共享 SRAM 2× 排程與 1× 回應、跨 word 取指必須等 matching tag；`BR≈1`、`MOV≈1+len` 等**只是引擎層規劃值，不是完整指令延遲，也不是 WCET signoff**（§14、§16.9） | 每指令一個固定延遲表，WCET 報告標示 placeholder | Profile 要新增取指／描述子／解碼／派發／退休／SRAM 仲裁上界；沒有有界上界時 WCET 只能標「planning-only」；跨 word 讀取要能被參考模型測試 |

## B. v31 新增的要求（v24 沒有對應條文）

| # | 主題 | v31 的要求 | 現況 |
|---|---|---|---|
| B1 | 區段對齊 | 由 profile 決定，**不可猜**（不一定 2/4 byte 對齊） | 目前一律依表項寬度對齊 |
| B2 | 描述子序列化位元組序 | 必須由核准的 profile 確認（v24 契約為小端） | 假設小端（已列為假設） |
| B3 | DeviceMap 實體格式 | entry 位元組數與位元排列由 profile 決定，不可因為剛好 8 bit 就宣告 bit7／bit[6:0] | 我假設 bit7 = bus、bit[6:0] = 位址（已列為假設）；需加 `device_map_entry_bytes` |
| B4 | 產物清單 | `*.logical.bin`、`*.layout.json`、`section_map.json`、`descriptor_map.json`、`services.json`、`event_vectors.json`、`shared_program_sram_words.hex`、`boot_load_plan.json`、`memory_proof.json`、`validation_report.json`、`wcet_report.json`、`build_manifest.json` | 只有 `.bin`、`.hex`、`.lst`、`.map`、`.dbg.json` |
| B5 | 就緒等級 | L0 結構、L1 二進位核准、L2 時序核准；沒有核准 profile 不可宣稱可開機的 production image；沒有有界延遲不可宣稱 WCET signoff | 無分級；輸出一律當成可用 |
| B6 | 新驗證項 | 位元組序（感測器 big-endian 進 ALU little-endian 必須轉換，`E_SENSOR_BYTE_ORDER`）、Timer 啟動先於等待、Poll arm 要有成功讀 RX[0] 的路徑、服務 payload 長度與 wire order 契約、CSR READ_BUF 失敗後不可使用 readback、同一 domain 內映像與 SRAM 視窗的「保留視窗」不重疊 | 資料流檢查有 11 條規則（W501–W511），上述多數沒有 |
| B7 | `csr_buf_valid_q[31:0]` | 提議性的選配完整性保護，**不可假設硬體有** | 不涉及 |
| B8 | 診斷命名 | 建議的 `E_*` 名稱與必須涵蓋的拒絕條件清單（handoff §12） | 我們用數字代碼（E1xx…、W5xx）；需建立對照 |

## C. 兩版相同、不需要動的地方

| 主題 | 說明 |
|---|---|
| 指令位元組 | `[7:6]` opcode 家族、`[5:0]` arg；Main = CSR／BR／MOV／ALU，Peri = CSR／BR／PUB／XFER；各指令長度規則相同 |
| 區段排列順序 | Main：碼、Move、Branch、CsrOneByte、CsrMultiByte、Alu、常數池；Peri：碼、XferOneByte、XferMultiByte、Publish、Branch、CsrOneByte、DeviceMap，兩版相同 |
| MOV 描述子欄位 | `src_region`／`dst_region`／`src_offset`／`dst_offset`／`len_m1` 位置不變；區域編號 0x0–0x8 不變 |
| ALU | 描述子欄位、運算元 16-bit 小端、輸出語意（ADD 精確 17 bit、SHL 精確 24 bit…）不變；**子運算碼數值兩版都沒定義**，仍是 profile 待補（我們的 ASSUMED-1 維持） |
| ConditionMap | Main 0x0–0xE 十五個條件名稱相同；`PERI_NO_ERROR = done && !error`（Peri 的 `IF_NO_ERROR` 只看 `!error`，兩版相同） |
| Cascade | 累進區塊（BEGIN k 執行區塊 0..k）、END 索引檢查、不可巢狀，兩版相同 |
| Peri XFER | OneByte A-prefix（`addr_len` 0/1/2、`xfer_addr10`）、MultiByte 只准 READ、`ptr+len ≤ 32`，兩版相同 |
| DeviceMap／PUB | 邏輯欄位相同；`dev_sel` 與 `service_id` 不可混用 |
| ResultSlot | 欄位、`PERI_RESULT_META` 版面（0 = service id、4 = valid、5 = overrun）、清除 valid 的同週期不可再接受發佈（最早 T+1）相同。「已滿則丟新、保留舊」v24 已是這個行為（`overrun_sticky`），v31 只是明文禁止 Latest Wins／FIFO |
| Poll／Timer／Error／Done | 章節結構與基本規則相同（v31 把用詞寫得更嚴，例如 `timer_done=0` 不代表計時器已啟動） |

## D. 對我們各部分的影響（由大到小）

| 部分 | 要改的 | 規模 |
|---|---|---|
| 組譯器 `lower`／`layout` | A3 區域規則、A4 助記符、A5 softret、A6 編碼與語法、A8 程式碼容量、A1／A2 的輸出（影像去尾、layout 旁路、SRAM 打包、boot 計畫、證明）、B1／B3 profile 欄位 | **大** |
| 資料流檢查 | csrbuf 語意改變（只由 READ 定義）、WRITE_SP 來源為 scratchpad、B6 新規則、事件 liveness | 中 |
| 模擬器 | MOV 區域合法性、WRITE_SP、事件 pending／派送／ERET、旁路 layout、A9 時序模型（最大的未知數） | **大** |
| 反組譯器 | 旁路 layout 輸入、新助記符、event_safe／ERET 解碼 | 中 |
| IDE | 範本／範例改寫、除錯面板（csrbuf 區域標示、事件）、新增的映像／SRAM／layout 檢視（正好是 P2 的「映像區段檢視」） | 中 |
| 測試與範例 | 凡用到「常數 → csrbuf → ALU」或 `WRITE_BUF` 的全部要改寫，預期是數十處 | 中 |

## E. 還是未知、仍需核准 profile 才能補的項目（v31 沒給）

ALU 各子運算碼的數值；CSR 位址表與位元（poll、timer、error、done 控制）；DeviceMap entry 位元組數與排列；各 SRAM 深度、base word、image 容量、PC 寬度；ImageLayout 旁路欄寬與載入介面；事件編號、優先權、向量來源；fault／reset 政策；各引擎延遲上界。這些我們繼續以 profile 欄位承載，缺的就明確報「缺口」而不是猜。
