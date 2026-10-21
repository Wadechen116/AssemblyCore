# 09_profile_artifacts

同一個 IMU 程式（與 `02_imu_gyro` 相同），但使用一份**非預設的目標 profile**（`toy.profile`，數字都是虛構的，不是核准的硬體值）：
SRAM 視窗配置、區段對齊、ImageLayout 欄寬 8 位元、2 位元組的 DeviceMap 項目、重新編號的 ALU 子運算碼、事件數與延遲。
組譯器與模擬器必須使用**同一份** profile，否則同樣的位元組代表不同的程式。

## 試試看
1. 建置後看輸出面板的 `wrote …` 與 `out/` 資料夾：
   - `*.main.bin`／`*.peri.bin`：**邏輯映像**（不含 ImageLayout）。
   - `*.main.layout.json`／`*.peri.layout.json`：六個 ImageLayout 偏移（旁路 latch 的資料）。**沒有它 `.bin` 無法解讀。**
   - `*.sram.hex`：共用 32 位元 SRAM 的實體 word（Main 從 256 開始，Peri 從 288）。
   - `*.boot.json`、`*.memory.json`（視窗不重疊的證明）、`*.sections.json`、`*.descriptors.json`、`*.services.json`、`*.events.json`、`*.validation.json`、`*.manifest.json`。
2. 打開 `.memory.json`：`"ok": true`，`occupied_range` 與 `reserved_window`。
3. **改 profile 製造錯誤**（存檔後自動重建）：
   - `peri_image_base_word = 270` → **E311**（視窗重疊）。
   - `main_capacity_bytes = 60` → **E312**（映像大於視窗）。
   - `layout_offset_bits = 6` → **E314**（偏移放不進欄位）。
   - `device_map_bus_shift = 5` → **E313**（DeviceMap 欄位重疊）。
   - `event_count = 0` → profile 載入錯誤並指出鍵名。
4. **模擬器跟著 profile**：Debug 跑起來結果和 `02` 一樣（ALU 碼與 DeviceMap 排列都用 profile）。把 `alu_arith_sub` 改成與 `alu_arith_add` 相同的數字 → profile 載入失敗（碼必須互異）。
5. 用命令列驗證映像可還原：
   `asmdis --main --profile toy.profile --layout out/09_profile_artifacts.main.layout.json out/09_profile_artifacts.main.bin --check`
   或直接從 SRAM 檔還原：`asmdis --main --profile toy.profile --layout …layout.json --sram out/09_profile_artifacts.sram.hex --check`。
6. Build → Call model → softret 後重建：映像大小與 SRAM word 數變化，`.memory.json` 仍然 ok。

## 預期
`asmsim … --profile toy.profile … --scenario profile_demo.scn` → `PASS (8 checks)`，且 `check_samples.py` 對兩種呼叫模型都通過。
