# 12_block_files

**Stage B0 of the block editor** (`AssemblyCoreIDE/docs/BLOCKS_DESIGN.md`): a program as *blocks* in a `.acblk` file. The graphical editor arrives in stage B1;
what exists now is the model, the file format, the generator and the importer — usable from the command line (`asmblk`) and from the C++ library.

| File | What it is |
|---|---|
| `hello.acblk` | the program of `01_hello_csr` as blocks (JSON): sections, labels, `mov`, `alu`, `csr_write_sp` ... each with an id |
| `hello.generated.asm` | the assembler text generated from it (this is what the project assembles) |

## 試試看（命令列）
```
asmblk blocks                                      列出所有積木（依抽屜分類）與欄位
asmblk import ../01_hello_csr/hello.asm -o x.acblk 文字 → 積木（看不懂的行會保留成 Raw 積木，不會失敗）
asmblk gen hello.acblk -o x.asm                    積木 → 文字
asmblk check hello.acblk                           檢查積木檔結構
asmblk check ../02_imu_gyro/imu_gyro.asm           文字 → 積木 → 文字，兩邊各組譯一次，映像必須相同
```
1. 打開 `hello.acblk`：每個積木有穩定的 `id`、`type`、`fields`；`proc` 這種容器有 `stacks`。
2. 把某個 `mov` 的 `fields.src` 改成 `CONST(9, 2)`，執行 `asmblk gen hello.acblk -o /tmp/x.asm`，看產生的文字變了一行。
3. 把某個積木的 `"type"` 改成 `"frobnicate"`，`asmblk gen` 會說明是哪個積木、什麼問題，並且不輸出檔案。
4. 在 `.acblk` 裡把 `"disabled": true` 加到某個積木 → 產生的文字把它變成註解。

## 保證
文字 → 積木 → 文字，**組譯後的映像位元組與診斷碼都相同**，且再轉一次文字不變（`asmcore_blocks_tests` 對 `examples/` 與 `sample/` 的每個程式都檢查，`check_samples.py` 也驗證本專案的產生檔是目前積木產生的）。
