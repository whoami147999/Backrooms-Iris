#include "textures.h"

#include <algorithm>
#include <cmath>

#include "../core/hash.h"
#include "../core/math.h"
#include "font.h"

namespace iris {

// =================================================================== Image
Image::Image(int w_, int h_, uint32_t fill) : w(w_), h(h_), rgba((size_t)w_ * h_ * 4) {
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = (uint8_t)(fill >> 16);
        rgba[i + 1] = (uint8_t)(fill >> 8);
        rgba[i + 2] = (uint8_t)fill;
        rgba[i + 3] = (uint8_t)(fill >> 24);
    }
}

void Image::set(int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t* p = &rgba[((size_t)y * w + x) * 4];
    p[0] = (uint8_t)(clampf(r, 0, 1) * 255.0f + 0.5f);
    p[1] = (uint8_t)(clampf(g, 0, 1) * 255.0f + 0.5f);
    p[2] = (uint8_t)(clampf(b, 0, 1) * 255.0f + 0.5f);
    p[3] = (uint8_t)(clampf(a, 0, 1) * 255.0f + 0.5f);
}

void Image::blend(int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
    uint8_t* p = &rgba[((size_t)y * w + x) * 4];
    float da = p[3] / 255.0f, oa = a + da * (1 - a);
    auto mixc = [&](uint8_t d, float s) { return oa > 0 ? (s * a + d / 255.0f * da * (1 - a)) / oa : 0.0f; };
    set(x, y, mixc(p[0], r), mixc(p[1], g), mixc(p[2], b), oa);
}

void Image::rect(int x0, int y0, int x1, int y1, float r, float g, float b, float a) {
    for (int y = std::max(0, y0); y < std::min(h, y1); y++)
        for (int x = std::max(0, x0); x < std::min(w, x1); x++) blend(x, y, r, g, b, a);
}

void Image::disc(float cx, float cy, float rad, float r, float g, float b, float a, float soft) {
    for (int y = (int)(cy - rad - 2); y <= (int)(cy + rad + 2); y++)
        for (int x = (int)(cx - rad - 2); x <= (int)(cx + rad + 2); x++) {
            float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float k = clampf((rad - d) / soft + 0.5f, 0, 1);
            if (k > 0) blend(x, y, r, g, b, a * k);
        }
}

void Image::line(float x0, float y0, float x1, float y1, float width, float r, float g, float b, float a) {
    float len = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    int steps = std::max(1, (int)(len * 2));
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps;
        disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, width * 0.5f, r, g, b, a * 0.6f, 0.8f);
    }
}

void Image::text(int x, int y, int scale, const std::string& s, float r, float g, float b, float a) {
    static std::vector<uint8_t> atlas = font_atlas();
    for (size_t i = 0; i < s.size(); i++) {
        int c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        c -= 32;
        if (c < 0 || c >= FONT_COLS * FONT_ROWS) continue;
        int ox = (c % FONT_COLS) * FONT_CELL, oy = (c / FONT_COLS) * FONT_CELL;
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++) {
                if (!atlas[((size_t)(oy + gy) * FONT_ATLAS_W + ox + gx) * 4 + 3]) continue;
                rect(x + ((int)i * 6 + gx) * scale, y + gy * scale, x + ((int)i * 6 + gx + 1) * scale, y + (gy + 1) * scale, r, g, b, a);
            }
    }
}

namespace {

float smooth(float t) { return t * t * (3 - 2 * t); }

// Tileable value noise: lattice wraps every px x py cells.
float vnoise(float x, float y, int px, int py, uint32_t seed) {
    int x0 = floor_to_int(x), y0 = floor_to_int(y);
    float fx = smooth(x - x0), fy = smooth(y - y0);
    auto at = [&](int ix, int iy) {
        ix = ((ix % px) + px) % px;
        iy = ((iy % py) + py) % py;
        return hash01(seed, ix, iy, 7);
    };
    return lerpf(lerpf(at(x0, y0), at(x0 + 1, y0), fx), lerpf(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
}

// Fractal noise in [0,1). (u, v) in [0,1) image space; base = lattice cells across the image.
float fbm(float u, float v, int bx, int by, int octaves, uint32_t seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < octaves; o++) {
        sum += amp * vnoise(u * bx, v * by, bx, by, seed + (uint32_t)o * 101u);
        norm += amp;
        amp *= 0.5f;
        bx *= 2;
        by *= 2;
    }
    return sum / norm;
}

float grain(int x, int y, uint32_t seed) { return hash01(seed, x, y, 3) - 0.5f; }

// Calls f(x, y, u, v) -> (r, g, b) for every pixel.
template <typename F>
Image paint(int w, int h, F f) {
    Image im(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float r, g, b;
            f(x, y, (float)x / w, (float)y / h, r, g, b);
            im.set(x, y, r, g, b);
        }
    return im;
}

// ------------------------------------------------------------------ Level 0
Image wallpaper(int N) {
    const int W = N / 2, H = N;
    const int baseboard = H / 25;
    return paint(W, H, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        if (y >= H - baseboard) {
            float t = (float)(y - (H - baseboard)) / baseboard;
            float sh = 0.42f + 0.08f * (t < 0.12f ? 1.0f : 0.0f) - 0.06f * t;
            float scuff = fbm(u, t, 16, 2, 3, 91) * 0.10f;
            r = sh + 0.10f - scuff; g = sh + 0.06f - scuff; b = sh - 0.06f - scuff;
            return;
        }
        int period = W / 8;
        int sx = x % period;
        float stripe = (sx == 0) ? -0.035f : ((sx == 1 || sx == period / 5) ? -0.018f : 0.0f);
        float cx = (float)(x % period) - period / 2.0f, cy = (float)(y % (period * 5 / 4)) - period * 5 / 8.0f;
        float diamond = std::fabs(cx) / (period * 0.28f) + std::fabs(cy) / (period * 0.37f);
        float motif = (diamond > 0.86f && diamond < 1.0f) ? -0.035f : 0.0f;
        float blot = fbm(u, v * 2, 3, 6, 4, 17) - 0.5f;
        float streak = fbm(u, v * 0.25f, 24, 1, 3, 33);
        float drip = std::max(0.0f, streak - 0.58f) * 2.6f * std::max(0.0f, 1.0f - v * 1.6f);
        float grime = std::pow(std::max(0.0f, v - 0.80f) / 0.2f, 2.0f) * 0.12f;
        float l = 1.0f + stripe + motif + blot * 0.16f + grain(x, y, 5) * 0.035f - grime;
        r = 0.80f * l - drip * 0.10f;
        g = 0.71f * l - drip * 0.14f + blot * 0.02f;
        b = 0.40f * l - drip * 0.16f - blot * 0.03f;
    });
}

Image carpet(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float fib = grain(x, y, 11) * 0.22f + (vnoise(u * N / 2, v * N / 2, N / 2, N / 2, 12) - 0.5f) * 0.18f;
        float tuft = ((x + (y / 2) % 2) % 4 == 0) ? -0.04f : 0.0f;
        float wet = std::max(0.0f, fbm(u, v, 6, 6, 5, 13) - 0.56f) * 1.5f;
        float worn = (fbm(u, v, 4, 4, 3, 14) - 0.5f) * 0.10f;
        float l = 0.95f + fib + tuft + worn - wet * 0.35f;
        r = 0.57f * l - wet * 0.06f; g = 0.48f * l - wet * 0.03f; b = 0.24f * l - wet * 0.02f;
    });
}

Image ceiling_tiles(int N) {
    const int T = N / 4;
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int tx = x % T, ty = y % T;
        if (tx < 3 || ty < 3) {
            float sh = (tx == 1 || ty == 1) ? 0.80f : 0.68f;
            r = sh * 0.98f; g = sh * 0.95f; b = sh * 0.84f;
            return;
        }
        float l = 0.90f + grain(x, y, 21) * 0.05f;
        if (hash01(21, x, y, 4) < 0.025f) l -= 0.20f;
        float fis = vnoise(u * N / 4, v * N / 4, N / 4, N / 4, 22);
        if (fis > 0.47f && fis < 0.50f) l -= 0.07f;
        if (tx < 6 || ty < 6) l -= 0.05f;
        float st = fbm(u, v, 5, 5, 4, 23);
        float stain = std::max(0.0f, st - 0.64f) * 2.0f;
        float edge = (st > 0.64f && st < 0.648f) ? 0.045f : 0.0f;
        r = 0.86f * l - stain * 0.10f - edge; g = 0.83f * l - stain * 0.16f - edge; b = 0.70f * l - stain * 0.28f - edge;
    });
}

Image light_panel(int W, int H, float warm) {
    return paint(W, H, [&](int x, int y, float, float, float& r, float& g, float& b) {
        bool frame = x < 3 || y < 3 || x >= W - 3 || y >= H - 3;
        if (frame) { r = 0.70f; g = 0.69f; b = 0.62f; return; }
        float d = ((x + y) % 4 == 0 || (x - y + 256) % 4 == 0) ? -0.06f : 0.0f;
        float tube = 0;
        for (int t = 0; t < 2; t++) tube += std::exp(-std::pow((y - H * (0.33f + 0.34f * t)) / (H * 0.11f), 2.0f)) * 0.08f;
        float l = 0.90f + d + tube;
        r = l; g = l * (0.99f - warm * 0.12f); b = l * (0.92f - warm * 0.35f);
    });
}

// ------------------------------------------------------------------ Level 1
Image concrete(int W, int H, float base, uint32_t seed, bool walls) {
    return paint(W, H, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float blot = fbm(u, v, 4, 4 * H / W, 5, seed) - 0.5f;
        float pits = hash01(seed, x, y, 9) < 0.012f ? -0.12f : 0.0f;
        float l = base + blot * 0.22f + grain(x, y, seed) * 0.05f + pits;
        if (walls) {
            // formwork seams and water streaks running down
            if (y % (H / 3) < 2) l -= 0.06f;
            float streak = fbm(u, v * 0.2f, 20, 1, 3, seed + 5);
            l -= std::max(0.0f, streak - 0.6f) * 0.5f * (1.0f - v * 0.5f);
            if (hash01(seed, x / 6, y / 6, 33) < 0.004f) { r = 0.42f; g = 0.24f; b = 0.14f; return; } // rust bleed
        } else {
            float crack = vnoise(u * 9, v * 9, 9, 9, seed + 7);
            l -= std::max(0.0f, 0.012f - std::fabs(crack - 0.5f)) * 5.0f; // hairline cracks
        }
        r = l; g = l * 1.0f; b = l * 0.97f;
    });
}

Image wet_concrete(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        // concrete with standing water: darker, smoother, with a soft reflection
        float blot = fbm(u, v, 4, 4, 5, 141) - 0.5f;
        float sheen = std::pow(fbm(u, v, 4, 4, 3, 142), 2.5f) * 0.28f;
        float l = 0.30f + blot * 0.08f + sheen + grain(x, y, 143) * 0.015f;
        r = l * 0.97f; g = l; b = l * 1.02f;
    });
}

Image crate(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int P = N / 8;                    // 8 planks: divides the texture, no seam
        int plank = y / P;
        float edge = (y % P < 2) ? -0.22f : ((y % P == 2) ? 0.06f : 0.0f);
        float frame = (x < N / 16 || x >= N - N / 16) ? -0.07f : 0.0f;
        float grainw = (vnoise(u * 2, v * 64 + plank * 5.0f, 2, 64, 151 + (uint32_t)plank) - 0.5f) * 0.22f;
        float tone = (hash01(152, plank, 0, 2) - 0.5f) * 0.10f;
        float l = 0.62f + grainw + edge + frame + tone + grain(x, y, 152) * 0.035f;
        if ((x == N / 32 || x == N - N / 32) && (y % P) == P / 2) l = 0.22f; // nails
        r = l * 0.86f; g = l * 0.66f; b = l * 0.40f;
    });
}

// ------------------------------------------------------------------ Level 5
Image hotel_wall(int N, bool with_door) {
    const int W = N / 2, H = N;
    Image im = paint(W, H, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float wear = fbm(u, v, 3, 6, 3, 161) - 0.5f;
        if (v > 0.62f) { // dark wood wainscot with vertical boards
            float board = (x % (W / 6) < 2) ? -0.10f : 0.0f;
            float gr = (vnoise(u * 40, v * 3, 40, 3, 162) - 0.5f) * 0.12f;
            float rail = (v < 0.64f) ? 0.10f : 0.0f;
            float l = 0.30f + board + gr + rail + wear * 0.08f;
            r = l * 0.95f; g = l * 0.55f; b = l * 0.32f;
            if (v > 0.965f) { r *= 0.6f; g *= 0.6f; b *= 0.6f; }
            return;
        }
        // faded maroon damask
        float px = (float)(x % (W / 4)) / (W / 4) - 0.5f, py = (float)(y % (W / 3)) / (W / 3) - 0.5f;
        float m = std::fabs(px) * 2.2f + std::fabs(py) * 1.6f;
        float leaf = std::fabs(std::sin(px * 12) * 0.3f - py);
        float pat = (m < 0.55f && leaf < 0.18f) ? 0.10f : ((m > 0.9f && m < 1.0f) ? 0.06f : 0.0f);
        float l = 1.0f + pat + wear * 0.25f + grain(x, y, 163) * 0.04f;
        r = 0.42f * l; g = 0.12f * l; b = 0.13f * l;
        if (v > 0.6f) { r = 0.55f; g = 0.42f; b = 0.25f; } // brass chair rail
    });
    if (with_door) {
        int dx0 = W * 28 / 100, dx1 = W * 72 / 100, dy0 = H * 22 / 100;
        im.rect(dx0 - 4, dy0 - 4, dx1 + 4, H, 0.16f, 0.09f, 0.05f);           // frame
        for (int y = dy0; y < H; y++)
            for (int x = dx0; x < dx1; x++) {
                float u = (float)(x - dx0) / (dx1 - dx0), v = (float)(y - dy0) / (H - dy0);
                bool panel = (u > 0.15f && u < 0.85f) && ((v > 0.07f && v < 0.45f) || (v > 0.55f && v < 0.93f));
                bool panel_edge = panel && (u < 0.18f || u > 0.82f || std::fabs(v - 0.07f) < 0.01f || std::fabs(v - 0.45f) < 0.01f ||
                                            std::fabs(v - 0.55f) < 0.01f || std::fabs(v - 0.93f) < 0.01f);
                float gr = (vnoise(u * 4, v * 50, 4, 50, 164) - 0.5f) * 0.1f;
                float l = 0.36f + gr + (panel_edge ? -0.1f : 0.0f) + (panel ? 0.03f : 0.0f);
                im.set(x, y, l * 0.9f, l * 0.52f, l * 0.3f);
            }
        im.disc(dx1 - (dx1 - dx0) * 0.14f, dy0 + (H - dy0) * 0.52f, W * 0.018f, 0.8f, 0.65f, 0.3f); // knob
        im.rect((dx0 + dx1) / 2 - W / 14, dy0 + (H - dy0) / 10 - 2, (dx0 + dx1) / 2 + W / 14, dy0 + (H - dy0) / 10 + W / 14, 0.75f, 0.6f, 0.3f);
        im.text((dx0 + dx1) / 2 - 8 * std::max(1, W / 256), dy0 + (H - dy0) / 10, std::max(1, W / 256), "237", 0.2f, 0.12f, 0.05f);
    }
    return im;
}

Image hotel_carpet(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int T = N / 4;
        float px = (float)(x % T) / T - 0.5f, py = (float)(y % T) / T - 0.5f;
        float dia = std::fabs(px) + std::fabs(py);
        bool gold = (dia > 0.36f && dia < 0.42f) || (std::fabs(px) < 0.04f && std::fabs(py) < 0.04f) ||
                    (dia < 0.16f && dia > 0.12f);
        float wear = fbm(u, v, 4, 4, 4, 171);
        float l = 0.9f + grain(x, y, 172) * 0.15f - std::max(0.0f, wear - 0.6f) * 0.8f;
        if (gold) { r = 0.62f * l; g = 0.45f * l; b = 0.16f * l; }
        else { r = 0.40f * l; g = 0.06f * l; b = 0.07f * l; }
    });
}

Image parquet(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int bw = N / 8;
        int bx = x / bw, by = y / (bw * 2);
        bool flip = (bx + by) & 1;
        int lx = flip ? y % bw : x % bw;
        float edge = (lx < 1) ? -0.15f : 0.0f;
        float gr = (vnoise(u * 50, v * 5, 50, 5, 181 + (uint32_t)(bx * 7 + by)) - 0.5f) * 0.12f;
        float l = 0.55f + gr + edge + (hash01(182, bx, by, 1) - 0.5f) * 0.12f;
        r = l * 0.75f; g = l * 0.48f; b = l * 0.26f;
    });
}

Image plaster(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float l = 0.78f + (fbm(u, v, 5, 5, 4, 191) - 0.5f) * 0.12f + grain(x, y, 192) * 0.03f;
        float crack = vnoise(u * 7, v * 7, 7, 7, 193);
        if (crack > 0.497f && crack < 0.503f) l -= 0.12f;
        r = l * 0.95f; g = l * 0.88f; b = l * 0.74f;
    });
}

// ------------------------------------------------------------------ Level 8
Image rock(int N, float base, uint32_t seed) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float n = fbm(u, v, 4, 4, 6, seed);
        float strata = std::sin((v + (n - 0.5f) * 0.08f) * 2 * PI * 5) * 0.035f; // 5 whole bands: tiles cleanly
        float crev = std::max(0.0f, 0.06f - std::fabs(vnoise(u * 6, v * 6, 6, 6, seed + 3) - 0.5f)) * 1.2f; // soft crevices
        float l = base + (n - 0.5f) * 0.32f + strata - crev + grain(x, y, seed) * 0.05f;
        r = l * 1.0f; g = l * 0.9f; b = l * 0.78f;
    });
}

Image fungus(int N) {
    Image im = rock(N / 2, 0.16f, 201);
    Rng rng(202);
    for (int i = 0; i < 40; i++) {
        float x = rng.range(4, N / 2 - 4.0f), y = rng.range(4, N / 2 - 4.0f), s = rng.range(2, 7);
        im.disc(x, y, s * 2.2f, 0.2f, 0.9f, 0.5f, 0.25f, 3);
        im.disc(x, y, s, 0.6f, 1.0f, 0.75f, 1.0f, 1.2f);
    }
    return im;
}

// ------------------------------------------------------------------ Level 9
Image siding(int N) {
    const int W = N / 2, H = N;
    Image im = paint(W, H, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int board = H / 22;
        float lip = (y % board) < 2 ? -0.18f : ((y % board) < 4 ? 0.06f : 0.0f);
        float dirt = (fbm(u, v, 3, 6, 4, 211) - 0.5f) * 0.12f - std::max(0.0f, v - 0.85f) * 0.6f;
        float l = 0.88f + lip + dirt + grain(x, y, 212) * 0.03f;
        r = g = b = l; // painted white: the house tint colours it
    });
    // a window, curtains drawn, maybe a light on
    int x0 = W * 25 / 100, x1 = W * 75 / 100, y0 = H * 20 / 100, y1 = H * 52 / 100;
    im.rect(x0 - 5, y0 - 5, x1 + 5, y1 + 7, 0.95f, 0.95f, 0.93f);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float u = (float)(x - x0) / (x1 - x0), v = (float)(y - y0) / (y1 - y0);
            bool mull = std::fabs(u - 0.5f) < 0.03f || std::fabs(v - 0.5f) < 0.03f;
            float glass = 0.05f + v * 0.04f + (u + v < 0.5f ? 0.05f : 0.0f);
            if (mull) im.set(x, y, 0.92f, 0.92f, 0.9f);
            else im.set(x, y, glass, glass * 1.05f, glass * 1.3f);
        }
    // front door on the lower half
    int dx0 = W * 35 / 100, dx1 = W * 65 / 100, dy0 = H * 62 / 100;
    im.rect(dx0 - 4, dy0 - 4, dx1 + 4, H, 0.9f, 0.9f, 0.88f);
    im.rect(dx0, dy0, dx1, H, 0.25f, 0.2f, 0.18f);
    im.disc(dx1 - (dx1 - dx0) * 0.18f, dy0 + (H - dy0) * 0.5f, W * 0.015f, 0.7f, 0.6f, 0.3f);
    return im;
}

Image shingles(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int row = y / (N / 8);
        int off = (row & 1) * (N / 16);
        int tab = ((x + off) % N) / (N / 8);
        float gap = (((x + off) % N) % (N / 8) < 2 || y % (N / 8) < 2) ? -0.12f : 0.0f;
        float l = 0.30f + gap + (hash01(221, tab, row, 1) - 0.5f) * 0.08f + grain(x, y, 222) * 0.08f +
                  (fbm(u, v, 4, 4, 3, 223) - 0.5f) * 0.1f;
        r = l * 1.0f; g = l * 0.92f; b = l * 0.86f;
    });
}

Image grass(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float blade = grain(x, y, 231) * 0.35f + (vnoise(u * N / 4, v * N / 4, N / 4, N / 4, 232) - 0.5f) * 0.2f;
        float patch = fbm(u, v, 4, 4, 4, 233) - 0.5f;
        float l = 0.85f + blade + patch * 0.3f;
        r = 0.18f * l + std::max(0.0f, patch) * 0.12f; g = 0.30f * l; b = 0.12f * l;
    });
}

Image asphalt(int N, bool lines) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float agg = grain(x, y, 241) * 0.12f + (hash01(242, x, y, 1) < 0.03f ? 0.12f : 0.0f);
        float patch = (fbm(u, v, 3, 3, 4, 243) - 0.5f) * 0.10f;
        float crack = vnoise(u * 5, v * 5, 5, 5, 244);
        float c = (crack > 0.495f && crack < 0.505f) ? -0.07f : 0.0f;
        float l = 0.22f + agg + patch + c;
        r = l; g = l; b = l * 1.03f;
        if (lines && std::fabs(u - 0.5f) < 0.015f && (y / (N / 8)) % 2 == 0) { r = 0.75f; g = 0.65f; b = 0.3f; }
    });
}

Image sidewalk(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int S = N / 2;
        float seam = (x % S < 2 || y % S < 2) ? -0.14f : 0.0f;
        float l = 0.58f + seam + (fbm(u, v, 4, 4, 4, 251) - 0.5f) * 0.12f + grain(x, y, 252) * 0.05f;
        r = l; g = l * 0.99f; b = l * 0.95f;
    });
}

// ------------------------------------------------------------------ Level 11
Image facade(int N) {
    const int W = N / 2, H = N;
    return paint(W, H, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int cw = W / 4, ch = H / 8;   // periods divide the texture: windows line up across faces
        int lx = x % cw, ly = y % ch;
        bool window = lx > cw * 0.15f && lx < cw * 0.85f && ly > ch * 0.2f && ly < ch * 0.85f;
        float stone = 0.80f + (fbm(u, v, 3, 6, 3, 261) - 0.5f) * 0.1f + grain(x, y, 262) * 0.03f;
        if (!window) { r = g = b = stone; return; }
        // glass reflecting a grey sky; the occasional blind pulled down
        float refl = 0.30f + (1.0f - (float)ly / ch) * 0.15f + (lx + ly < cw * 0.5f ? 0.06f : 0.0f);
        bool blind = hash01(263, x / cw, y / ch, 1) < 0.2f && ly < ch * 0.5f;
        r = blind ? 0.7f : refl * 0.85f; g = blind ? 0.68f : refl * 0.95f; b = blind ? 0.6f : refl * 1.15f;
    });
}

Image metal(int N, float base) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float l = base + (vnoise(u * 2, v * 40, 2, 40, 271) - 0.5f) * 0.08f + grain(x, y, 272) * 0.03f;
        r = l; g = l * 1.01f; b = l * 1.04f;
    });
}

// ------------------------------------------------------------------ exit door
Image exit_door(int N) {
    const int W = N / 2, H = N;
    Image im = paint(W, H, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        bool frame = u < 0.06f || u > 0.94f || v < 0.03f;
        float scratches = vnoise(u * 60, v * 4, 60, 4, 281) > 0.82f ? 0.06f : 0.0f;
        float l = (frame ? 0.30f : 0.42f) + scratches + grain(x, y, 282) * 0.03f;
        r = l * 0.80f; g = l * 0.88f; b = l * 0.82f;
    });
    im.rect(W * 12 / 100, H * 52 / 100, W * 88 / 100, H * 56 / 100, 0.75f, 0.75f, 0.72f); // push bar
    im.rect(W * 30 / 100, H * 14 / 100, W * 70 / 100, H * 30 / 100, 0.08f, 0.1f, 0.1f);   // little window
    im.rect(W * 32 / 100, H * 15 / 100, W * 68 / 100, H * 29 / 100, 0.02f, 0.03f, 0.03f);
    return im;
}

// ------------------------------------------------------------------ Level 2
Image tunnel_wall(int N) {
    return paint(N / 2, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float blot = fbm(u, v, 3, 6, 5, 401) - 0.5f;
        float rust = std::max(0.0f, fbm(u, v * 0.25f, 16, 2, 3, 402) - 0.55f) * 2.0f * (1.0f - v * 0.6f);
        float band = (v > 0.86f) ? -0.08f : 0.0f;                    // grime line along the floor
        float seam = (x % (N / 8) < 2) ? -0.05f : 0.0f;               // panel seams
        float l = 0.34f + blot * 0.18f + band + seam + grain(x, y, 403) * 0.04f;
        r = l * 1.02f + rust * 0.20f; g = l * 0.96f + rust * 0.06f; b = l * 0.88f - rust * 0.04f;
    });
}
Image grate(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int c = N / 16;
        int lx = x % c, ly = y % c;
        bool hole = lx > c / 4 && lx < c * 3 / 4 && ly > c / 4 && ly < c * 3 / 4;
        float rust = fbm(u, v, 4, 4, 4, 411);
        float l = hole ? 0.04f : 0.36f + (rust - 0.5f) * 0.2f + grain(x, y, 412) * 0.05f;
        r = l * 1.05f + (hole ? 0 : std::max(0.0f, rust - 0.55f) * 0.25f); g = l * 0.95f; b = l * 0.85f;
    });
}
Image rusty_pipe(int N) {
    return paint(N / 4, N / 4, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float band = (y % (N / 16) < 2) ? -0.10f : 0.0f;
        float rust = fbm(u, v, 4, 4, 4, 421);
        float shade = 0.75f + 0.25f * std::sin(u * PI);               // round-ish
        float l = (0.38f + band + grain(x, y, 422) * 0.05f) * shade;
        r = l * 1.1f + std::max(0.0f, rust - 0.5f) * 0.35f; g = l * 0.9f + std::max(0.0f, rust - 0.5f) * 0.1f; b = l * 0.75f;
    });
}
Image caged_bulb(int N) {
    return paint(N / 8, N / 8, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f));
        bool cage = (x % (N / 32) == 0) || (y % (N / 32) == 0);
        float glow = clampf(1.2f - d * 2.2f, 0, 1);
        r = cage ? 0.2f : 0.5f + glow * 0.5f; g = cage ? 0.18f : 0.35f + glow * 0.55f; b = cage ? 0.15f : 0.15f + glow * 0.4f;
    });
}

// ------------------------------------------------------------------ Level 4
Image cubicle_fabric(int N) {
    return paint(N / 2, N / 2, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float weave = ((x + y) % 2 == 0 ? 0.02f : -0.02f) + (vnoise(u * 64, v * 64, 64, 64, 431) - 0.5f) * 0.06f;
        float trim = (v > 0.93f || v < 0.03f) ? -0.18f : 0.0f;        // aluminium top/bottom trim
        float dirt = (fbm(u, v, 3, 3, 3, 432) - 0.5f) * 0.08f;
        float l = 0.52f + weave + dirt + trim;
        r = l * 0.80f; g = l * 0.86f; b = l * 0.98f;
        if (trim < 0) { r = g = b = 0.55f + weave; }
    });
}
Image office_carpet(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int T = N / 4;
        int tx = x / T, ty = y / T;
        bool turn = ((tx + ty) & 1) != 0;                              // carpet tiles laid at 90 degrees
        float rib = std::sin(((turn ? x : y) % 8) * PI / 4) * 0.03f;
        float seam = (x % T == 0 || y % T == 0) ? -0.05f : 0.0f;
        float tile = (hash01(441, tx, ty, 1) - 0.5f) * 0.05f;
        float stain = std::max(0.0f, fbm(u, v, 4, 4, 4, 442) - 0.62f) * 0.7f;
        float l = 0.40f + rib + seam + tile + grain(x, y, 443) * 0.08f - stain;
        r = l * 0.78f; g = l * 0.84f; b = l * 0.96f;
    });
}
Image desk_side(int N) {
    return paint(N / 2, N / 2, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float l = 0.55f + (vnoise(u * 2, v * 32, 2, 32, 451) - 0.5f) * 0.04f + grain(x, y, 452) * 0.02f;
        if (v < 0.08f) l = 0.42f;                                      // desktop edge
        r = l * 0.92f; g = l * 0.92f; b = l * 0.90f;
    });
}
Image desk_top(int N) {
    Image im = paint(N / 2, N / 2, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float gr = (vnoise(u * 3, v * 40, 3, 40, 461) - 0.5f) * 0.12f;
        float l = 0.52f + gr + grain(x, y, 462) * 0.02f;
        r = l * 0.80f; g = l * 0.62f; b = l * 0.42f;
    });
    Rng rng(463); // scattered papers
    for (int i = 0; i < 5; i++) {
        int px = (int)rng.range(0, N / 2 - N / 8.0f), py = (int)rng.range(0, N / 2 - N / 6.0f);
        im.rect(px, py, px + N / 10, py + N / 7, 0.9f, 0.9f, 0.86f, 0.95f);
        for (int l = 0; l < 6; l++) im.rect(px + 3, py + 5 + l * 5, px + N / 10 - 3, py + 6 + l * 5, 0.4f, 0.4f, 0.45f, 0.6f);
    }
    return im;
}

// ------------------------------------------------------------------ Level 37
Image pool_tiles(int N, float r0, float g0, float b0, float water) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int T = N / 16;
        bool grout = (x % T) < 2 || (y % T) < 2;
        float tile = (hash01(471, x / T, y / T, 1) - 0.5f) * 0.03f;
        float l = grout ? 0.72f : 0.92f + tile + grain(x, y, 472) * 0.015f;
        r = l * r0; g = l * g0; b = l * b0;
        if (water > 0) { // rippled light from the water above it
            float c1 = vnoise(u * 8 + 0.3f, v * 8, 8, 8, 473), c2 = vnoise(u * 8, v * 8 + 0.7f, 8, 8, 474);
            float caustic = std::pow(clampf(1.0f - std::fabs(c1 - c2) * 6.0f, 0, 1), 3.0f);
            r = lerpf(r, 0.55f, water * 0.45f) + caustic * 0.18f * water;
            g = lerpf(g, 0.85f, water * 0.45f) + caustic * 0.20f * water;
            b = lerpf(b, 0.95f, water * 0.45f) + caustic * 0.22f * water;
        }
    });
}

// ------------------------------------------------------------------ Level Fun
Image party_wall(int N) {
    Image im = paint(N / 2, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int st = (x / (N / 16)) % 4;
        static const float C[4][3] = {{0.98f, 0.80f, 0.85f}, {0.98f, 0.93f, 0.70f}, {0.75f, 0.90f, 0.98f}, {0.85f, 0.95f, 0.78f}};
        float l = 0.95f + grain(x, y, 481) * 0.03f + (fbm(u, v, 3, 6, 3, 482) - 0.5f) * 0.06f;
        if (v > 0.93f) l *= 0.6f; // baseboard
        r = C[st][0] * l; g = C[st][1] * l; b = C[st][2] * l;
    });
    Rng rng(483);
    for (int i = 0; i < N / 4; i++) { // confetti
        float x = rng.range(0, (float)N / 2), y = rng.range(0, N * 0.9f);
        static const float C[5][3] = {{0.95f, 0.2f, 0.3f}, {0.2f, 0.6f, 0.95f}, {0.98f, 0.8f, 0.1f}, {0.3f, 0.85f, 0.4f}, {0.8f, 0.3f, 0.9f}};
        const float* c = C[i % 5];
        im.disc(x, y, 1.4f + rng.f01(), c[0], c[1], c[2], 0.9f, 0.8f);
    }
    return im;
}
Image confetti_carpet(int N) {
    Image im = carpet(N);
    Rng rng(491);
    static const float C[5][3] = {{0.95f, 0.2f, 0.3f}, {0.2f, 0.6f, 0.95f}, {0.98f, 0.8f, 0.1f}, {0.3f, 0.85f, 0.4f}, {0.8f, 0.3f, 0.9f}};
    for (int i = 0; i < N; i++) {
        const float* c = C[i % 5];
        float x = rng.range(2, N - 2.0f), y = rng.range(2, N - 2.0f);
        im.rect((int)x, (int)y, (int)x + 2 + (i % 2), (int)y + 3 - (i % 2), c[0], c[1], c[2], 0.95f);
    }
    return im;
}

} // namespace

// =================================================================== levels
void make_level_textures(LevelId level, int N, Image out[MAT_COUNT]) {
    for (int i = 0; i < MAT_COUNT; i++) out[i] = Image();
    out[MAT_DOOR] = exit_door(N);
    switch (level) {
        case LevelId::L0:
            out[MAT_WALL] = wallpaper(N);
            out[MAT_FLOOR] = carpet(N);
            out[MAT_CEIL] = ceiling_tiles(N);
            out[MAT_LIGHT] = light_panel(N / 4, N / 8, 0);
            out[MAT_PROP] = out[MAT_WALL];
            break;
        case LevelId::L1:
            out[MAT_WALL] = concrete(N / 2, N, 0.56f, 301, true);
            out[MAT_FLOOR] = concrete(N, N, 0.46f, 302, false);
            out[MAT_FLOOR2] = wet_concrete(N);
            out[MAT_CEIL] = concrete(N / 2, N / 2, 0.30f, 303, false);
            out[MAT_LIGHT] = light_panel(N / 4, N / 16, 0);
            out[MAT_BLOCK] = crate(N / 2);
            out[MAT_ROOF] = out[MAT_BLOCK];
            out[MAT_PROP] = out[MAT_WALL];
            break;
        case LevelId::L5:
            out[MAT_WALL] = hotel_wall(N, false);
            out[MAT_BLOCK] = hotel_wall(N, true);
            out[MAT_FLOOR] = hotel_carpet(N);
            out[MAT_FLOOR2] = parquet(N);
            out[MAT_CEIL] = plaster(N / 2);
            out[MAT_LIGHT] = light_panel(N / 8, N / 8, 1.0f);
            out[MAT_PROP] = parquet(N / 4);
            break;
        case LevelId::L8:
            out[MAT_WALL] = rock(N, 0.36f, 311);
            out[MAT_BLOCK] = out[MAT_WALL];
            out[MAT_FLOOR] = rock(N, 0.26f, 312);
            out[MAT_CEIL] = rock(N / 2, 0.22f, 313);
            out[MAT_LIGHT] = fungus(N);
            out[MAT_PROP] = out[MAT_WALL];
            break;
        case LevelId::L9:
            out[MAT_BLOCK] = siding(N);
            out[MAT_ROOF] = shingles(N / 2);
            out[MAT_FLOOR] = grass(N);
            out[MAT_FLOOR2] = asphalt(N, false);
            out[MAT_FLOOR3] = sidewalk(N / 2);
            out[MAT_LIGHT] = light_panel(N / 8, N / 8, 1.0f);
            out[MAT_PROP] = metal(N / 4, 0.35f);
            out[MAT_WALL] = out[MAT_PROP];
            break;
        case LevelId::L11:
            out[MAT_BLOCK] = facade(N);
            out[MAT_ROOF] = concrete(N / 2, N / 2, 0.45f, 321, false);
            out[MAT_FLOOR] = sidewalk(N / 2);
            out[MAT_FLOOR2] = asphalt(N, true);
            out[MAT_FLOOR3] = sidewalk(N / 2);
            out[MAT_PROP] = metal(N / 4, 0.4f);
            out[MAT_WALL] = out[MAT_PROP];
            out[MAT_LIGHT] = light_panel(N / 8, N / 8, 0);
            break;
        case LevelId::L2:
            out[MAT_WALL] = tunnel_wall(N);
            out[MAT_BLOCK] = out[MAT_WALL];
            out[MAT_FLOOR] = concrete(N, N, 0.30f, 404, false);
            out[MAT_FLOOR2] = grate(N);
            out[MAT_CEIL] = concrete(N / 2, N / 2, 0.24f, 405, false);
            out[MAT_LIGHT] = caged_bulb(N);
            out[MAT_PROP] = rusty_pipe(N);
            break;
        case LevelId::L4:
            out[MAT_WALL] = cubicle_fabric(N);
            out[MAT_FLOOR] = office_carpet(N);
            out[MAT_CEIL] = ceiling_tiles(N);
            out[MAT_LIGHT] = light_panel(N / 4, N / 8, 0);
            out[MAT_BLOCK] = desk_side(N);
            out[MAT_ROOF] = desk_top(N);
            out[MAT_PROP] = concrete(N / 2, N, 0.80f, 406, true);
            break;
        case LevelId::L6: {
            // the lobby with the lights cut: same rooms, colour drained out of everything
            out[MAT_WALL] = wallpaper(N);
            for (size_t i = 0; i < out[MAT_WALL].rgba.size(); i += 4) {
                uint8_t* p = &out[MAT_WALL].rgba[i];
                int l = (p[0] * 3 + p[1] * 5 + p[2] * 2) / 10;
                p[0] = (uint8_t)(l * 0.55f); p[1] = (uint8_t)(l * 0.55f); p[2] = (uint8_t)(l * 0.6f);
            }
            out[MAT_FLOOR] = concrete(N, N, 0.22f, 407, false);
            out[MAT_CEIL] = ceiling_tiles(N);
            out[MAT_LIGHT] = light_panel(N / 4, N / 8, 0);
            out[MAT_PROP] = out[MAT_WALL];
            break;
        }
        case LevelId::L37:
            out[MAT_WALL] = pool_tiles(N, 0.97f, 0.99f, 1.0f, 0);
            out[MAT_FLOOR] = pool_tiles(N, 0.92f, 0.97f, 1.0f, 0.5f);
            out[MAT_FLOOR2] = pool_tiles(N, 0.55f, 0.80f, 0.95f, 1.0f);
            out[MAT_CEIL] = pool_tiles(N / 2, 0.98f, 0.98f, 0.98f, 0);
            out[MAT_LIGHT] = light_panel(N / 4, N / 8, 0);
            out[MAT_PROP] = out[MAT_WALL];
            break;
        case LevelId::LFun:
            out[MAT_WALL] = party_wall(N);
            out[MAT_FLOOR] = confetti_carpet(N);
            out[MAT_CEIL] = ceiling_tiles(N);
            out[MAT_LIGHT] = light_panel(N / 4, N / 8, 0.4f);
            out[MAT_PROP] = out[MAT_WALL];
            break;
        default: break;
    }
    // anything a level doesn't use still gets a valid texture
    for (int i = 0; i < MAT_COUNT; i++)
        if (out[i].w == 0) out[i] = out[MAT_FLOOR].w ? out[MAT_FLOOR] : Image(4, 4, 0xff808080u);
}

// =================================================================== sprites
Sprite item_sprite(ItemType t) {
    switch (t) {
        case ItemType::AlmondWater: return SPR_ALMOND;
        case ItemType::EnergyDrink: return SPR_ENERGY;
        case ItemType::MemoryJuice: return SPR_JUICE;
        case ItemType::RoyalRation: return SPR_ROYAL;
        case ItemType::LiquidPain: return SPR_PAIN;
        case ItemType::CashewWater: return SPR_CASHEW;
        case ItemType::LuckyMilk: return SPR_LUCKY;
        case ItemType::Firesalt: return SPR_FIRESALT;
        case ItemType::Flashlight: return SPR_FLASHLIGHT;
        case ItemType::Crowbar: return SPR_CROWBAR;
        default: return SPR_WHITE;
    }
}

Image make_sprite(Sprite s) {
    switch (s) {
        case SPR_WHITE: return Image(4, 4, 0xffffffffu);
        case SPR_FONT: {
            Image im(FONT_ATLAS_W, FONT_ATLAS_H, 0);
            im.rgba = font_atlas();
            return im;
        }
        case SPR_ALMOND: { // clear plastic bottle, white label, faintly blue water
            Image im(64, 64, 0);
            im.rect(22, 14, 42, 60, 0.75f, 0.85f, 0.95f, 0.55f);
            im.rect(26, 6, 38, 14, 0.8f, 0.88f, 0.95f, 0.6f);
            im.rect(27, 2, 37, 7, 0.2f, 0.45f, 0.9f, 1);          // blue cap
            im.rect(22, 30, 42, 46, 0.96f, 0.95f, 0.9f, 1);       // label
            im.rect(24, 35, 40, 37, 0.25f, 0.55f, 0.35f, 1);
            im.rect(24, 39, 36, 41, 0.6f, 0.45f, 0.25f, 1);
            im.rect(23, 16, 26, 58, 1, 1, 1, 0.5f);               // highlight
            return im;
        }
        case SPR_ENERGY: { // tall can, neon green on black, a lightning bolt
            Image im(64, 64, 0);
            im.rect(21, 8, 43, 60, 0.08f, 0.08f, 0.08f, 1);
            im.rect(21, 6, 43, 9, 0.7f, 0.7f, 0.7f, 1);
            im.rect(21, 58, 43, 61, 0.6f, 0.6f, 0.6f, 1);
            im.line(36, 16, 27, 34, 4, 0.3f, 1, 0.2f, 1);
            im.line(27, 34, 37, 32, 4, 0.3f, 1, 0.2f, 1);
            im.line(37, 32, 28, 52, 4, 0.3f, 1, 0.2f, 1);
            im.rect(22, 10, 25, 56, 1, 1, 1, 0.25f);
            return im;
        }
        case SPR_JUICE: { // small corked vial of swirling violet
            Image im(64, 64, 0);
            im.disc(32, 42, 15, 0.45f, 0.1f, 0.65f, 0.95f);
            im.rect(27, 12, 37, 32, 0.5f, 0.2f, 0.7f, 0.9f);
            im.rect(26, 6, 38, 13, 0.55f, 0.38f, 0.22f, 1);        // cork
            for (int i = 0; i < 30; i++) {
                float a = i * 0.45f;
                im.disc(32 + std::cos(a) * (i * 0.4f), 42 + std::sin(a) * (i * 0.4f), 1.6f, 0.95f, 0.6f, 1.0f, 0.8f);
            }
            im.disc(26, 37, 3, 1, 1, 1, 0.5f);
            return im;
        }
        case SPR_EXIT_SIGN: {
            Image im(128, 64, 0xff102010u);
            im.rect(0, 0, 128, 4, 0.2f, 0.25f, 0.2f);
            im.rect(0, 60, 128, 64, 0.2f, 0.25f, 0.2f);
            im.text(17, 14, 4, "EXIT", 0.35f, 1.0f, 0.45f);
            return im;
        }
        case SPR_SMILER: { // narrow glowing eyes and a grin far too wide, full of teeth
            Image im(128, 128, 0);
            im.disc(64, 64, 62, 0, 0, 0, 0.8f, 30);
            for (int e = -1; e <= 1; e += 2) {
                // slanted almond eyes with a soft glow
                for (int k = -12; k <= 12; k++) {
                    float x = 64 + e * 30.0f + k, y = 40 + e * k * 0.18f - std::fabs((float)k) * 0.05f;
                    float hh = 5.0f * (1.0f - (k * k) / 160.0f);
                    if (hh <= 0) continue;
                    im.disc(x, y, hh + 5, 1, 1, 0.9f, 0.08f, 4);
                    im.line(x, y - hh, x, y + hh, 1.4f, 1, 1, 0.97f, 1);
                }
            }
            // the grin: a wide crescent from cheek to cheek
            for (int i = 0; i <= 100; i++) {
                float t = (i / 100.0f) * 2 - 1;
                float x = 64 + t * 56;
                float top = 74 + (1 - t * t) * 8 - std::fabs(t) * 6, bot = 74 + (1 - t * t) * 30 - std::fabs(t) * 8;
                if (bot < top) continue;
                im.line(x, top, x, bot, 1.4f, 1, 1, 0.95f, 1);
                im.disc(x, (top + bot) / 2, (bot - top) / 2 + 6, 1, 1, 0.9f, 0.04f, 5);
            }
            // jagged teeth: dark gaps between them
            for (int i = 0; i < 24; i++) {
                float t = (i / 23.0f) * 2 - 1;
                float x = 64 + t * 52;
                float mid = 74 + (1 - t * t) * 19 - std::fabs(t) * 7;
                im.line(x - 2, mid - 2, x + 1, mid + 3, 1.2f, 0.05f, 0.03f, 0.03f, 0.95f);
                im.line(x + 2.2f, mid + 4, x + 3.2f, mid + 9, 1.0f, 0.05f, 0.03f, 0.03f, 0.9f);
            }
            im.line(14, 76, 114, 76, 1.0f, 0.06f, 0.03f, 0.03f, 0.7f); // the line between the jaws
            return im;
        }
        case SPR_PARTY_FACE: { // =)
            Image im(64, 64, 0xffe8c43au);
            im.text(9, 20, 4, "=)", 0.05f, 0.05f, 0.05f);
            return im;
        }
        case SPR_SKIN: {
            Image im(64, 64);
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    float n = vnoise(x / 8.0f, y / 8.0f, 8, 8, 291);
                    float l = 0.8f + (n - 0.5f) * 0.3f + grain(x, y, 292) * 0.08f;
                    im.set(x, y, 0.85f * l, 0.62f * l, 0.55f * l);
                }
            for (int i = 0; i < 6; i++) im.line(10.0f + i * 9, 5, 14.0f + i * 7, 60, 1, 0.5f, 0.2f, 0.2f, 0.5f); // stitches
            return im;
        }
        case SPR_SHADOW: { // a tall silhouette with soft edges
            Image im(64, 128, 0);
            im.disc(32, 22, 11, 0, 0, 0, 0.95f, 4);
            for (int y = 32; y < 124; y++) {
                float t = (y - 32) / 92.0f;
                float half = 15 - t * 5 + std::sin(t * 9) * 1.5f;
                for (int x = 0; x < 64; x++) {
                    float d = std::fabs(x - 32.0f) - half;
                    float a = clampf(0.5f - d / 4.0f, 0, 1) * (1.0f - std::max(0.0f, t - 0.85f) * 5);
                    im.blend(x, y, 0, 0, 0, a * 0.95f);
                }
            }
            im.disc(27, 21, 1.6f, 0.9f, 0.9f, 0.9f, 0.6f);
            im.disc(37, 21, 1.6f, 0.9f, 0.9f, 0.9f, 0.6f);
            return im;
        }
        case SPR_SPIDER: {
            Image im(64, 64, 0);
            for (int k = 0; k < 4; k++)
                for (int side = -1; side <= 1; side += 2) {
                    float a0 = (k - 1.5f) * 0.45f;
                    float kx = 32 + side * 14 * std::cos(a0), ky = 32 + 14 * std::sin(a0) - 5;
                    im.line(32, 32, kx, ky, 1.8f, 0.05f, 0.04f, 0.03f, 1);
                    im.line(kx, ky, kx + side * 9, ky + 12, 1.8f, 0.05f, 0.04f, 0.03f, 1);
                }
            im.disc(32, 37, 9, 0.06f, 0.05f, 0.04f, 1, 1.2f);
            im.disc(32, 25, 6, 0.06f, 0.05f, 0.04f, 1, 1.2f);
            return im;
        }
        case SPR_VIGNETTE: {
            Image im(128, 128, 0);
            for (int y = 0; y < 128; y++)
                for (int x = 0; x < 128; x++) {
                    float dx = (x - 63.5f) / 64, dy = (y - 63.5f) / 64;
                    float d = std::sqrt(dx * dx + dy * dy);
                    im.set(x, y, 0, 0, 0, clampf((d - 0.45f) / 0.65f, 0, 1));
                }
            return im;
        }
        case SPR_NOISE: {
            Image im(128, 128, 0);
            for (int y = 0; y < 128; y++)
                for (int x = 0; x < 128; x++) {
                    float n = hash01(301, x, y, 1);
                    im.set(x, y, n, n, n, 0.5f + n * 0.5f);
                }
            return im;
        }
        case SPR_GLOW: {
            Image im(64, 64, 0);
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    float d = std::sqrt((x - 31.5f) * (x - 31.5f) + (y - 31.5f) * (y - 31.5f)) / 32;
                    im.set(x, y, 1, 1, 1, clampf(1 - d, 0, 1) * clampf(1 - d, 0, 1));
                }
            return im;
        }
        case SPR_ROYAL: { // a ration tin with a crown on the label
            Image im(64, 64, 0);
            im.rect(14, 22, 50, 54, 0.62f, 0.62f, 0.6f, 1);
            im.rect(14, 22, 50, 26, 0.8f, 0.8f, 0.78f, 1);
            im.rect(16, 30, 48, 46, 0.55f, 0.12f, 0.15f, 1);
            im.line(24, 42, 26, 34, 2, 0.95f, 0.8f, 0.2f, 1); im.line(26, 34, 32, 40, 2, 0.95f, 0.8f, 0.2f, 1);
            im.line(32, 40, 38, 34, 2, 0.95f, 0.8f, 0.2f, 1); im.line(38, 34, 40, 42, 2, 0.95f, 0.8f, 0.2f, 1);
            im.rect(24, 42, 40, 44, 0.95f, 0.8f, 0.2f, 1);
            return im;
        }
        case SPR_PAIN: { // a syringe-like vial of red
            Image im(64, 64, 0);
            im.rect(27, 10, 37, 54, 0.9f, 0.9f, 0.9f, 0.6f);
            im.rect(28, 20, 36, 53, 0.85f, 0.05f, 0.08f, 1);
            im.rect(25, 6, 39, 11, 0.2f, 0.2f, 0.2f, 1);
            for (int i = 0; i < 4; i++) im.rect(37, 24 + i * 7, 40, 25 + i * 7, 0.2f, 0.2f, 0.2f, 1);
            im.rect(29, 21, 31, 50, 1, 0.6f, 0.6f, 0.5f);
            return im;
        }
        case SPR_CASHEW: { // a milky bottle, cashew on the label
            Image im(64, 64, 0);
            im.rect(22, 16, 42, 60, 0.92f, 0.88f, 0.78f, 0.9f);
            im.rect(27, 8, 37, 16, 0.92f, 0.88f, 0.78f, 0.9f);
            im.rect(26, 4, 38, 9, 0.55f, 0.35f, 0.15f, 1);
            im.rect(22, 30, 42, 46, 0.35f, 0.55f, 0.3f, 1);
            im.disc(32, 38, 5, 0.85f, 0.7f, 0.45f, 1);
            return im;
        }
        case SPR_LUCKY: { // a milk carton with a clover
            Image im(64, 64, 0);
            im.rect(18, 20, 46, 60, 0.95f, 0.95f, 0.92f, 1);
            im.line(18, 20, 32, 8, 3, 0.95f, 0.95f, 0.92f, 1); im.line(46, 20, 32, 8, 3, 0.95f, 0.95f, 0.92f, 1);
            im.rect(18, 20, 46, 28, 0.2f, 0.65f, 0.3f, 1);
            for (int k = 0; k < 4; k++) im.disc(32 + (k % 2 ? 4.0f : -4.0f), 42 + (k / 2 ? 4.0f : -4.0f), 4.5f, 0.15f, 0.7f, 0.25f, 1);
            im.line(32, 46, 34, 54, 1.5f, 0.15f, 0.6f, 0.25f, 1);
            return im;
        }
        case SPR_FIRESALT: { // a little pouch of glowing orange crystals
            Image im(64, 64, 0);
            im.disc(32, 40, 17, 0.55f, 0.4f, 0.25f, 1);
            im.rect(26, 18, 38, 26, 0.5f, 0.35f, 0.2f, 1);
            Rng rng(77);
            for (int i = 0; i < 14; i++) im.disc(rng.range(20, 44), rng.range(30, 52), rng.range(1.5f, 3.5f), 1, 0.55f + rng.f01() * 0.3f, 0.1f, 1);
            im.disc(32, 40, 22, 1, 0.5f, 0.1f, 0.15f, 8);
            return im;
        }
        case SPR_FLASHLIGHT: {
            Image im(64, 64, 0);
            im.rect(14, 26, 46, 38, 0.18f, 0.18f, 0.2f, 1);
            im.rect(44, 22, 54, 42, 0.3f, 0.3f, 0.32f, 1);
            im.rect(52, 24, 56, 40, 0.95f, 0.95f, 0.8f, 1);
            im.rect(22, 26, 28, 28, 0.8f, 0.2f, 0.2f, 1);
            for (int i = 0; i < 5; i++) im.rect(16 + i * 5, 36, 18 + i * 5, 38, 0.1f, 0.1f, 0.1f, 1);
            return im;
        }
        case SPR_CROWBAR: {
            Image im(64, 64, 0);
            im.line(16, 54, 46, 12, 5, 0.6f, 0.1f, 0.1f, 1);
            im.line(46, 12, 52, 10, 4, 0.6f, 0.1f, 0.1f, 1);
            im.line(52, 10, 54, 16, 3, 0.6f, 0.1f, 0.1f, 1);
            im.line(16, 54, 12, 56, 4, 0.6f, 0.1f, 0.1f, 1);
            im.line(18, 52, 44, 14, 1.5f, 1, 0.5f, 0.5f, 0.4f);
            return im;
        }
        case SPR_BEAM: { // flashlight cookie: bright hot centre, soft edge, a ring
            Image im(128, 128, 0);
            for (int y = 0; y < 128; y++)
                for (int x = 0; x < 128; x++) {
                    float d = std::sqrt((x - 63.5f) * (x - 63.5f) + (y - 63.5f) * (y - 63.5f)) / 64;
                    float k = clampf(1.0f - d, 0, 1);
                    float ring = std::exp(-std::pow((d - 0.55f) / 0.05f, 2.0f)) * 0.15f;
                    float v = std::pow(k, 0.8f) * 0.9f + ring;
                    im.set(x, y, v, v * 0.97f, v * 0.88f, 1);
                }
            return im;
        }
        case SPR_FLAME: {
            Image im(64, 64, 0);
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    float dx = (x - 31.5f) / 14, dy = (y - 40) / 22.0f;
                    float d = dx * dx + (dy < 0 ? dy * dy * 0.35f : dy * dy);
                    if (d < 1) im.set(x, y, 1, 0.75f - d * 0.4f, 0.3f - d * 0.3f, clampf((1 - d) * 2, 0, 1));
                }
            return im;
        }
        case SPR_BALLOON: {
            Image im(64, 64, 0);
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    float dx = (x - 31.5f) / 24, dy = (y - 28) / 27.0f;
                    float d = dx * dx + dy * dy;
                    if (d < 1) {
                        float hl = std::max(0.0f, 1 - ((x - 22) * (x - 22) + (y - 16) * (y - 16)) / 70.0f);
                        im.set(x, y, 0.8f + hl * 0.2f, 0.8f + hl * 0.2f, 0.8f + hl * 0.2f, clampf((1 - d) * 8, 0, 1));
                    }
                }
            im.rect(30, 55, 34, 59, 0.8f, 0.8f, 0.8f); // the knot
            return im;
        }
        default: return Image(4, 4, 0xffff00ffu);
    }
}

} // namespace iris
