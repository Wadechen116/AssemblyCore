# 03_call_models

`leaf` 被呼叫 3 次、`mid` 呼叫 `leaf`。**呼叫模型**決定硬體沒有的 CALL/RET 怎麼翻譯（見下）。

| | inline | softret |
|---|---|---|
| 程式碼大小 | 較大（每個 CALL 展開一份） | 較小（程序一份，但每個呼叫點與 RET 多幾條指令） |
| 週期 | 最短 | `RET` 要比對返回編號，成本隨呼叫點數增加 |
| 副作用 | 無 | 破壞 `ALU_OPA/OPB/旗標`，佔用 scratchpad 保留位元組 |

## 試試看
1. 建置（預設 inline），看輸出面板的 `WCET one_pass` 與專案設定的 `start:done` 查詢（兩個數字相同的查詢）。
2. **Build → Call model → softret** 重新建置：程式碼位元組數與 WCET 都變大；打開 `.map` 看 `procedures` 段的 `return slot SCRATCHPAD[24..25]`。
3. **清單檢視**：開 `.lst`，inline 時 `leaf` 的指令重複 3 次（右側註解 `; in leaf@行號`），softret 時只出現一次，呼叫點多出 `MOV CONST(k,2)->scratch` 與 `BR`。
4. **大綱**：`leaf` 顯示 3 個呼叫點；對 `leaf` 按 F2 重新命名，全部呼叫處一起改。
5. **除錯**：在 `start:` 設中斷點，F11 單步：`CALL leaf` 是**一步**，再 F11 才進入程序；**Call Stack** 面板顯示 `leaf ← start`，`mid` 內再呼叫 `leaf` 時顯示兩層。inline 與 softret 兩種模型的單步行為相同。
6. **Shift+F11（Step Out）** 從程序裡跳回呼叫處。
7. 資料流：softret 時 `ALU_OPA/OPB` 會被破壞，若自己在 CALL 前後插入使用 ALU 的程式，會出現 W501。

## 預期
兩種模型下 `asmsim … --scenario call_models.scn` 都是 `PASS`（行為相同，只有大小與時間不同）。
