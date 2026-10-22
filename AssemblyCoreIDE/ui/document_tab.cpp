#include "document_tab.h"

#include "block_editor.h"
#include "hardware_designer.h"

namespace acide {

TabRegistry& TabRegistry::instance() {
    static TabRegistry r;
    return r;
}

void TabRegistry::add(const TabKind& k) {
    for (TabKind& e : kinds_) if (e.extension == k.extension) { e = k; return; }
    kinds_.push_back(k);
}

const TabKind* TabRegistry::find(const std::string& path) const {
    for (const TabKind& k : kinds_)
        if (path.size() > k.extension.size() && path.compare(path.size() - k.extension.size(), k.extension.size(), k.extension) == 0) return &k;
    return nullptr;
}

std::vector<std::string> TabRegistry::extensions() const {
    std::vector<std::string> v;
    for (const TabKind& k : kinds_) v.push_back(k.extension);
    return v;
}

void registerBuiltinTabs() {
    static bool done = false;
    if (done) return;
    done = true;
    TabRegistry& r = TabRegistry::instance();
    r.add({".achw", [](AppContext& c, const std::string& p) -> QWidget* { return new HardwareDesigner(c, p); }});
    r.add({".acblk", [](AppContext& c, const std::string& p) -> QWidget* { return new BlockEditor(c, p); }});
}

}  // namespace acide
