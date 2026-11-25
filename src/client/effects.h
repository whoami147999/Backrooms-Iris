// Backrooms Iris - what your head does to the picture: low sanity, the energy drink, and
// the memory juice (a deliriant nightmare that ends in death).
#pragma once
#include <string>
#include <vector>

#include "../core/player.h"
#include "renderer.h"

namespace iris {

struct ScreenFx {
    // camera
    float roll = 0, fov_add = 0, fog_mult = 1;
    // full-screen distortion (PC)
    float wobble = 0, ghost = 0;
    // overlays
    float vignette = 0;                 // black edges
    float tint[4] = {0, 0, 0, 0};       // colour wash (rgba)
    float red_flash = 0;
    float grain = 0;
    float blackout = 0;                 // 0..1 fade to black
    struct Spider { float x, y, rot, size; };
    std::vector<Spider> spiders;        // normalised screen coords
    std::string whisper;                // text on screen
    float whisper_alpha = 0;
    float wx = 0.5f, wy = 0.5f;
    std::vector<Phantom> phantoms;      // things in the world that aren't there
};

ScreenFx compute_fx(const PlayerState& me, Vec3 eye, float yaw, float time, int seed);

} // namespace iris
