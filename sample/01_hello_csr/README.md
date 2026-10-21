# 01_hello_csr

最小的 Main 程式：三種單位元組 CSR 寫入、用 ALU 把計數器加到 5、再用 `EXEC_CSR WRITE_SP` 把結果寫到 CSR，最後跳到自己（Main 停機）。

## 試試看
1. **建置（F7）**：輸出面板顯示 `OK`、零警告；`out/` 出現 `01_hello_csr.main.bin`、`.main.layout.json` 等。
2. **懸停**：`AE_CTRL`、`WRITE_SP`、`CMP.EQ` 各出現說明（符號值、簽名、規格章節）。
3. **補全**：在新的一行輸入 `MOV `，看到區域名稱；輸入 `ALU ` 看到運算。
4. **除錯**：在 `loop:` 下一行按 F9 設中斷點，F5 開始 → 停在該行；用 F10 單步，看 **State** 面板的 ALU 與 **Memory** 面板 `scratch[0..1]` 一次加 1（變動的位元組標紅）。
5. **條件中斷點**：把中斷點條件設成 `scratch[0..1] == 4`，只在第 5 圈前停。
6. **改記憶體**：停住時把 `scratch[4..5]`（上限）改成 `0x0002`，繼續 → 迴圈提早結束。
7. **CSR 面板**：停機後 `0x120` 應為 5，`0x010` 為 `0x10`。
8. **失控保護**：把最後的 `done: JMP done` 改成 `done: JMP loop`（不再停機），F5 會在上限（5000 萬週期或 15 秒）停下並說明原因。

## 預期（情境腳本）
`asmsim -I ../common hello.asm --scenario hello.scn` → `PASS (4 checks)`，約在第 122 週期停機。
