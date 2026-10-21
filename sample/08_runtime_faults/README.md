# 08_runtime_faults

程式**建置成功**（有 W505、W502 兩個警告，正是它們預告的問題），但執行時會 fault。模式由 CSR `MODE`（`0x050`）決定：

| MODE | 發生什麼 | fault 碼 | 條件 |
|---|---|---|---|
| 1 | 結果槽是空的卻去消耗 | `fault_slot_not_valid` | — |
| 2 | 寫入環境檔沒定義的 CSR `0x7F0` | `fault_csr_unmapped` | 環境檔 `[sim] unmapped_csr = fault` |
| 3 | 從沒讀成功的 `MAIN_CSR_DATA_BUF` 位元組做 MOV | `fault_csrbuf_invalid` | profile `csr_buf_valid = true` |
| 其他 | 無，`AE_CTRL = 0x77` 後停機 | — | — |

## 試試看
1. 建置：2 個警告（W505、W502）。
2. 讓 `MODE = 1`：最簡單是在 `faults_env.cfg` 的 `[csr MODE]` 加一行 `reset = 1`；或在第一行（`start:`）設中斷點，F5 停下後到 **CSRs** 面板把 `MODE` 改成 1，再 F5。停止原因是 fault；**State** 面板顯示 fault 碼、位址與訊息，編輯器停在 `MOV PERI_RESULT_SLOT…` 那一行。
3. **Clear fault**（State 面板）：因為原因還在，同一條指令會再 fault。先在 **Memory** 面板把 `slot` 填一個位元組、**Signals** 面板把 `slot.valid` 設成 1，再 Clear fault → 該 MOV 成功，程式往下走完。
4. 改 `MODE = 2`、`3` 重做：fault 不同；2 與 3 要改環境檔／profile 才能避免（這就是「fault 指出的是環境／契約問題」）。
5. 把 profile 的 `csr_buf_valid` 改成 `false`（Edit 專案設定的 profile）→ 模式 3 不再 fault（舊資料被直接複製，這正是該保護要擋的狀況）。
6. 事件記錄面板：fault 那一筆紅色，點一下跳回原始碼。

## 預期
三個情境腳本 `fault_slot.scn`、`fault_csr.scn`、`fault_buf.scn` 都 `PASS`（用 `asmsim --profile faults.profile --env faults_env.cfg`）。
