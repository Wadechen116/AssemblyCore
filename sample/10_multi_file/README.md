# 10_multi_file

專案含三個來源檔，**依序**組譯：`shared.inc`（定義，放第一個）→ `main_part.asm`（Main）→ `peri_part.asm`（Peri）。
這是真實專案的常見結構：共用定義、兩個 domain 各一個檔案。服務 `SVC_TEMP` 是 16 位元溫度（`len=2, little`），Peri 讀 DeviceMap `TEMP_SENSOR`，Main 取出並寫到 CSR。

## 試試看
1. **專案樹**：列出三個 sources、環境檔與情境腳本；雙擊開啟。
2. **跳到定義（F12）**：在 `main_part.asm` 的 `SVC_TEMP`、`TEMP_OUT` 上按 F12 → 跳到 `shared.inc`。
3. **找所有參照（Shift+F12）**：在 `shared.inc` 的 `SVC_TEMP`，列出 Main 與 Peri 兩個檔案的使用處。
4. **重新命名（F2）**：把 `SVC_TEMP` 改成 `SVC_TEMPERATURE` → 三個檔案同步修改並重新建置，仍然零警告。
5. **專案內搜尋（Ctrl+Shift+F）**：搜尋 `TEMP`。
6. **未存檔的緩衝區**：在 `shared.inc` 把 `len=2` 改成 `len=3`（不存檔）→ 問題面板立即出現 **W517**（建置用的是編輯中的文字）。
7. 除錯：在 `handle_temp:` 設中斷點，F5 → 命中後看 **CSRs** 面板，單步後 `0x150`、`0x151` 變成 `0x34`、`0x12`。
8. 在 `shared.inc` 把 `.devmap 0, …` 刪掉 → Peri 檔出現 **E103**／**E309**（DeviceMap 項目未定義）。

## 預期
`asmsim shared.inc main_part.asm peri_part.asm --env temp_env.cfg --scenario multi.scn` → `PASS (2 checks)`。
