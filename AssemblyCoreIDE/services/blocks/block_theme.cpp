#include "blocks/block_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace acide {
namespace blocks {

Rgba rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { Rgba c; c.r = r; c.g = g; c.b = b; c.a = a; return c; }

bool parseColor(const std::string& t, Rgba& out) {
    if (t.size() != 7 && t.size() != 9) return false;
    if (t[0] != '#') return false;
    for (size_t i = 1; i < t.size(); i++) if (!std::isxdigit((unsigned char)t[i])) return false;
    auto hex = [&](size_t at) { return (uint8_t)std::strtoul(t.substr(at, 2).c_str(), nullptr, 16); };
    out = rgb(hex(1), hex(3), hex(5), t.size() == 9 ? hex(7) : 255);
    return true;
}

std::string formatColor(const Rgba& c) {
    char b[16];
    if (c.a == 255) std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
    else std::snprintf(b, sizeof b, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return b;
}

Rgba mix(const Rgba& a, const Rgba& b, double t) {
    t = std::max(0.0, std::min(1.0, t));
    auto m = [&](uint8_t x, uint8_t y) { return (uint8_t)(x + (y - x) * t + 0.5); };
    return rgb(m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a));
}

Rgba withAlpha(const Rgba& c, uint8_t a) { Rgba o = c; o.a = a; return o; }

double luminance(const Rgba& c) {
    auto lin = [](double v) { v /= 255.0; return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

Rgba BlockTheme::textOn(const Rgba& fill) const { return luminance(fill) > 0.35 ? blockText : blockTextOnDark; }

Rgba BlockTheme::categoryColor(const std::string& cat) const {
    auto it = category.find(cat);
    if (it != category.end()) return it->second;
    // a stable colour from the name (FNV-1a), so an unknown category is recognisable and always the same
    uint32_t h = 2166136261u;
    for (unsigned char c : cat) { h ^= c; h *= 16777619u; }
    const double hue = (h % 360) / 60.0;
    const double s = dark ? 0.45 : 0.5, v = dark ? 0.62 : 0.78;
    const double c = v * s, x = c * (1 - std::fabs(std::fmod(hue, 2.0) - 1)), m = v - c;
    double r = 0, g = 0, b = 0;
    if (hue < 1) { r = c; g = x; } else if (hue < 2) { r = x; g = c; } else if (hue < 3) { g = c; b = x; } else if (hue < 4) { g = x; b = c; } else if (hue < 5) { r = x; b = c; } else { r = c; b = x; }
    return rgb((uint8_t)((r + m) * 255), (uint8_t)((g + m) * 255), (uint8_t)((b + m) * 255));
}

BlockTheme BlockTheme::light() {
    BlockTheme t;
    t.name = "light";
    t.dark = false;
    t.canvas = rgb(0xf7, 0xf8, 0xfa);
    t.grid = rgb(0xe6, 0xe8, 0xec);
    t.gridMajor = rgb(0xd8, 0xdb, 0xe1);
    t.blockText = rgb(0x1f, 0x23, 0x28);
    t.blockTextOnDark = rgb(0xff, 0xff, 0xff);
    t.chip = rgb(0xff, 0xff, 0xff, 0xe6);
    t.chipText = rgb(0x1f, 0x23, 0x28);
    t.chipBorder = rgb(0x00, 0x00, 0x00, 0x33);
    t.selection = rgb(0x1a, 0x73, 0xe8);
    t.hover = rgb(0xff, 0xff, 0xff, 0x80);
    t.error = rgb(0xd9, 0x30, 0x25);
    t.warning = rgb(0xe3, 0x74, 0x00);
    t.hatch = rgb(0xc6, 0x28, 0x28);
    t.floatingOutline = rgb(0x70, 0x78, 0x85);
    t.hatFill = rgb(0x3b, 0x42, 0x4d);
    t.hatText = rgb(0xff, 0xff, 0xff);
    t.note = rgb(0xff, 0xf6, 0xc8);
    t.noteText = rgb(0x5f, 0x4b, 0x00);
    t.shadow = rgb(0x00, 0x00, 0x00, 0x30);
    t.hole = rgb(0x0a, 0x0a, 0x0e, 0xe6);
    t.holeBorder = rgb(0xff, 0xff, 0xff, 0x38);
    t.category = {{"Data", rgb(0x58, 0xa5, 0xe8)},       {"Math", rgb(0x7e, 0xc8, 0x6b)},      {"CSR", rgb(0xe8, 0xa2, 0x4a)},
                  {"Control", rgb(0xd9, 0x7a, 0xb8)},    {"Peripheral", rgb(0x4d, 0xc1, 0xb0)}, {"Structure", rgb(0x9a, 0x86, 0xe0)},
                  {"Definitions", rgb(0xb4, 0xb9, 0xc4)}, {"Text", rgb(0xd3, 0xd6, 0xdc)},
                  {"References", rgb(0x6f, 0xcf, 0xe0)}, {"Values", rgb(0xea, 0xc5, 0x4f)},      {"Regions", rgb(0xf0, 0x8a, 0x6b)},   {"Options", rgb(0xa9, 0xb4, 0xd0)}};
    return t;
}

BlockTheme BlockTheme::dark_() {
    BlockTheme t;
    t.name = "dark";
    t.dark = true;
    t.canvas = rgb(0x1e, 0x20, 0x24);
    t.grid = rgb(0x28, 0x2b, 0x31);
    t.gridMajor = rgb(0x33, 0x37, 0x3f);
    t.blockText = rgb(0x1f, 0x23, 0x28);
    t.blockTextOnDark = rgb(0xf2, 0xf4, 0xf7);
    t.chip = rgb(0x00, 0x00, 0x00, 0x55);
    t.chipText = rgb(0xf2, 0xf4, 0xf7);
    t.chipBorder = rgb(0xff, 0xff, 0xff, 0x30);
    t.selection = rgb(0x5a, 0xa9, 0xff);
    t.hover = rgb(0xff, 0xff, 0xff, 0x40);
    t.error = rgb(0xff, 0x6b, 0x5e);
    t.warning = rgb(0xff, 0xa7, 0x3d);
    t.hatch = rgb(0xff, 0x6b, 0x6b);
    t.floatingOutline = rgb(0x8d, 0x95, 0xa3);
    t.hatFill = rgb(0x4a, 0x52, 0x60);
    t.hatText = rgb(0xf2, 0xf4, 0xf7);
    t.note = rgb(0x4d, 0x44, 0x1c);
    t.noteText = rgb(0xf3, 0xe3, 0x9c);
    t.shadow = rgb(0x00, 0x00, 0x00, 0x60);
    t.hole = rgb(0x00, 0x00, 0x00, 0xf0);
    t.holeBorder = rgb(0xff, 0xff, 0xff, 0x30);
    t.category = {{"Data", rgb(0x3a, 0x78, 0xb8)},       {"Math", rgb(0x4e, 0x96, 0x41)},      {"CSR", rgb(0xb8, 0x7c, 0x2c)},
                  {"Control", rgb(0xa8, 0x55, 0x8a)},    {"Peripheral", rgb(0x2c, 0x95, 0x88)}, {"Structure", rgb(0x70, 0x5f, 0xb4)},
                  {"Definitions", rgb(0x6c, 0x71, 0x7d)}, {"Text", rgb(0x55, 0x59, 0x62)},
                  {"References", rgb(0x3b, 0x9a, 0xaa)}, {"Values", rgb(0xb3, 0x9a, 0x2e)},      {"Regions", rgb(0xb8, 0x60, 0x3f)},   {"Options", rgb(0x6b, 0x76, 0x94)}};
    return t;
}

BlockTheme BlockTheme::fromSettings(const Settings& s, bool dark) {
    BlockTheme t = forMode(dark);
    struct Role { const char* key; Rgba BlockTheme::*field; };
    static const Role roles[] = {{"canvas", &BlockTheme::canvas}, {"grid", &BlockTheme::grid}, {"gridMajor", &BlockTheme::gridMajor}, {"blockText", &BlockTheme::blockText},
                                 {"blockTextOnDark", &BlockTheme::blockTextOnDark}, {"chip", &BlockTheme::chip}, {"chipText", &BlockTheme::chipText}, {"chipBorder", &BlockTheme::chipBorder},
                                 {"selection", &BlockTheme::selection}, {"hover", &BlockTheme::hover}, {"error", &BlockTheme::error}, {"warning", &BlockTheme::warning},
                                 {"hatch", &BlockTheme::hatch}, {"floatingOutline", &BlockTheme::floatingOutline}, {"hatFill", &BlockTheme::hatFill}, {"hatText", &BlockTheme::hatText},
                                 {"note", &BlockTheme::note}, {"noteText", &BlockTheme::noteText}, {"shadow", &BlockTheme::shadow}, {"hole", &BlockTheme::hole}, {"holeBorder", &BlockTheme::holeBorder}};
    for (const Role& r : roles) {
        Rgba c;
        if (parseColor(s.get(std::string("blocks.") + r.key), c)) t.*(r.field) = c;
    }
    const std::string prefix = "blocks.category.";
    for (auto& kv : t.category) {
        Rgba c;
        if (parseColor(s.get(prefix + kv.first), c)) kv.second = c;
    }
    return t;
}

}  // namespace blocks
}  // namespace acide
