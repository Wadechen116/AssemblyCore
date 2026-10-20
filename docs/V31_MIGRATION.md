# v31 遷移檢查清單

依據 `docs/SPEC_V24_VS_V31.md`。每一項完成並有測試後勾掉（`[x]`）；階段順序即實作順序。

## 階段 1：會讓舊程式出錯的語意與編碼（A3、A4、A5、A6）
- [x] A3-1 MOV：`MAIN_CONST_POOL` 的唯一合法目的是 `MAIN_SCRATCHPAD`（直接到 ALU／csrbuf 皆拒絕）
- [x] A3-2 MOV：`MAIN_CSR_DATA_BUF` 不可當目的（只讀來源）
- [x] A3-3 MOV：合法目的只有 scratchpad、`ALU_OPA`、`ALU_OPB`
- [x] A4-1 `EXEC_CSR WRITE_SP`（來源 scratchpad，`ptr` 指向 scratchpad），舊的 `WRITE_BUF` 以明確錯誤訊息拒絕
- [x] A4-2 `READ_BUF` 不變（寫入 csrbuf），`READ8` 只寫 csrbuf
- [x] A4-3 資料流：WRITE_SP 檢查 scratchpad 範圍已定義；csrbuf 只由 READ 定義
- [x] A5 softret：返回編號改為單一 `MOV CONST → scratchpad`，移除 csrbuf 暫存與其保留區（E306 的 csrbuf 部分、`ret_stage_base`）
- [x] A6-1 `MainBranchDesc` bit9 = `event_safe`、type 11 = `BR.ERET`
- [x] A6-2 `PeriBranchDesc` bit11 = `event_safe`、type 8 = `BR.ERET`
- [x] A6-3 語法：`BR.COND cond, label, safe`／Peri 分支同；`BR.ERET`；`event_safe` 納入描述子去重鍵
- [x] A6-4 驗證：`event_safe` 只能在一般 BR；Cascade 內與 ERET 一律 0；ERET 描述子欄位必須為 0
- [x] 階段 1 的測試、範例、範本、文件改寫完成（組譯器 220、模擬器 114、反組譯器 38、服務層 140、UI 33；ASM_SYNTAX 第 4、6、7、9 節已更新）

## 階段 2：輸出與 profile（A8、A1、A2、B1、B3、B4）
- [x] A8-1 程式碼容量與 PC 寬度由 profile 決定（不再硬限 256）
- [x] A8-2 普通 BR 目標 > 255 → 獨立錯誤（不截斷）
- [x] A2-1 映像去掉尾端 ImageLayout；`DomainImage` 帶獨立的 layout（六個 offset）
- [x] A2-2 `*.layout.json` 輸出；欄寬檢查（`E_LAYOUT_OFFSET_WIDTH`）
- [x] A1-1 每個 image 打包成 32-bit 小端 word（各自補 padding，不共用最後 word）
- [x] A1-2 實體 SRAM 配置（`IMAGE_BASE_WORD`、深度）與「佔用範圍＋保留視窗」不重疊證明
- [x] A1-3 `shared_program_sram_words.hex`、`boot_load_plan.json`、`memory_proof.json`
- [x] B1 區段對齊由 profile 決定
- [x] B2 描述子序列化位元組序由 profile 宣告
- [x] B3 DeviceMap `entry_bytes` 與位元排列由 profile 決定
- [x] B4 其餘產物：`section_map.json`、`descriptor_map.json`、`services.json`、`event_vectors.json`、`validation_report.json`、`build_manifest.json`
- [x] 跨 word 取指的參考函式與測試（opcode 在 lane3、inline 在下一 word lane0）

## 階段 3：反組譯器與模擬器跟進
- [x] 反組譯器：layout 由旁路資料取得（`--layout`／JSON）；新助記符；`event_safe`／ERET 解碼
- [x] 往返測試（組譯→反組譯→組譯）在 v31 規則下仍位元組相同
- [x] 模擬器：MOV 區域合法性（含 csrbuf 唯讀）、`WRITE_SP`、旁路 layout
- [x] 環境檔／情境腳本／IDE 除錯面板相容

## 階段 4：硬體事件（A7）
- [x] 語法：事件向量與 handler（`.vector`）
- [x] 驗證：向量落在本 domain 的 opcode 邊界、ERET 只在 handler、Cascade／critical commit 內不可 safe、handler 所有正常出口都有 ERET
- [x] 靜態警告：handler 覆寫前景 live 的 ALU／scratchpad／RX／Poll／Timer／Error
- [x] 模擬器：event pending 注入、派送（只在 safe BR 退休）、全寬 resume PC、ERET、不巢狀、`fault_bad_eret_context`
- [x] 事件延遲的分項報告與「無上界」判定

## 階段 5：新驗證項（B6、B8）
- [ ] 感測器位元組序（big-endian 進 ALU）→ `E_SENSOR_BYTE_ORDER`
- [ ] Timer 啟動先於等待；Poll arm 前有成功讀 RX[0] 的路徑
- [ ] 服務契約：payload 長度與 wire order；未知 service 政策
- [ ] CSR READ_BUF 失敗後不可使用 readback
- [ ] 診斷代碼與 v31 建議的 `E_*` 名稱對照表（B8）

## 階段 6：時序與就緒等級（A9、B5）
- [ ] profile 加入 Frontend／SRAM 仲裁／引擎延遲欄位
- [ ] WCET 報告：缺上界時標「planning-only」，不給 signoff
- [ ] 就緒等級 L0／L1／L2 判定與報告；缺口明列

## 階段 7：IDE、範例與文件，然後繼續 P2
- [ ] 範例、範本、測試資料全部改寫成 v31 寫法
- [ ] IDE：詞彙表／補全／上色／懸停 跟上新助記符與 `safe`／`BR.ERET`；除錯面板標示 csrbuf 唯讀、事件
- [ ] IDE：映像／SRAM／layout 檢視（P2 D-5）
- [ ] 文件：ASM_SYNTAX、SIM、DISASM、DEBUG_INFO、EXTENDING、LANG_SERVICE、IDE 文件更新
- [ ] 專案記憶更新；Linux（CMake＋QMake）與 Windows MSVC 2026 全部重新驗證
- [ ] 回到 P2（D-5～D-9、C9、C14）

## 貫穿所有階段：易用性（使用者要求：建置完成後要好操作、易於使用）
每個階段的功能都要同時滿足下列項目，最後一併驗收；沒做完的不算該階段完成。

- [ ] U1 **每個錯誤都說明怎麼修**：訊息含原因、規格依據（章節）、建議寫法（例如 `WRITE_BUF` → 「v31 已改名為 `WRITE_SP`，資料來源是 MAIN_SCRATCHPAD」）
- [ ] U2 **舊寫法的遷移協助**：偵測 v24 寫法並給精確提示；提供自動轉換（`asmc --upgrade-v24` 輸出改寫後的原始碼，IDE 內「升級到 v31」指令與預覽差異）
- [ ] U3 **IDE 快速修復（C10）**：對 U1 的每個錯誤提供一鍵修正（改名、補 MOV 經過 scratchpad、加 `safe`…）
- [ ] U4 **Profile 不再是門檻**：內建「v31 規劃用 profile」讓新手開箱即用；缺口以清單面板呈現（缺什麼、影響什麼、怎麼補），而不是一堆錯誤
- [ ] U5 **就緒等級一眼看懂**：狀態列顯示 L0／L1／L2，點開看缺口與可做的事；「能做什麼／不能宣稱什麼」用白話說明
- [ ] U6 **新使用者路徑**：新專案精靈（範本：最小程式、Peri 讀感測器＋Main 取用、含事件的範例）、範例庫（每個範例附說明）、歡迎頁、內建說明（指令速查、快捷鍵表、v31 與 v24 差異）
- [ ] U7 **輸出可理解**：建置結果摘要（程式大小、各表使用量、SRAM 佔用、WCET 狀態、事件延遲狀態）一頁看完，並說明每個數字的意義；各產物檔案的用途清單
- [ ] U8 **預設值合理、設定可發現**：設定集中在一個對話框，每個選項有說明；危險選項（例如降低資料流檢查）有警示
- [ ] U9 **操作可預期**：所有破壞性動作可復原或有確認；長時間工作有進度與取消；錯誤不會讓 IDE 失去未存檔內容
- [ ] U10 **中英文同步**：所有新增的介面字串、錯誤提示與說明都有繁中翻譯，翻譯完整性檢查維持通過
- [ ] U11 **文件以使用者角度寫**：「快速上手」「從 v24 升級」「常見錯誤與修法」三篇，與介面內說明一致
- [ ] U12 **最終易用性驗收**：以「從零新建專案到組譯、除錯、輸出映像」走完整流程，逐步檢查是否需要看文件才能繼續；不能的地方就改介面
