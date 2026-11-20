# 語言服務層（給編輯器與 IDE 用）

不依賴任何 UI 的純標準 C++，位於核心內，IDE 與日後的 LSP 伺服器共用。前三個部分都**不需要組譯成功**，輸入到一半、有錯誤時仍可用。

（2026-11-20 對照 `lang.h`、`index.h`、`diagnostics.h` 核對。）

| 部分 | 標頭 | 用途 |
|---|---|---|
| 診斷欄位範圍 | `asmcore.h` 的 `SrcLoc::col/endCol` | 波浪線標在正確的字上。預設標整條陳述（去掉標籤與註解）；訊息中有 `'符號'` 且出現在該行時縮到那個符號。0 表示未知 |
| 語彙與 token | `lang.h` | `Language::defaults(profile)` 給出所有助記符、指示字、區域、條件、子運算、具名參數（含簽名、說明、規格章節）；`find()`／`all()` 依種類與 domain 查詢；`tokenizeLine()` 把一行切成 token（上色用；傳入已知符號集合時，不認得的字標成 `Unknown`）；`cursorAt()` 回答「游標在哪個指令的第幾個運算元、運算元內第幾個字、是否在 `=` 之後、已輸入的字首」（補全、參數提示用）。`Entry` 另有 `insert`（補全時插入的文字）與 `group`（子運算的細分類：`alu`、`alumod`、`csr`、`xfer`、`branchattr`） |
| 索引與來源對應 | `index.h` | `Index::build()` 由 token 建立符號定義／參照（標籤、程序、`.equ`、`.csr`、`.service`、`.const`、`.devmap`、WCET 查詢名稱），支援跳到定義（`at`、`find`）、找參照（`occurrences`）、重新命名檢查（`renameProblem`）、未定義符號（`undefined`）；`SourceMap`（需要一份組譯結果）回答「這一行產生哪些位址（inline 展開會有多個；`CALL` 那一行對應到它的呼叫序列）」（`instrsForLine`）、「這個位址來自哪一行」（`instrAt`）、「中斷點該移到哪一行」（`nearestCodeLine`）、「這個檔案哪些行有程式碼」（`codeLines`） |
| 診斷代碼表 | `diagnostics.h` | `diagInfo(code)`／`allDiagnostics()`／`explainDiagnostic(code)`：每個代碼的說明、修法、規格 v31 對應的名稱（`E_*`）。`asmc --explain CODE`、驗證報告與 IDE 共用同一張表（`core/src/diag_names.cpp`） |

## 規則

- 區域與分支條件的詞彙直接取自 `isa.h` 的清單，規格改了不用兩邊同步；助記符、指示字、子運算在 `lang.cpp` 的 `Language::defaults()` 逐項登錄，新增指令時要一起加。要加擴充詞彙：`Language::add(Entry)`。
- 範圍：標籤與程序名屬於 domain（`.main`／`.peri`），`.proc` 內的標籤屬於該程序，其他定義是全域的；解析順序是「程序區域 → domain → 全域」。
- `.include`：把含入的檔案依 `.include` 的位置排在 `sources` 裡傳入（共用檔案放前面），或給 `Index::build()` 一個 `IncludeResolver` 由它取檔；未存檔的編輯器緩衝區直接傳文字即可。
- 這一層**不做**型別或資料流判斷；那是組譯器的工作（`Result::diags`）。

## 尚未有的部分

原計畫的語言服務項目中，下列三項在核心的公開標頭裡沒有對應的介面：資料流結果的查詢（L7；資料流檢查本身在組譯器，結果只以 W5xx 診斷呈現）、環境檔的結構描述（L8；環境檔仍是純文字，模擬器的硬體參數另有目錄 `sim_params.h`）、格式化（L9）。
