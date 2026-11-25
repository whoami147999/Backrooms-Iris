// Backrooms Iris - procedural textures. Nothing is loaded from disk: every texture is
// painted in code at startup, deterministic, and tiles seamlessly.
#pragma once
#include <cstdint>
#include <vector>

namespace iris {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba; // w*h*4
    void set(int x, int y, float r, float g, float b);
};

Image make_wallpaper();     // 256x512: one wall's full height (baseboard at the bottom)
Image make_carpet();        // 512x512: one cell (2.5 m) of damp mustard carpet
Image make_ceiling_tiles(); // 512x512: 4x4 drop-ceiling tiles (one cell)
Image make_light_panel();   // 128x64: fluorescent troffer diffuser

} // namespace iris
