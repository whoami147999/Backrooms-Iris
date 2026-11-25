// Backrooms Iris - built-in 5x7 pixel font (hand-drawn, caps only: lower case is shown as
// upper case). Rendered as an 8x8-cell atlas texture.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace iris {

constexpr int FONT_COLS = 16, FONT_ROWS = 6;   // atlas grid (ASCII 32..127)
constexpr int FONT_CELL = 8;                    // pixels per cell
constexpr int FONT_ATLAS_W = FONT_COLS * FONT_CELL, FONT_ATLAS_H = 64; // power of two (Vita GPU friendly)

// RGBA8 atlas: white glyphs on transparent.
std::vector<uint8_t> font_atlas();
// width in font pixels (6 per character including spacing)
inline int text_width_px(const std::string& s) { return (int)s.size() * 6 - (s.empty() ? 0 : 1); }

} // namespace iris
