#include "asmcore/sim_params.h"
#include <cstdio>
using namespace asmcore;
int main() {
    std::puts("# 模擬器的硬體參數\n");
    std::puts("> 這份表由 `gen_sim_params_doc` 從目錄產生（程式碼是唯一來源；目錄變動後執行 `gen_sim_params_doc > docs/SIM_PARAMS.md`；`asmcore_sim_tests` 會檢查每個參數都在這裡）。");
    std::puts("> **初始值**＝規劃值：沒有設定時模擬器用的數字，等於改這些功能之前的行為。上 FPGA 量到實際值後，用下面任何一種方式覆寫，並把狀態標成 `measured`／`confirmed`。\n");
    std::puts("## 在 IDE 裡\n");
    std::puts("- **View → Simulator Parameters**（模擬器參數面板）：所有參數分組列出，雙擊「值」直接改（數字、true／false、選項）；**值**欄粗體橘色＝和初始值不同；**狀態**欄選 planning／measured／confirmed；**備註**欄記量測方式。選到一列，下面會顯示說明、範圍、以及在 FPGA 上怎麼量。右鍵「還原為初始值」；上方有 Load／Save／Save As（`.simparams` 檔，Save As 會把它登記進專案的 `simparams =`）、Reset all、「只看有改的」與篩選。尚未被模擬器使用的參數以斜體加「（尚未被模擬器使用）」標示，值照樣存進檔案。");
    std::puts("- **View → Execution**（執行狀態面板）：把程式在獨立的模擬器跑一次，Main 與 Peri 是兩條軌道（寬度＝指令花的週期，EXEC_XFER 會拉長；事件、I2C 傳輸結束、發佈、結果槽被取走各有標記），**下方的「基準」軌道是用初始值（或存下的那次）跑出來的**，兩者疊在一起，位移一目了然；最下面的折線是兩個定序器的**累積位移**；表格列出每一條被移動的指令（基準週期、現在週期、位移）。參數一改就自動重跑（可關），「第一個差異」跳到最先被動到的指令並放大，滾輪縮放、拖曳平移、雙擊全部看到、點指令跳到原始碼那一行。「存下這次」把現在這次當基準，之後每調一次參數都和它比。");
    std::puts("- 「套用到執行中的模擬器」：除錯中的模擬器用的是啟動時的參數；改了之後按這個鈕，會用新參數重新啟動（執行狀態面板不需要，它每次都用目前的值）。\n");
    std::puts("## 怎麼設定\n");
    std::puts("| 方式 | 寫法 | 說明 |\n|---|---|---|");
    std::puts("| 環境檔 | `[params]` 區段：`key = value`，另可寫 `key.status = measured`、`key.note = 說明` | 和 `[latency]`、`[timing]`、`[bus i2c]`、`[sim]` 一起；`[params]` 優先 |");
    std::puts("| 命令列 | `asmsim --params FILE`、`asmsim --param key=value`（可重複） | 比環境檔優先；`latency.*`、`clock.*` 也會用在組譯器的 WCET，兩邊一致 |");
    std::puts("| 程式 | `SimConfig::params`（IDE 的參數面板用這個） | 最優先 |");
    std::puts("\n優先順序（後者蓋前者）：初始值 → 環境檔的各區段 → 環境檔的 `[params]` → `SimConfig::params`／命令列。\n");
    std::puts("**狀態**：`planning`（規劃值，或手動設定但沒有量測）、`measured`（FPGA 實測）、`confirmed`（已對照 RTL）。\n");
    std::string group;
    for (const ParamDef& d : paramCatalog()) {
        if (d.group != group) {
            group = d.group;
            std::printf("\n## %s\n\n| 參數 | 單位 | 初始值 | 範圍 | 說明 | 在 FPGA 上怎麼量 |\n|---|---|---|---|---|---|\n", group.c_str());
        }
        std::string range;
        if (d.kind == ParamDef::Enum) { for (const std::string& o : d.options) range += (range.empty() ? "" : " \\| ") + o; }
        else if (d.kind == ParamDef::Bool) range = "true \\| false";
        else range = std::to_string(d.lo) + " … " + std::to_string(d.hi);
        std::printf("| `%s` | %s | %s | %s | %s%s | %s |\n", d.key.c_str(), d.unit.c_str(), SimParams().text(d.key).c_str(), range.c_str(), d.help.c_str(), d.active ? "" : "（**尚未被模擬器使用**）", d.measure.c_str());
    }
    return 0;
}
