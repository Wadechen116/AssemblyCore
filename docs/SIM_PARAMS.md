# 模擬器的硬體參數

> 這份表由 `gen_sim_params_doc` 從目錄產生（程式碼是唯一來源；目錄變動後執行 `gen_sim_params_doc > docs/SIM_PARAMS.md`；`asmcore_sim_tests` 會檢查每個參數都在這裡）。
> **初始值**＝規劃值：沒有設定時模擬器用的數字，等於改這些功能之前的行為。上 FPGA 量到實際值後，用下面任何一種方式覆寫，並把狀態標成 `measured`／`confirmed`。

## 在 IDE 裡

- **View → Simulator Parameters**（模擬器參數面板）：所有參數分組列出，雙擊「值」直接改（數字、true／false、選項）；**值**欄粗體橘色＝和初始值不同；**狀態**欄選 planning／measured／confirmed；**備註**欄記量測方式。選到一列，下面會顯示說明、範圍、以及在 FPGA 上怎麼量。右鍵「還原為初始值」；上方有 Load／Save／Save As（`.simparams` 檔，Save As 會把它登記進專案的 `simparams =`）、Reset all、「只看有改的」與篩選。尚未被模擬器使用的參數以斜體加「（尚未被模擬器使用）」標示，值照樣存進檔案。
- **View → Execution**（執行狀態面板）：把程式在獨立的模擬器跑一次，Main 與 Peri 是兩條軌道（寬度＝指令花的週期，EXEC_XFER 會拉長；事件、I2C 傳輸結束、發佈、結果槽被取走各有標記），**下方的「基準」軌道是用初始值（或存下的那次）跑出來的**，兩者疊在一起，位移一目了然；最下面的折線是兩個定序器的**累積位移**；表格列出每一條被移動的指令（基準週期、現在週期、位移）。參數一改就自動重跑（可關），「第一個差異」跳到最先被動到的指令並放大，滾輪縮放、拖曳平移、雙擊全部看到、點指令跳到原始碼那一行。「存下這次」把現在這次當基準，之後每調一次參數都和它比。
- 「套用到執行中的模擬器」：除錯中的模擬器用的是啟動時的參數；改了之後按這個鈕，會用新參數重新啟動（執行狀態面板不需要，它每次都用目前的值）。

## 怎麼設定

| 方式 | 寫法 | 說明 |
|---|---|---|
| 環境檔 | `[params]` 區段：`key = value`，另可寫 `key.status = measured`、`key.note = 說明` | 和 `[latency]`、`[timing]`、`[bus i2c]`、`[sim]` 一起；`[params]` 優先 |
| 命令列 | `asmsim --params FILE`、`asmsim --param key=value`（可重複） | 比環境檔優先；`latency.*`、`clock.*` 也會用在組譯器的 WCET，兩邊一致 |
| 程式 | `SimConfig::params`（IDE 的參數面板用這個） | 最優先 |

優先順序（後者蓋前者）：初始值 → 環境檔的各區段 → 環境檔的 `[params]` → `SimConfig::params`／命令列。

**狀態**：`planning`（規劃值，或手動設定但沒有量測）、`measured`（FPGA 實測）、`confirmed`（已對照 RTL）。


## Instruction latency

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `latency.br_cond` | ISA cycles | 1 | 0 … 100000 | BR.COND, taken or not | N back-to-back BR.COND: total cycles / N |
| `latency.cascade` | ISA cycles | 1 | 0 … 100000 | CASCADE_BEGIN / CASCADE_END | a cascade of N parts: total cycles / (2N) |
| `latency.alu` | ISA cycles | 1 | 0 … 100000 | ALU add / sub / compare / logic / shift | N back-to-back ALU operations: total cycles / N |
| `latency.mul` | ISA cycles | 2 | 0 … 100000 | ALU multiply | N back-to-back MUL: total cycles / N |
| `latency.mov_setup` | ISA cycles | 1 | 0 … 100000 | MOV: fixed part (MOV costs setup + byte x length) | MOV of 1 and of 8 bytes: the difference gives the per-byte part, the rest is the setup |
| `latency.mov_byte` | ISA cycles | 1 | 0 … 100000 | MOV: cycles per byte | see mov_setup |
| `latency.slot_consume_setup` | ISA cycles | 2 | 0 … 100000 | MOV from PERI_RESULT_SLOT: fixed part (setup + byte x length) | MOV from the slot of 1 and of 8 bytes |
| `latency.csr_access` | ISA cycles | 2 | 0 … 100000 | one CSR access (a one-byte EXEC_CSR is 1 + access, a read-modify-write 1 + 2 x access) | N back-to-back EXEC_CSR WRITE8: (total / N) - 1 |
| `latency.csr_byte` | ISA cycles | 2 | 0 … 100000 | multi-byte EXEC_CSR: cycles per byte | WRITE_SP of 1 and of 8 bytes |
| `latency.csr_done` | ISA cycles | 1 | 0 … 100000 | multi-byte EXEC_CSR: closing cycles | see csr_byte |
| `latency.pub_setup` | ISA cycles | 2 | 0 … 100000 | EXEC_PUB: fixed part (setup + byte x length) | EXEC_PUB of 1 and of 8 bytes |
| `latency.pub_byte` | ISA cycles | 1 | 0 … 100000 | EXEC_PUB: cycles per byte | see pub_setup |
| `latency.event_dispatch` | ISA cycles | 2 | 0 … 100000 | redirect to the handler vector once a pending event is dispatched | event injection: dispatch to the first instruction of the handler |
| `latency.eret` | ISA cycles | 2 | 0 … 100000 | BR.ERET: back to the resume address | the last handler instruction to the first resumed one |
| `latency.xfer_issue` | ISA cycles | 2 | 0 … 100000 | EXEC_XFER issue overhead, before the bus time | EXEC_XFER issue to the first SCL pulse (minus i2ch.overhead) |

## Clocks

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `clock.cpu_hz` | Hz | 27000000 | 1000 … 1000000000 | the ISA clock (spec 14.1) | from the FPGA design |
| `clock.i2c_hz` | Hz | 400000 | 1000 … 10000000 | the I2C bus clock; 400 kHz is 67.5 ISA cycles per SCL pulse at 27 MHz (spec 14.4) | SCL period on a logic analyzer |

## Frame timing

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `timing.frame_cycles` | ISA cycles | 0 | 0 … 2147483647 | length of a frame; 0 = no frame timing (FRAME_END_EVT never fires) | frame period from the sensor / timing generator |
| `timing.vblank_start` | ISA cycles | 0 | 0 … 2147483647 | where in the frame the vertical blanking window opens | timing generator |
| `timing.vblank_len` | ISA cycles | 0 | 0 … 2147483647 | length of the vertical blanking window | timing generator |
| `timing.line_cycles` | ISA cycles | 0 | 0 … 2147483647 | length of a line; 0 = no line timing | timing generator |
| `timing.hblank_start` | ISA cycles | 0 | 0 … 2147483647 | where in the line the horizontal blanking window opens | timing generator |
| `timing.hblank_len` | ISA cycles | 0 | 0 … 2147483647 | length of the horizontal blanking window | timing generator |
| `frame.wcet_pgl` | ISA cycles | 0 | 0 … 2147483647 | Frame Timing WCET: worst-case time of a PGL commit (PGL_COMMIT_ELIGIBLE needs this much blanking left) | the WCET register of the Frame Timing block |
| `frame.wcet_ae` | ISA cycles | 0 | 0 … 2147483647 | Frame Timing WCET of an AE commit | the WCET register of the Frame Timing block |
| `frame.wcet_line` | ISA cycles | 0 | 0 … 2147483647 | Frame Timing WCET of a line commit | the WCET register of the Frame Timing block |

## Scheduling

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `sim.tie_order` |  | main_first | main_first \| peri_first | who goes first when both sequencers are ready in the same cycle (timed events always come before both) | two programs that touch the same CSR in the same cycle |
| `sim.unmapped_csr` |  | ram | ram \| fault | an EXEC_CSR to an address the environment does not define: behaves as RAM, or faults |  |
| `sim.fault_stops` |  | domain | domain \| all | a fault stops only its own sequencer, or both |  |

## Decoders

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `main.fetch_cycles` | ISA cycles | 0 | 0 … 1000 | extra cycles the Main decoder spends on every instruction (descriptor fetch and decode; the program SRAM runs at 2 x the ISA clock) | N back-to-back one-cycle instructions: (total / N) - 1 |
| `peri.fetch_cycles` | ISA cycles | 0 | 0 … 1000 | the same for the Peri decoder | N back-to-back BR instructions: (total / N) - 1 |
| `csr.access_main` | ISA cycles | -1 | -1 … 100000 | one CSR access as the Main decoder sees it; -1 = the same as latency.csr_access | N back-to-back EXEC_CSR from Main |
| `csr.access_peri` | ISA cycles | -1 | -1 … 100000 | one CSR access as the Peri decoder sees it; -1 = the same as latency.csr_access | N back-to-back EXEC_CSR from Peri |
| `reset.boot_cycles` | ISA cycles | 0 | 0 … 1000000 | from the end of reset to the first instruction of each sequencer | first CSR write after reset on the logic analyzer |

## Events

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `event.dispatch_extra` | ISA cycles | 0 | 0 … 100000 | extra cycles on top of latency.event_dispatch (checkpoint sampling, vector load) | event injection: raised to dispatched, minus latency.event_dispatch |
| `event.eret_extra` | ISA cycles | 0 | 0 … 100000 | extra cycles on top of latency.eret | last handler instruction to the first resumed one |

## Timers

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `timer.main.clock_div` | ISA cycles per tick | 1 | 1 … 65535 | main timer: the timer counts at the ISA clock divided by this | start with period N, measure the cycles until done, divide by N |
| `timer.main.start_latency` | ISA cycles | 0 | 0 … 1000000 | main timer: from the EXEC_CSR that starts it to the first tick | start with period 1: total time minus the done latency |
| `timer.main.done_latency` | ISA cycles | 0 | 0 … 1000000 | main timer: from the last tick to the done latch being visible to BR | start with period 1: total time minus the start latency |
| `timer.peri.clock_div` | ISA cycles per tick | 1 | 1 … 65535 | peri timer: the timer counts at the ISA clock divided by this | start with period N, measure the cycles until done, divide by N |
| `timer.peri.start_latency` | ISA cycles | 0 | 0 … 1000000 | peri timer: from the EXEC_CSR that starts it to the first tick | start with period 1: total time minus the done latency |
| `timer.peri.done_latency` | ISA cycles | 0 | 0 … 1000000 | peri timer: from the last tick to the done latch being visible to BR | start with period 1: total time minus the start latency |

## I2C path

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `i2ch.overhead` | ISA cycles | 0 | 0 … 1000000 | extra cycles from EXEC_XFER to the I2C master starting (the i2ch block) | EXEC_XFER issue to the first SCL pulse |
| `i2c.ack_delay` | ISA cycles per byte | 0 | 0 … 1000 | extra cycles per byte phase for the acknowledge | SCL falling edge of the 9th pulse to the next byte |
| `i2c.stretch` | ISA cycles per byte | 0 | 0 … 1000000 | clock stretching per byte phase | SCL held low by the device |
| `i2c.turnaround` | ISA cycles | 0 | 0 … 1000000 | extra cycles for the direction change (repeated start) of a read with a register address | repeated start on the bus |

## CSR arbiter

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `csr.arbiter_policy` |  | none | none \| main_first \| peri_first \| round_robin | how the Main/Peri CSR arbiter chooses when both decoders want the CSR bus in the same cycle; none = no arbitration is modelled（**尚未被模擬器使用**） | both decoders issue EXEC_CSR in the same cycle |
| `csr.arbiter_penalty` | ISA cycles | 0 | 0 … 100000 | extra cycles the decoder that loses the arbitration waits（**尚未被模擬器使用**） | difference in completion between the winner and the loser |

## Data path

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `datapath.contention` |  | false | true \| false | MOV and the result slot / EXEC_PUB share Data Xfer: one waits for the other（**尚未被模擬器使用**） | Main MOV from the slot while Peri runs EXEC_PUB |
| `datapath.contention_cycles` | ISA cycles | 0 | 0 … 100000 | how long the one that waits waits（**尚未被模擬器使用**） | see datapath.contention |

## Result slot

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `slot.copy_extra` | ISA cycles | 0 | 0 … 100000 | extra cycles for EXEC_PUB to copy rx_buf into result_slot（**尚未被模擬器使用**） | EXEC_PUB of 8 bytes against the model |
| `slot.valid_delay` | ISA cycles | 0 | 0 … 100000 | delay from the copy to PERI_RESULT_VALID being visible to Main（**尚未被模擬器使用**） | Main polling PERI_RESULT_VALID after a PUB |

## Status flags

| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |
|---|---|---|---|---|---|
| `flag.sample_delay_main` | ISA cycles | 0 | 0 … 100000 | delay from a condition changing to a Main BR seeing it（**尚未被模擬器使用**） | toggle a condition, find the first BR that sees it |
| `flag.sample_delay_peri` | ISA cycles | 0 | 0 … 100000 | the same for Peri（**尚未被模擬器使用**） | toggle a condition, find the first BR that sees it |
