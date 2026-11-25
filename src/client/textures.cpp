#include "textures.h"

#include <algorithm>
#include <cmath>

#include "../core/hash.h"
#include "../core/math.h"

namespace iris {

void Image::set(int x, int y, float r, float g, float b) {
    uint8_t* p = &rgba[((size_t)y * w + x) * 4];
    p[0] = (uint8_t)(clampf(r, 0, 1) * 255.0f + 0.5f);
    p[1] = (uint8_t)(clampf(g, 0, 1) * 255.0f + 0.5f);
    p[2] = (uint8_t)(clampf(b, 0, 1) * 255.0f + 0.5f);
    p[3] = 255;
}

namespace {

Image blank(int w, int h) {
    Image im;
    im.w = w;
    im.h = h;
    im.rgba.assign((size_t)w * h * 4, 255);
    return im;
}

float smooth(float t) { return t * t * (3 - 2 * t); }

// Tileable value noise: lattice wraps every `px` x `pz` cells.
float vnoise(float x, float y, int px, int py, uint32_t seed) {
    int x0 = floor_to_int(x), y0 = floor_to_int(y);
    float fx = smooth(x - x0), fy = smooth(y - y0);
    auto at = [&](int ix, int iy) {
        ix = ((ix % px) + px) % px;
        iy = ((iy % py) + py) % py;
        return hash01(seed, ix, iy, 7);
    };
    float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy);
}

// Fractal noise in [0,1). (u, v) in [0,1) image space; base = lattice cells across the image.
float fbm(float u, float v, int base_x, int base_y, int octaves, uint32_t seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    int fx = base_x, fy = base_y;
    for (int o = 0; o < octaves; o++) {
        sum += amp * vnoise(u * fx, v * fy, fx, fy, seed + (uint32_t)o * 101u);
        norm += amp;
        amp *= 0.5f;
        fx *= 2;
        fy *= 2;
    }
    return sum / norm;
}

float grain(int x, int y, uint32_t seed) { return hash01(seed, x, y, 3) - 0.5f; }

} // namespace

Image make_wallpaper() {
    const int W = 256, H = 512;
    Image im = blank(W, H);
    const float base_r = 0.80f, base_g = 0.71f, base_b = 0.40f; // tired mustard
    const int baseboard = 20;  // ~0.1 m
    for (int y = 0; y < H; y++) {
        float v = (float)y / H;           // 0 = top of wall (ceiling), 1 = floor
        for (int x = 0; x < W; x++) {
            float u = (float)x / W;
            if (y >= H - baseboard) {
                // painted wooden baseboard, scuffed
                float t = (float)(y - (H - baseboard)) / baseboard;
                float sh = 0.42f + 0.08f * (t < 0.12f ? 1.0f : 0.0f) - 0.06f * t;
                float scuff = fbm(u, t, 16, 2, 3, 91) * 0.10f;
                im.set(x, y, sh + 0.10f - scuff, sh + 0.06f - scuff, sh - 0.06f - scuff);
                continue;
            }
            // vertical stripe pair every 32 px
            int sx = x % 32;
            float stripe = (sx == 0) ? -0.035f : ((sx == 1 || sx == 6) ? -0.018f : 0.0f);
            // faint repeating diamond motif between the stripes
            float cx = (float)(x % 32) - 16.0f, cy = (float)(y % 40) - 20.0f;
            float diamond = std::fabs(cx) / 9.0f + std::fabs(cy) / 12.0f;
            float motif = (diamond > 0.86f && diamond < 1.0f) ? -0.035f : 0.0f;
            if (std::fabs(cx) < 1.5f && std::fabs(cy) < 1.5f) motif = -0.04f;
            // big blotchy discolouration, yellower in places
            float blot = fbm(u, v * 2, 3, 6, 4, 17) - 0.5f;
            // water stains dripping from the ceiling
            float streak = fbm(u, v * 0.25f, 24, 1, 3, 33);
            float drip = std::max(0.0f, streak - 0.58f) * 2.6f * std::max(0.0f, 1.0f - v * 1.6f);
            float ring = fbm(u, v * 2, 6, 12, 2, 44);
            float stain_edge = (ring > 0.70f && ring < 0.715f) ? 0.025f : 0.0f;
            // grime collecting toward the floor
            float grime = std::pow(std::max(0.0f, v - 0.80f) / 0.2f, 2.0f) * 0.12f;
            float n = grain(x, y, 5) * 0.035f;
            float l = 1.0f + stripe + motif + blot * 0.16f + n - grime - stain_edge;
            float r = base_r * l - drip * 0.10f;
            float g = base_g * l - drip * 0.14f + blot * 0.02f;
            float b = base_b * l - drip * 0.16f - blot * 0.03f;
            im.set(x, y, r, g, b);
        }
    }
    return im;
}

Image make_carpet() {
    const int N = 512;
    Image im = blank(N, N);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            float u = (float)x / N, v = (float)y / N;
            // fibres: strong per-pixel noise, slightly clumped
            float fib = grain(x, y, 11) * 0.22f + (vnoise(u * 256, v * 256, 256, 256, 12) - 0.5f) * 0.18f;
            // tuft rows
            float tuft = ((x + (y / 2) % 2) % 4 == 0) ? -0.04f : 0.0f;
            // damp patches
            float damp = fbm(u, v, 6, 6, 5, 13);
            float wet = std::max(0.0f, damp - 0.56f) * 1.5f;
            float worn = (fbm(u, v, 4, 4, 3, 14) - 0.5f) * 0.10f;
            float l = 0.95f + fib + tuft + worn - wet * 0.35f;
            float r = 0.57f * l - wet * 0.06f;
            float g = 0.48f * l - wet * 0.03f;
            float b = 0.24f * l - wet * 0.02f;
            im.set(x, y, r, g, b);
        }
    }
    return im;
}

Image make_ceiling_tiles() {
    const int N = 512, T = 128; // four tiles per side (one cell)
    Image im = blank(N, N);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int tx = x % T, ty = y % T;
            float u = (float)x / N, v = (float)y / N;
            bool grid = tx < 3 || ty < 3;
            if (grid) { // aluminium T-bar painted off-white
                float sh = (tx == 1 || ty == 1) ? 0.80f : 0.68f;
                im.set(x, y, sh * 0.98f, sh * 0.95f, sh * 0.84f);
                continue;
            }
            // fissured mineral tile: pinholes + worm-like fissures
            float l = 0.90f + grain(x, y, 21) * 0.05f;
            if (hash01(21, x, y, 4) < 0.025f) l -= 0.20f;
            float fis = vnoise(u * 128, v * 128, 128, 128, 22);
            if (fis > 0.47f && fis < 0.50f) l -= 0.07f;
            // bevel shading near the grid
            if (tx < 6 || ty < 6) l -= 0.05f;
            // yellow-brown water stains
            float st = fbm(u, v, 5, 5, 4, 23);
            float stain = std::max(0.0f, st - 0.64f) * 2.0f;
            float edge = (st > 0.64f && st < 0.648f) ? 0.045f : 0.0f;
            float r = 0.86f * l - stain * 0.10f - edge;
            float g = 0.83f * l - stain * 0.16f - edge;
            float b = 0.70f * l - stain * 0.28f - edge;
            im.set(x, y, r, g, b);
        }
    }
    return im;
}

Image make_light_panel() {
    const int W = 128, H = 64;
    Image im = blank(W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            bool frame = x < 3 || y < 3 || x >= W - 3 || y >= H - 3;
            if (frame) { im.set(x, y, 0.70f, 0.69f, 0.62f); continue; }
            // prismatic diffuser: tiny diamond grid
            float d = ((x + y) % 4 == 0 || (x - y + 256) % 4 == 0) ? -0.06f : 0.0f;
            // tubes glowing behind the diffuser
            float tube = 0.0f;
            for (int t = 0; t < 2; t++) {
                float c = H * (0.33f + 0.34f * t);
                tube += std::exp(-std::pow((y - c) / 7.0f, 2.0f)) * 0.08f;
            }
            float l = 0.90f + d + tube;
            im.set(x, y, l, l * 0.99f, l * 0.92f);
        }
    }
    return im;
}

} // namespace iris
