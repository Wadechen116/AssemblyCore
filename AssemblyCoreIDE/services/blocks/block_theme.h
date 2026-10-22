#pragma once
// Colours of the block canvas, as plain numbers: no Qt here, so the same theme drives any renderer and can be tested. A theme is a table
// (category name -> colour, role -> colour); the canvas hard-codes none of them. Categories that a theme does not know get a stable colour
// derived from the name, so a block from a plug-in is never invisible.
#include <cstdint>
#include <map>
#include <string>

#include "settings.h"

namespace acide {
namespace blocks {

struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};
Rgba rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
bool parseColor(const std::string& text, Rgba& out);             // "#rrggbb" or "#rrggbbaa"
std::string formatColor(const Rgba& c);
Rgba mix(const Rgba& a, const Rgba& b, double t);                // t = 0: a, t = 1: b
Rgba withAlpha(const Rgba& c, uint8_t a);
double luminance(const Rgba& c);                                // 0..1, for choosing text that can be read on a colour

struct BlockTheme {
    std::string name;
    bool dark = false;
    Rgba canvas, grid, gridMajor;                   // background and grid lines
    Rgba blockText, blockTextOnDark;                // text on a block: dark text on light blocks, light text on dark blocks
    Rgba chip, chipText, chipBorder;                // the field "chips"
    Rgba selection, hover;                          // outlines
    Rgba error, warning;                            // badges
    Rgba hatch;                                     // the diagonal lines that mean "these two blocks cannot be nested"
    Rgba floatingOutline;                           // the dashed outline of a stack that is not part of the program
    Rgba hatFill, hatText;                          // the section start (.main / .peri / definitions)
    Rgba note, noteText;                            // comment blocks
    Rgba shadow;
    std::map<std::string, Rgba> category;           // by BlockDef.category

    Rgba categoryColor(const std::string& cat) const;
    // The text colour that reads best on `fill`.
    Rgba textOn(const Rgba& fill) const;

    static BlockTheme light();
    static BlockTheme dark_();
    static BlockTheme forMode(bool dark) { return dark ? dark_() : light(); }
    // Starts from the light or dark theme and applies "blocks.category.<Name> = #rrggbb" and "blocks.<role> = #rrggbb" overrides.
    static BlockTheme fromSettings(const Settings& s, bool dark);
};

}  // namespace blocks
}  // namespace acide
