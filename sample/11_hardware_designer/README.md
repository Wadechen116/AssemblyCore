# 11_hardware_designer

和 `02_imu_gyro` 是同一個 IMU 程式，但 **程式裡一行定義都沒有**：所有 CSR、裝置、服務的名稱與意義都來自 `board.achw`（硬體描述）。
專案把產生的 `board.gen.inc` 列為**第一個**來源，`board.gen.env.cfg` 當模擬器環境。說明見 `docs/HARDWARE.md`。

## 試試看（File → Hardware、設計器頁籤）
1. 開專案，專案樹的 **Hardware** 群組有 `board.achw`；雙擊（或 File → Hardware → Open Hardware Designer）開設計器。
2. 左邊元件樹點 **PERI_CTRL**：右邊看到位址、種類 `action`，**8 個位元**各自的模擬動作與組譯器角色（bit3 = `timer_start`）。
3. 底部 **Generated definitions** 頁籤看到 `.csr`、`.csrbit`、`.service … big`、`.devmap` 就是從表單來的；**Generated environment** 是模擬器用的。
4. **改一個值看反應**（不必存檔）：
   - 把服務 `SID_IMU_GYRO_RAW` 的 **Payload bytes** 從 6 改成 4 → 問題面板立刻出現 **W517**（Peri 發佈與 Main 消耗的 6 位元組與契約不符）。
   - 先在 `imu.asm` 刪掉位元組交換的兩行 `MOV … [8..8]`／`[9..9]`（並讓 `ALU_OPA` 直接讀 `scratch[0..1]`）→ **W514**（高位元組先的值未交換就進 ALU）；
     再到設計器把服務的 **Byte order** 改成 `little` → W514 消失（契約改了，不必交換）。
   - 把 CSR `PERI_CTRL` 的位址改成 `0x301`（`POLL_CTRL` 已佔用）→ 設計器問題清單：`address 0x301 is already taken by csr POLL_CTRL`。
   - 在 `imu.asm` 刪掉 `EXEC_CSR SET8 PERI_CTRL, PERI_TIMER_START` 那行 → **W515**（計時器沒啟動就在等它）。這個檢查靠 `PERI_CTRL` bit3 的 **timer_start 角色**；角色是設計器裡的設定，不在程式裡。
5. 設計器問題清單雙擊一筆 → 跳到該元件。
6. **Add → CSR register** 新增一個暫存器，名稱填 `NEW_LED`、位址 `0x140`；在 `imu.asm` 打 `EXEC_CSR WRITE8 NEW_LED, 1`，**補全清單與診斷立刻認得它**。
7. 存檔（Ctrl+Shift+S）後 `board.achw`、`board.gen.inc`、`board.gen.env.cfg` 三個檔案一起寫入；專案樹的圓點消失。
8. 除錯（F5）：模擬器用的是產生的環境檔；在設計器改了 `[device imu]` 的暫存器值，重新開始除錯就會用新值。

## 目標 profile（設計器最下方的 Target profile）
專案的 `board.profile` 用表單編輯，**只改你動的那一行、保留檔案裡的註解與順序**：
- 每個群組一個頁籤（Placement、ALU numbers、Latency…）；每個鍵都有說明，滑鼠移上去看預設值。
- 頁面最上面的黃字：**還有幾組數值只是規劃值、沒有人對照硬體確認過**（這個範例只確認了 pc_width，所以顯示 8 組）。分頁名稱後面有 ⚠ 的就是沒確認的。
- 硬體負責人檢查過某一組，就勾選該頁籤上的「These values are confirmed against the hardware」→ 檔案多一行 `confirmed = …`，缺口數減一。
- 改 `main_image_base_word` 讓 Main 視窗和 Peri 視窗重疊 → 重新建置出現 **E311**（修改不需存檔就生效）。
- 命令列：`asmc --profile board.profile --profile-gaps` 列出缺口與原因；`asmc … --artifacts` 的 `manifest.json` 會記錄 `unconfirmed` 清單。

## 接手既有專案
複製 `02_imu_gyro`，在新專案選 **File → Hardware → Adopt Existing Definitions…**，依序選 `defs.inc`、`imu_env.cfg`、輸出的 `.achw`：
專案的 sources、環境檔與 `.include` 都不用改；`defs.inc` 與 `imu_env.cfg` 變成「產生的檔案」（存檔時被取代，內容等價）。

## 預期
`asmsim board.gen.inc imu.asm --env board.gen.env.cfg --scenario imu.scn` → `PASS (8 checks)`；
`check_samples.py` 另外確認 `asmhw check` 通過，且專案裡的兩個產生檔和現在的描述產生的**完全相同**。
