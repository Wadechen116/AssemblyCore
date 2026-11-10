; 範例 16：故意出錯的程式 —— 讓「問題」面板、編輯器、Decoder 面板一次呈現各類問題
;
; 這份原始碼可以建置（只有警告），因為組譯器會拒絕 A、B 類錯誤：它們不可能從原始碼產生，
; 只會出現在燒進去的 byte 和原始碼不一致的時候（燒錯檔、bit 翻轉、燒錄器出錯）。
; 所以這個範例另外附一份「故意改壞」的燒錄檔 burned/showcase_bad.eeprom.bin，專案用 debug_eeprom 指定它，
; 開啟專案按 F5，解碼器讀的就是那份壞掉的燒錄檔。
;
; 會看到的問題（類別：A 不合法的編碼、B 編碼合法但違反規格、C 執行當下的狀態）：
;   行  原始碼                      建置           燒錄檔裡被改壞的地方            執行時
;   26  MOV scratch -> ALU_OPA      W502 警告
;   27  MOV 結果槽 -> scratch       W505 警告                                       Main 錯誤 C（結果槽沒有資料）
;   28  ALU ADD                     W501 警告      描述子改成未指定的子運算碼 → A
;   30  MOV [4..5] -> ALU_OPB       W402 警告      來源位移改成 31（範圍 31..32）→ B
;   36  EXEC_XFER（讀 2 byte）                     ptr 改成 31（31 + 2 > 32）→ B    Peri 錯誤 B
;
; 按 F5：解碼的 A、B 問題在執行前就列在「問題」面板；執行後 Main 先停在第 27 行（錯誤 C），
; 再按 F5，Peri 停在第 36 行（錯誤 B）。兩者在同一個週期發生，Main 先報（共用匯流排同時要用時 Main 優先）。
; 要看「沒有改壞」的情況：除錯 →「使用建置產生的 EEPROM 除錯」，只剩建置警告與 Main 的錯誤 C。

.service SENSOR = 1, len=2
.devmap 0, I2C, 0x68                    ; 感測器（環境檔沒有放裝置：這個範例不需要它回應）

; ---- Main ---------------------------------------------------------------------------------------------
.main
start:
    MOV MAIN_SCRATCHPAD[0..1] -> ALU_OPA                    ; W502：scratch[0..1] 從來沒寫過就拿來用
    MOV PERI_RESULT_SLOT[0..1] -> MAIN_SCRATCHPAD[2..3]     ; W505：沒先檢查 PERI_RESULT_VALID；執行時結果槽是空的 → 錯誤 C
    ALU ADD unsigned                                        ; 燒錄檔把它的 ALU 描述子改壞 → 解碼 A
hold: JMP hold
    MOV MAIN_SCRATCHPAD[4..5] -> ALU_OPB                    ; W402：永遠跑不到；燒錄檔又把它的來源位移改成 31 → 解碼 B
    JMP hold

; ---- Peri ---------------------------------------------------------------------------------------------
.peri
poll:
    EXEC_XFER READ dev=0, len=2, ptr=0, ext=[0x3B]          ; 燒錄檔把 ptr 改成 31：31 + 2 超出 PERI_RX_BUF → 解碼 B，執行到時錯誤 B
    EXEC_PUB service=SENSOR, src=0, len=2
idle: JMP idle
