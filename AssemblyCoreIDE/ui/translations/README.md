# 介面翻譯

- `acide_zh_TW.ts`：繁體中文翻譯來源（Qt Linguist 格式）。原文是英文，所以沒有 `acide_en.ts`。
- `acide_zh_TW.qm`：由 `.ts` 編出的二進位檔，**提交進版本庫**（CMake 與 qmake 都直接把它內嵌，不需要建置時執行 `lrelease`）。
- `qtbase_zh_TW.qm`：Qt 自己的繁中翻譯（檔案對話框、訊息框的按鈕），取自 Qt 6.5.3 的 `translations/`。
- `translations.qrc`：把上面兩個 `.qm` 內嵌成 `:/i18n/`。

## 新增或修改了介面字串之後

```
cmake --build <建置目錄> --target acide_update_translations     # lupdate + 套用已有翻譯 + lrelease
```
然後在 `acide_zh_TW.ts` 補上新字串的翻譯（`type="unfinished"` 的項目），再執行一次同一個目標。
`tests/check_translations.py`（ctest 會跑）與 UI 測試會檢查：每個 `tr("…")` 都在 `.ts` 裡、沒有未完成的項目、`%1` 等佔位符一致、
`.qm` 與 `.ts` 內容相同。

## 新增一種語言

複製 `acide_zh_TW.ts` 為 `acide_<代碼>.ts` 並翻譯，把 `.qm` 加進 `translations.qrc` 與 `ui/CMakeLists.txt`，
在 `ui/i18n.cpp` 的 `availableLanguages()` 加一列。

## 範圍

介面（選單、面板、對話框、狀態列）已翻譯。組譯器的診斷訊息、模擬器的停止原因與說明字串、指令詞彙的說明文字仍是英文。
