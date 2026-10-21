# 04_alu_cascade

用 `300` 與 `25` 兩個常數（來自 `.const … dw`，以 `MAIN_CONST_POOL[@K_A]` 引用）跑完 ALU 的所有族：ADD、SUB、MUL、AND、OR、XOR、SHL、SHR、CMP.LT、CMP.GE。
再用 `CASCADE_BEGIN 2` 依序執行 3 個區塊（每塊 `SET8` 一個位元），最後用**一次** `WRITE_SP`（16 位元組）把結果寫進 CSR 視窗 `0x140…`。

## 試試看
1. 建置：零警告。`.map` 看常數池、描述子表用量（如 `MainMoveDesc 30/64`）。
2. 除錯單步，每個 `ALU` 之後看 **State** 面板的 ALU（`out`、旗標）與 **Memory** 的 `scratch[8..23]` 逐步填入 325、275、7500、8、317、309、1200、37。
3. **監看**面板新增 `scratch[8..9]`、`csr[0x010]`，十進位與十六進位同時顯示。
4. **資料中斷點**：Breakpoints 面板新增「值改變就停」`csr[0x010]` → 每個 cascade 區塊執行後都停一次。
5. 把 `CASCADE_BEGIN 2` 改成 `1` → `AE_CTRL` 最後是 `0x07`（少了區塊 2）；改成 `5` → 錯誤 **E304**（選擇值超過最後一個區塊）。
6. 在 `block0` 與 `CASCADE_END` 之間插入 `JMP done` → **E304**（cascade 區域內不可有一般分支）。
7. 刪掉 `ALU CMP.GE` 前的 `BR.COND … less`，讓 `ALU_OUT` 未被搬走就被下一個 ALU 覆寫 → **W504**。
8. 改 `ALU EQ signed`（在 `CMP.LT` 處） → **E203**（EQ/NE 不能 signed）。

## 預期
`asmsim -I ../common alu_cascade.asm --scenario alu_cascade.scn` → `PASS (11 checks)`。
