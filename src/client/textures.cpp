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

// Worn guest-room carpet: muddy olive with a faded trellis and old stains.
Image guest_carpet(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int T = N / 8;
        float px = (float)(x % T) / T, py = (float)(y % T) / T;
        float tre = std::min(std::fabs(px - py), std::fabs(px + py - 1.0f));
        float pat = tre < 0.05f ? 0.08f : 0.0f;
        float stain = std::max(0.0f, fbm(u, v, 3, 3, 4, 1171) - 0.62f) * 1.4f;
        float l = 0.86f + pat + grain(x, y, 1172) * 0.16f - stain;
        r = 0.42f * l; g = 0.38f * l; b = 0.22f * l;
    });
}

// Furniture atlas, four quadrants (u, v from the top left):
//   [0.0-0.5, 0.0-0.5] dark wood           [0.5-1.0, 0.0-0.5] bed sheets
//   [0.0-0.5, 0.5-1.0] red velvet          [0.5-1.0, 0.5-0.75] drawer fronts, [0.75-1.0] painting
Image furniture_atlas(int N) {
    const int Q = N / 2;
    Image im(N, N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float u = (float)(x % Q) / Q, v = (float)(y % Q) / Q;
            int qx = x / Q, qy = y / Q;
            float r, g, b;
            if (qx == 0 && qy == 0) { // wood
                float ring = vnoise(u * 3, v * 40, 3, 40, 1181);
                float gr = std::sin((u * 18 + ring * 3.0f) * 2 * PI) * 0.04f + (ring - 0.5f) * 0.10f;
                float l = 0.30f + gr + grain(x, y, 1182) * 0.03f;
                r = l * 0.95f; g = l * 0.58f; b = l * 0.34f;
            } else if (qx == 1 && qy == 0) { // sheets: off-white, rumpled, a few stains
                float fold = std::sin((u * 3 + vnoise(u * 4, v * 4, 4, 4, 1183) * 1.5f) * 2 * PI) * 0.05f;
                float stain = std::max(0.0f, fbm(u, v, 3, 3, 4, 1184) - 0.66f) * 1.2f;
                float l = 0.80f + fold + grain(x, y, 1185) * 0.03f;
                r = l * 0.97f - stain * 0.2f; g = l * 0.93f - stain * 0.3f; b = l * 0.82f - stain * 0.5f;
            } else if (qx == 0 && qy == 1) { // velvet: deep red with a soft diamond tuft pattern
                float px = std::fmod(u * 6, 1.0f) - 0.5f, py = std::fmod(v * 6, 1.0f) - 0.5f;
                float tuft = std::fabs(px) + std::fabs(py) < 0.08f ? -0.12f : 0.0f;
                float sheen = (vnoise(u * 8, v * 8, 8, 8, 1186) - 0.5f) * 0.12f;
                float l = 0.9f + tuft + sheen + grain(x, y, 1187) * 0.05f;
                r = 0.42f * l; g = 0.08f * l; b = 0.09f * l;
            } else if (v < 0.5f) { // drawer fronts: two rows of drawers, brass pulls
                float dv = v * 2.0f;
                float gap = (std::fabs(dv - 0.5f) < 0.025f || dv < 0.03f || dv > 0.97f || u < 0.02f || u > 0.98f) ? -0.14f : 0.0f;
                float ring = vnoise(u * 30, dv * 3, 30, 3, 1188);
                float l = 0.32f + (ring - 0.5f) * 0.08f + gap + grain(x, y, 1189) * 0.03f;
                r = l * 0.95f; g = l * 0.58f; b = l * 0.34f;
                float kx = std::fabs(u - 0.5f), ky = std::min(std::fabs(dv - 0.25f), std::fabs(dv - 0.75f));
                if (kx < 0.08f && ky < 0.035f) { r = 0.75f; g = 0.6f; b = 0.3f; }
            } else { // a dark, muddy painting of a hallway in a gold frame
                float pv = (v - 0.5f) * 2.0f;
                bool frame = u < 0.07f || u > 0.93f || pv < 0.1f || pv > 0.9f;
                if (frame) {
                    float l = 0.55f + (vnoise(u * 20, pv * 20, 20, 20, 1190) - 0.5f) * 0.3f;
                    r = l; g = l * 0.78f; b = l * 0.35f;
                } else {
                    float cx = std::fabs(u - 0.5f), horizon = std::fabs(pv - 0.55f);
                    float hall = (cx < 0.08f + horizon * 0.5f) ? 0.10f : 0.0f; // a corridor vanishing into the dark
                    float l = 0.12f + hall + (fbm(u, pv, 4, 4, 3, 1191) - 0.5f) * 0.1f;
                    r = l * 1.2f; g = l * 1.0f; b = l * 0.6f;
                }
            }
            im.set(x, y, r, g, b);
        }
    return im;
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

// ================================================================== 0.5 levels
void desaturate(Image& im, float amount, float bright) {
    for (size_t i = 0; i + 3 < im.rgba.size(); i += 4) {
        uint8_t* p = &im.rgba[i];
        float l = (p[0] * 0.3f + p[1] * 0.59f + p[2] * 0.11f);
        for (int k = 0; k < 3; k++) p[k] = (uint8_t)clampf((p[k] + (l - p[k]) * amount) * bright, 0, 255);
    }
}

// Level 7: dark sea water, ripples
Image sea_water(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float w = fbm(u, v, 4, 4, 4, 701), rip = std::sin((u * 8 + w * 2.0f) * 2 * PI) * std::sin((v * 8 - w * 1.5f) * 2 * PI);
        float spark = std::pow(std::max(0.0f, rip), 6.0f) * 0.25f;
        float l = 0.26f + (w - 0.5f) * 0.12f + spark * 1.6f + grain(x, y, 702) * 0.01f;
        r = l * 0.40f; g = l * 0.80f; b = l * 1.10f;
    });
}

// planks, horizontal (floors, attic walls); tone = base brightness, red/green/blue tint
Image planks(int N, float tone, float rr, float gg, float bb, uint32_t seed) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int P = N / 8;
        int row = y / P;
        int off = (int)(hash01(seed, row, 0, 1) * N);
        int seam_x = ((x + off) % (N / 2));
        float edge = (y % P < 2) ? -0.2f : 0.0f;
        float butt = seam_x < 2 ? -0.18f : 0.0f;
        float gr = (vnoise(u * 3, v * 64 + row * 3.0f, 3, 64, seed + (uint32_t)row) - 0.5f) * 0.2f;
        float knot = vnoise(u * 12, v * 12, 12, 12, seed + 7) > 0.83f ? -0.12f : 0.0f;
        float l = tone + gr + edge + butt + knot + (hash01(seed, row, (x + off) / (N / 2), 2) - 0.5f) * 0.12f + grain(x, y, seed) * 0.03f;
        r = l * rr; g = l * gg; b = l * bb;
    });
}

// Level 21: four numbered doors side by side on an institutional wall
Image numbered_doors(int N) {
    const int W = N, H = N / 4, D = W / 4;
    Image im = paint(W, H, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float l = 0.80f + (fbm(u, v, 8, 2, 3, 2101) - 0.5f) * 0.06f + grain(x, y, 2102) * 0.02f;
        if (v > 0.9f) l = 0.42f;                // dark skirting
        if (v > 0.55f && v < 0.57f) l -= 0.18f; // dado rail
        r = l * 0.92f; g = l * 0.97f; b = l * 0.88f;
    });
    static const char* NUM[4] = {"214", "215", "216", "217"};
    for (int k = 0; k < 4; k++) {
        int x0 = k * D + D * 30 / 100, x1 = k * D + D * 70 / 100, y0 = H * 18 / 100;
        im.rect(x0 - 3, y0 - 3, x1 + 3, H - H / 10, 0.35f, 0.33f, 0.3f);
        for (int y = y0; y < H - H / 10; y++)
            for (int x = x0; x < x1; x++) {
                float gr = (vnoise((float)x / D * 4, (float)y / H * 30, 16, 30, 2103 + (uint32_t)k) - 0.5f) * 0.08f;
                float l = 0.45f + gr;
                im.set(x, y, l * 0.62f, l * 0.75f, l * 0.68f); // painted green door
            }
        im.disc(x1 - (x1 - x0) * 0.15f, y0 + (H - y0) * 0.5f, std::max(1.5f, D * 0.02f), 0.75f, 0.72f, 0.6f);
        int sc = std::max(1, D / 90);
        im.rect((x0 + x1) / 2 - 10 * sc, y0 + 6 * sc, (x0 + x1) / 2 + 10 * sc, y0 + 16 * sc, 0.9f, 0.88f, 0.8f);
        im.text((x0 + x1) / 2 - 8 * sc, y0 + 7 * sc, sc, NUM[k], 0.1f, 0.1f, 0.1f);
    }
    return im;
}

// ceiling tiles with air vents
Image vent_ceiling(int N) {
    Image im = ceiling_tiles(N);
    int T = N / 4;
    for (int ty = 0; ty < 4; ty++)
        for (int tx = 0; tx < 4; tx++) {
            if (((tx + ty) & 1) || hash01(2111, tx, ty, 1) < 0.5f) continue;
            int x0 = tx * T + T / 5, y0 = ty * T + T / 5, x1 = tx * T + T * 4 / 5, y1 = ty * T + T * 4 / 5;
            im.rect(x0, y0, x1, y1, 0.55f, 0.55f, 0.52f);
            for (int y = y0 + 3; y < y1 - 3; y += 4) im.rect(x0 + 3, y, x1 - 3, y + 2, 0.12f, 0.12f, 0.12f);
        }
    return im;
}

// Level 22: car park floor; bays get painted lines
Image park_floor(int N, bool bay) {
    Image im = concrete(N, N, 0.40f, bay ? 2201 : 2202, false);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float u = (float)x / N, v = (float)y / N;
            float oil = std::max(0.0f, fbm(u, v, 3, 3, 4, 2203) - 0.62f) * 0.8f;
            uint8_t* p = &im.rgba[((size_t)y * N + x) * 4];
            for (int k = 0; k < 3; k++) p[k] = (uint8_t)(p[k] * (1.0f - oil));
            if (bay && (x < N / 64 + 1 || x > N - N / 64 - 2)) { p[0] = 210; p[1] = 200; p[2] = 150; } // bay lines
        }
    return im;
}
Image car_side(int N) { // white body (tinted per car), dark windows, wheels
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float l = 0.85f + (vnoise(u * 2, v * 2, 2, 2, 2211) - 0.5f) * 0.08f + grain(x, y, 2212) * 0.02f;
        if (v < 0.3f) { l = 0.18f + u * 0.1f; }                 // windows
        else if (v < 0.34f) l = 0.5f;                            // trim
        if (v > 0.72f && (std::fabs(u - 0.2f) < 0.12f || std::fabs(u - 0.8f) < 0.12f)) { // wheels
            float dx = std::min(std::fabs(u - 0.2f), std::fabs(u - 0.8f)), dy = v - 0.86f;
            if (dx * dx + dy * dy < 0.016f) { r = g = b = (dx * dx + dy * dy < 0.004f ? 0.5f : 0.08f); return; }
        }
        if (v > 0.93f) l = 0.15f;
        r = g = b = l;
    });
}
Image car_roof(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float l = 0.8f + grain(x, y, 2221) * 0.02f;
        if ((u > 0.12f && u < 0.32f) || (u > 0.7f && u < 0.86f)) l = 0.2f; // windscreens
        r = g = b = l;
        (void)v;
    });
}

// Level 24: moon dust
Image regolith(int N, float base, uint32_t seed) {
    Image im = paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float n = fbm(u, v, 6, 6, 5, seed);
        float l = base + (n - 0.5f) * 0.2f + grain(x, y, seed) * 0.08f;
        r = g = l; b = l * 1.02f;
    });
    Rng rng(seed);
    for (int i = 0; i < 18; i++) { // little craters: dark floor, bright rim
        float cx = rng.range(0, (float)N), cy = rng.range(0, (float)N), rad = rng.range(N * 0.02f, N * 0.07f);
        im.disc(cx, cy, rad * 1.15f, base + 0.15f, base + 0.15f, base + 0.16f, 0.35f, 2);
        im.disc(cx, cy, rad, base - 0.12f, base - 0.12f, base - 0.11f, 0.6f, 2);
    }
    return im;
}

// Level 33: mall floor tiles, shop fronts
Image mall_tiles(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int T = N / 4;
        bool dark = ((x / T) + (y / T)) & 1;
        float seam = (x % T < 2 || y % T < 2) ? -0.12f : 0.0f;
        float shine = std::pow(fbm(u, v, 2, 2, 3, 3301), 3.0f) * 0.25f;
        float l = (dark ? 0.32f : 0.82f) + seam + shine + grain(x, y, 3302) * 0.02f;
        r = l * (dark ? 0.9f : 1.0f); g = l * (dark ? 0.75f : 0.97f); b = l * (dark ? 1.0f : 0.95f);
    });
}
Image storefront(int N) {
    const int W = N / 2, H = N;
    Image im = paint(W, H, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        if (v < 0.22f) { // the sign band
            float l = 0.25f + grain(x, y, 3311) * 0.03f;
            r = l; g = l * 0.9f; b = l * 1.1f;
            return;
        }
        if (v > 0.93f || u < 0.05f || u > 0.95f) { r = 0.75f; g = 0.75f; b = 0.78f; return; } // frame
        // glass: a dark shop behind it, a reflection streak, a pulled-down shutter on some
        float refl = (u + v * 0.5f > 0.55f && u + v * 0.5f < 0.62f) ? 0.15f : 0.0f;
        float shutter = (v < 0.55f && ((int)(v * H) % 6 < 2)) ? 0.1f : 0.0f;
        float l = 0.08f + refl + shutter + (fbm(u, v, 3, 6, 3, 3312) - 0.5f) * 0.05f;
        r = l * 0.9f; g = l; b = l * 1.15f;
    });
    // neon: a pink glowing stripe and a word
    im.rect(W * 10 / 100, H * 7 / 100, W * 90 / 100, H * 15 / 100, 1.0f, 0.4f, 0.75f, 0.9f);
    int sc = std::max(1, W / 110);
    im.text(W / 2 - 15 * sc, H * 8 / 100, sc, "OPEN", 0.15f, 0.05f, 0.1f);
    return im;
}
Image planter_top(int N) {
    Image im = paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float l = 0.18f + grain(x, y, 3321) * 0.05f; r = l * 1.1f; g = l * 0.8f; b = l * 0.5f; (void)u; (void)v;
    });
    Rng rng(3322);
    for (int i = 0; i < 60; i++) {
        float cx = rng.range(0, (float)N), cy = rng.range(0, (float)N);
        im.disc(cx, cy, rng.range(N * 0.03f, N * 0.08f), 0.15f + rng.f01() * 0.1f, 0.4f + rng.f01() * 0.2f, 0.12f, 0.9f, 2);
    }
    im.rect(0, 0, N, N / 16, 0.6f, 0.6f, 0.6f);
    im.rect(0, N - N / 16, N, N, 0.6f, 0.6f, 0.6f);
    return im;
}

// Level 36: airport
Image window_wall(int N) {
    const int W = N / 2, H = N;
    return paint(W, H, [&](int x, int y, float, float v, float& r, float& g, float& b) {
        bool mullion = (x % (W / 2) < W / 40) || v < 0.04f || v > 0.9f;
        if (mullion) { float l = 0.55f; r = g = l; b = l * 1.05f; return; }
        // the night outside: a dark blue sky, a strip of runway lights at the horizon
        float sky = 0.05f + (1.0f - v) * 0.08f;
        r = sky * 0.5f; g = sky * 0.7f; b = sky * 1.4f;
        if (std::fabs(v - 0.62f) < 0.006f && (x % 24) < 3) { r = 1.0f; g = 0.8f; b = 0.4f; }
        if (v > 0.62f) { r = 0.03f; g = 0.035f; b = 0.04f; }
        (void)y;
    });
}
Image seat_side(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int seat = x / (N / 4);
        float gap = (x % (N / 4) < N / 64) ? -0.2f : 0.0f;
        float l = 0.35f + gap + grain(x, y, 3601) * 0.04f;
        if (v > 0.7f) { l = 0.55f; r = g = b = l; return; } // chrome legs
        r = l * 0.35f; g = l * 0.45f; b = l * 1.0f;
        (void)u; (void)seat;
    });
}
Image terrazzo(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float chip = hash01(3611, x / 3, y / 3, 1);
        float l = 0.72f + (chip > 0.92f ? -0.25f : (chip < 0.05f ? 0.12f : 0.0f)) + std::pow(fbm(u, v, 2, 2, 3, 3612), 3.0f) * 0.2f;
        r = l; g = l * 0.98f; b = l * 0.96f;
    });
}

// Level 51: ancient carved stone
Image carved_stone(int N, uint32_t seed) {
    Image im = paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int bh = N / 6, row = y / bh;
        int off = (row & 1) ? N / 4 : 0;
        bool joint = (y % bh < 2) || ((x + off) % (N / 2) < 2);
        float wear = fbm(u, v, 4, 4, 4, seed) - 0.5f;
        float l = 0.55f + wear * 0.25f + grain(x, y, seed) * 0.05f + (joint ? -0.22f : 0.0f);
        r = l * 0.95f; g = l * 0.85f; b = l * 0.68f;
    });
    // a band of glyphs no one can read
    int y0 = N / 6 * 2 + N / 40, y1 = N / 6 * 3 - N / 40;
    Rng rng(seed + 1);
    for (int gx = 0; gx < N; gx += N / 16) {
        int k = (int)(rng.f01() * 4);
        float cx = gx + N / 32.0f, cy = (y0 + y1) / 2.0f, s2 = N / 48.0f;
        if (k == 0) im.disc(cx, cy, s2, 0.3f, 0.26f, 0.2f, 0.9f, 1);
        if (k == 1) im.line(cx - s2, cy - s2, cx + s2, cy + s2, 1.5f, 0.3f, 0.26f, 0.2f);
        if (k == 2) { im.line(cx - s2, cy, cx + s2, cy, 1.5f, 0.3f, 0.26f, 0.2f); im.line(cx, cy - s2, cx, cy + s2, 1.5f, 0.3f, 0.26f, 0.2f); }
        if (k == 3) im.rect((int)(cx - s2), (int)(cy - s2), (int)(cx + s2), (int)(cy + s2), 0.3f, 0.26f, 0.2f, 0.7f);
    }
    return im;
}
Image paving(int N) {
    return paint(N, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        int S = N / 3;
        float seam = (x % S < 3 || y % S < 3) ? -0.18f : 0.0f;
        float slab = (hash01(5111, x / S, y / S, 1) - 0.5f) * 0.12f;
        float l = 0.42f + seam + slab + (fbm(u, v, 4, 4, 4, 5112) - 0.5f) * 0.12f + grain(x, y, 5113) * 0.05f;
        r = l * 0.95f; g = l * 0.86f; b = l * 0.7f;
    });
}

// Level !: concrete panels with hazard stripes
Image hazard_wall(int N) {
    return paint(N / 2, N, [&](int x, int y, float u, float v, float& r, float& g, float& b) {
        float l = 0.5f + (fbm(u, v, 3, 6, 4, 9001) - 0.5f) * 0.12f + grain(x, y, 9002) * 0.03f;
        if (y % (N / 4) < 2 || x % (N / 4) < 2) l -= 0.12f;
        r = g = b = l;
        if (v > 0.82f && v < 0.92f) { // yellow/black stripes at waist height... at the bottom
            bool yb = ((x + y) / (N / 32)) & 1;
            if (yb) { r = 0.85f; g = 0.7f; b = 0.1f; } else { r = g = b = 0.08f; }
        }
    });
}

// ------------------------------------------------------------------ decals
// 4x4 atlas of small alpha-tested details. Alpha is hard-edged (alpha test, PS1 style).
Image decal_atlas(int N) {
    const int C = N / 4;
    Image im(N, N, 0);
    auto cell = [&](int k, auto fn) {
        int ox = (k % 4) * C, oy = (k / 4) * C;
        for (int y = 2; y < C - 2; y++)
            for (int x = 2; x < C - 2; x++) {
                float u = (x + 0.5f) / C, v = (y + 0.5f) / C, r = 0, g = 0, b = 0, a = 0;
                fn(x, y, u, v, r, g, b, a);
                if (a > 0.5f) im.set(ox + x, oy + y, r, g, b, 1);
            }
    };
    cell(DECAL_PAPER, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        float rx = (u - 0.5f) * 0.94f - (v - 0.5f) * 0.34f, ry = (u - 0.5f) * 0.34f + (v - 0.5f) * 0.94f; // tilted sheet
        if (std::fabs(rx) < 0.28f && std::fabs(ry) < 0.36f) {
            float l = 0.86f + grain(x, y, 31) * 0.05f;
            bool line = std::fmod(ry + 1.0f, 0.06f) < 0.008f && std::fabs(rx) < 0.22f;
            r = l - (line ? 0.3f : 0); g = l - (line ? 0.3f : 0); b = l * 0.95f - (line ? 0.25f : 0); a = 1;
        }
    });
    cell(DECAL_CRUMPLED, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) + (vnoise(u * 8, v * 8, 8, 8, 32) - 0.5f) * 0.15f;
        if (d < 0.22f) { float l = 0.75f + (vnoise(u * 16, v * 16, 16, 16, 33) - 0.5f) * 0.4f; r = l; g = l; b = l * 0.95f; a = 1; }
        (void)x; (void)y;
    });
    auto blot = [&](float u, float v, uint32_t seed, float size) {
        float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) + (fbm(u, v, 4, 4, 3, seed) - 0.5f) * 0.35f;
        return d < size;
    };
    cell(DECAL_STAIN, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        if (blot(u, v, 34, 0.32f)) { float l = 0.22f + grain(x, y, 35) * 0.03f; r = l * 1.1f; g = l * 0.9f; b = l * 0.6f; a = 1; }
    });
    cell(DECAL_STAIN2, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        if (blot(u, v, 36, 0.28f)) { float l = 0.15f + grain(x, y, 37) * 0.03f; r = l; g = l * 1.05f; b = l * 0.9f; a = 1; }
    });
    cell(DECAL_CRACK, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        float n = vnoise(u * 6, v * 6, 6, 6, 38);
        if (std::fabs(n - 0.5f) < 0.018f && std::fabs(u - 0.5f) < 0.45f && std::fabs(v - 0.5f) < 0.45f) { r = g = b = 0.06f; a = 1; }
        (void)x; (void)y;
    });
    cell(DECAL_PUDDLE, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        if (blot(u, v, 39, 0.38f)) {
            float sheen = std::pow(fbm(u, v, 3, 3, 3, 40), 2.0f) * 0.35f;
            r = 0.08f + sheen; g = 0.09f + sheen; b = 0.11f + sheen * 1.1f; a = 1;
        }
        (void)x; (void)y;
    });
    cell(DECAL_LEAVES, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        for (int k = 0; k < 6; k++) {
            float cx = 0.2f + hash01(41, k, 0, 1) * 0.6f, cy = 0.2f + hash01(41, k, 1, 1) * 0.6f;
            float dx = (u - cx) / 0.1f, dy = (v - cy) / 0.05f;
            if (dx * dx + dy * dy < 1) { float t = hash01(41, k, 2, 1); r = 0.4f + t * 0.3f; g = 0.25f + t * 0.1f; b = 0.08f; a = 1; }
        }
        (void)x; (void)y;
    });
    cell(DECAL_TRASH, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        for (int k = 0; k < 9; k++) {
            float cx = 0.15f + hash01(42, k, 0, 1) * 0.7f, cy = 0.15f + hash01(42, k, 1, 1) * 0.7f;
            if (std::fabs(u - cx) < 0.04f && std::fabs(v - cy) < 0.015f + hash01(42, k, 3, 1) * 0.02f) {
                float t = hash01(42, k, 2, 1);
                r = 0.5f + t * 0.4f; g = 0.45f + t * 0.3f; b = 0.4f; a = 1;
            }
        }
        (void)x; (void)y;
    });
    cell(DECAL_PRINTS, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        for (int k = 0; k < 4; k++) {
            float cx = k & 1 ? 0.62f : 0.38f, cy = 0.15f + k * 0.22f;
            float dx = (u - cx) / 0.06f, dy = (v - cy) / 0.1f;
            if (dx * dx + dy * dy < 1) { r = 0.12f; g = 0.1f; b = 0.08f; a = 1; }
        }
        (void)x; (void)y;
    });
    cell(DECAL_MOLD, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        if (blot(u, v, 43, 0.3f) && hash01(44, x, y, 1) < 0.8f) { float l = 0.18f + grain(x, y, 45) * 0.08f; r = l * 0.7f; g = l * 0.9f; b = l * 0.5f; a = 1; }
    });
    cell(DECAL_SCUFF, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        float n = vnoise(u * 2, v * 14, 2, 14, 46);
        if (n > 0.72f && std::fabs(u - 0.5f) < 0.45f) { r = g = b = 0.2f; a = 1; }
        (void)x; (void)y;
    });
    cell(DECAL_CAN, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        if (std::fabs(u - 0.5f) < 0.22f && std::fabs(v - 0.5f) < 0.1f) {
            bool rim = std::fabs(u - 0.5f) > 0.19f;
            r = rim ? 0.75f : 0.8f; g = rim ? 0.75f : 0.15f; b = rim ? 0.78f : 0.12f; a = 1;
        }
        (void)x; (void)y;
    });
    cell(DECAL_OUTLET, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        if (std::fabs(u - 0.5f) < 0.18f && std::fabs(v - 0.5f) < 0.26f) {
            r = 0.85f; g = 0.83f; b = 0.76f; a = 1;
            for (int k = -1; k <= 1; k += 2)
                if (std::fabs(v - 0.5f - k * 0.11f) < 0.05f && (std::fabs(u - 0.46f) < 0.012f || std::fabs(u - 0.54f) < 0.012f)) r = g = b = 0.1f;
        }
        (void)x; (void)y;
    });
    cell(DECAL_VENT, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        if (std::fabs(u - 0.5f) < 0.4f && std::fabs(v - 0.5f) < 0.28f) {
            bool slot = ((int)(v * 40) % 3 == 0) && std::fabs(u - 0.5f) < 0.35f;
            r = g = b = slot ? 0.08f : 0.62f; a = 1;
        }
        (void)x; (void)y;
    });
    cell(DECAL_STREAK, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        float w = 0.1f + vnoise(v * 4, 0, 4, 1, 47) * 0.12f;
        if (std::fabs(u - 0.5f) < w * (1.0f - v * 0.6f)) { float l = 0.3f + grain(x, y, 48) * 0.05f; r = l * 1.1f; g = l * 0.95f; b = l * 0.7f; a = 1; }
    });
    cell(DECAL_GLASS, [&](int x, int y, float u, float v, float& r, float& g, float& b, float& a) {
        for (int k = 0; k < 10; k++) {
            float cx = 0.15f + hash01(49, k, 0, 1) * 0.7f, cy = 0.15f + hash01(49, k, 1, 1) * 0.7f;
            if (std::fabs(u - cx) + std::fabs(v - cy) < 0.035f) { r = 0.7f; g = 0.85f; b = 0.9f; a = 1; }
        }
        (void)x; (void)y;
    });
    return im;
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
static float id_bright(LevelId l) { return l == LevelId::TSGBlack ? 0.45f : 0.95f; }

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
            out[MAT_FLOOR3] = guest_carpet(N);
            out[MAT_FURN] = furniture_atlas(N);
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
        case LevelId::L7:
            out[MAT_WALL] = wallpaper(N);
            desaturate(out[MAT_WALL], 0.5f, 0.6f); // water-stained, mouldy
            out[MAT_FLOOR] = planks(N, 0.42f, 0.8f, 0.62f, 0.42f, 711);
            out[MAT_FLOOR2] = sea_water(N);
            out[MAT_BLOCK] = rock(N, 0.30f, 712);
            out[MAT_ROOF] = out[MAT_BLOCK];
            out[MAT_LIGHT] = light_panel(N / 8, N / 8, 0.6f);
            out[MAT_PROP] = metal(N / 4, 0.3f);
            break;
        case LevelId::L19:
            out[MAT_FLOOR] = planks(N, 0.40f, 0.85f, 0.65f, 0.42f, 1911);
            out[MAT_BLOCK] = planks(N, 0.30f, 0.8f, 0.6f, 0.4f, 1912);
            out[MAT_ROOF] = crate(N / 2);
            out[MAT_WALL] = out[MAT_BLOCK];
            out[MAT_CEIL] = planks(N, 0.26f, 0.78f, 0.6f, 0.42f, 1913);
            out[MAT_LIGHT] = caged_bulb(N);
            out[MAT_PROP] = planks(N / 2, 0.32f, 0.8f, 0.6f, 0.4f, 1914);
            break;
        case LevelId::L21:
            out[MAT_BLOCK] = numbered_doors(N);
            out[MAT_WALL] = out[MAT_BLOCK];
            out[MAT_FLOOR] = office_carpet(N);
            desaturate(out[MAT_FLOOR], 0.6f, 0.9f);
            out[MAT_CEIL] = vent_ceiling(N);
            out[MAT_LIGHT] = light_panel(N / 4, N / 8, 0);
            out[MAT_PROP] = metal(N / 4, 0.6f);
            break;
        case LevelId::L22:
            out[MAT_FLOOR] = park_floor(N, false);
            out[MAT_FLOOR2] = park_floor(N, true);
            out[MAT_FLOOR3] = rock(N, 0.18f, 2231);
            out[MAT_WALL] = concrete(N / 2, N, 0.48f, 2232, true);
            out[MAT_CEIL] = concrete(N / 2, N / 2, 0.36f, 2233, false);
            out[MAT_BLOCK] = car_side(N / 2);
            out[MAT_ROOF] = car_roof(N / 2);
            out[MAT_LIGHT] = light_panel(N / 4, N / 16, 0.8f);
            out[MAT_PROP] = concrete(N / 2, N, 0.55f, 2234, true);
            break;
        case LevelId::L24:
            out[MAT_FLOOR] = regolith(N, 0.55f, 2411);
            out[MAT_FLOOR2] = regolith(N, 0.40f, 2412);
            out[MAT_BLOCK] = rock(N, 0.42f, 2413);
            desaturate(out[MAT_BLOCK], 1.0f, 1.0f);
            out[MAT_ROOF] = regolith(N / 2, 0.5f, 2414);
            out[MAT_PROP] = out[MAT_BLOCK];
            out[MAT_WALL] = out[MAT_BLOCK];
            break;
        case LevelId::L33:
            out[MAT_FLOOR] = mall_tiles(N);
            out[MAT_FLOOR2] = carpet(N);
            out[MAT_BLOCK] = storefront(N);
            out[MAT_ROOF] = planter_top(N / 2);
            out[MAT_CEIL] = ceiling_tiles(N);
            out[MAT_LIGHT] = light_panel(N / 4, N / 4, 0.3f);
            out[MAT_PROP] = metal(N / 4, 0.7f);
            out[MAT_WALL] = out[MAT_PROP];
            break;
        case LevelId::L36:
            out[MAT_FLOOR] = office_carpet(N);
            out[MAT_FLOOR2] = terrazzo(N);
            out[MAT_BLOCK] = window_wall(N);
            out[MAT_ROOF] = seat_side(N / 2);
            out[MAT_WALL] = concrete(N / 2, N, 0.78f, 3631, true);
            out[MAT_CEIL] = ceiling_tiles(N);
            out[MAT_LIGHT] = light_panel(N / 4, N / 8, 0);
            out[MAT_PROP] = metal(N / 4, 0.75f);
            break;
        case LevelId::L51:
            out[MAT_BLOCK] = carved_stone(N, 5121);
            out[MAT_ROOF] = paving(N / 2);
            out[MAT_FLOOR] = paving(N);
            out[MAT_FLOOR2] = rock(N, 0.24f, 5122);
            out[MAT_CEIL] = rock(N / 2, 0.18f, 5123);
            out[MAT_LIGHT] = fungus(N);
            out[MAT_PROP] = carved_stone(N / 2, 5124);
            out[MAT_WALL] = out[MAT_PROP];
            break;
        case LevelId::LRun:
            out[MAT_BLOCK] = hazard_wall(N);
            out[MAT_WALL] = out[MAT_BLOCK];
            out[MAT_FLOOR] = concrete(N, N, 0.34f, 9011, false);
            out[MAT_CEIL] = concrete(N / 2, N / 2, 0.30f, 9012, false);
            out[MAT_LIGHT] = light_panel(N / 4, N / 8, 1.0f);
            out[MAT_PROP] = metal(N / 4, 0.4f);
            break;
        case LevelId::TSG: case LevelId::TSGBlack:
            out[MAT_WALL] = wallpaper(N);
            desaturate(out[MAT_WALL], 1.0f, id_bright(level));
            out[MAT_FLOOR] = carpet(N);
            desaturate(out[MAT_FLOOR], 1.0f, id_bright(level) * 0.8f);
            out[MAT_CEIL] = ceiling_tiles(N);
            desaturate(out[MAT_CEIL], 1.0f, id_bright(level));
            out[MAT_LIGHT] = light_panel(N / 4, N / 8, 0);
            out[MAT_PROP] = out[MAT_WALL];
            break;
        case LevelId::TSGMansion:
            out[MAT_WALL] = hotel_wall(N, false);
            out[MAT_BLOCK] = hotel_wall(N, true);
            out[MAT_FLOOR] = hotel_carpet(N);
            out[MAT_FLOOR2] = parquet(N);
            out[MAT_FLOOR3] = guest_carpet(N);
            out[MAT_CEIL] = plaster(N / 2);
            out[MAT_PROP] = parquet(N / 4);
            out[MAT_FURN] = furniture_atlas(N);
            for (int i = 0; i < MAT_COUNT; i++) if (out[i].w) desaturate(out[i], 1.0f, 1.1f);
            out[MAT_LIGHT] = light_panel(N / 8, N / 8, 0);
            break;
        case LevelId::TSGHope:
            out[MAT_WALL] = concrete(N / 2, N, 0.30f, 9021, true);
            out[MAT_FLOOR] = concrete(N, N, 0.20f, 9022, false);
            out[MAT_CEIL] = concrete(N / 2, N / 2, 0.18f, 9023, false);
            out[MAT_LIGHT] = light_panel(N / 4, N / 8, 0);
            out[MAT_PROP] = out[MAT_WALL];
            break;
        default: break;
    }
    // anything a level doesn't use still gets a valid texture
    if (out[MAT_FURN].w == 0) out[MAT_FURN] = Image(4, 4, 0xff808080u); // only Level 5 has furniture
    out[MAT_DECAL] = decal_atlas(std::min(N, 512));
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
        case ItemType::SuperAlmond: return SPR_SUPER;
        case ItemType::MothJelly: return SPR_MOTH;
        case ItemType::LiquidSilence: return SPR_SILENCE;
        case ItemType::MedKit: return SPR_MEDKIT;
        case ItemType::FlashBulb: return SPR_BULB;
        case ItemType::GlowStick: return SPR_GLOWSTICK;
        case ItemType::MusicBox: return SPR_MUSICBOX;
        case ItemType::RabbitFoot: return SPR_RABBIT;
        case ItemType::Compass: return SPR_COMPASS;
        case ItemType::Radio: return SPR_RADIO;
        case ItemType::Pistol: return SPR_PISTOL;
        case ItemType::Shotgun: return SPR_SHOTGUN;
        case ItemType::Rifle: return SPR_RIFLE;
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
        case SPR_SUPER: { // a tall bottle, glowing faintly gold
            Image im(64, 64, 0);
            im.disc(32, 34, 22, 1, 0.85f, 0.4f, 0.25f, 10);
            im.rect(24, 22, 40, 56, 0.85f, 0.9f, 0.95f, 0.9f);
            im.rect(24, 34, 40, 50, 1.0f, 0.85f, 0.45f, 1);
            im.rect(28, 12, 36, 22, 0.85f, 0.9f, 0.95f, 0.9f);
            im.rect(27, 8, 37, 13, 0.9f, 0.75f, 0.2f, 1);
            im.text(27, 38, 1, "S", 0.4f, 0.25f, 0.05f);
            return im;
        }
        case SPR_MOTH: { // a jar of pale jelly with a moth on the label
            Image im(64, 64, 0);
            im.rect(18, 22, 46, 56, 0.85f, 0.88f, 0.75f, 0.95f);
            im.rect(20, 30, 44, 54, 0.95f, 0.9f, 0.6f, 1);
            im.rect(16, 16, 48, 23, 0.55f, 0.45f, 0.35f, 1);
            im.disc(26, 41, 5, 0.45f, 0.38f, 0.3f, 1, 1);
            im.disc(38, 41, 5, 0.45f, 0.38f, 0.3f, 1, 1);
            im.rect(31, 36, 33, 48, 0.2f, 0.15f, 0.1f, 1);
            return im;
        }
        case SPR_SILENCE: { // a black bottle with a white X
            Image im(64, 64, 0);
            im.rect(22, 20, 42, 56, 0.08f, 0.08f, 0.1f, 1);
            im.rect(27, 10, 37, 20, 0.08f, 0.08f, 0.1f, 1);
            im.line(26, 30, 38, 46, 2, 0.9f, 0.9f, 0.9f);
            im.line(38, 30, 26, 46, 2, 0.9f, 0.9f, 0.9f);
            return im;
        }
        case SPR_MEDKIT: {
            Image im(64, 64, 0);
            im.rect(10, 18, 54, 50, 0.9f, 0.9f, 0.88f, 1);
            im.rect(26, 13, 38, 18, 0.5f, 0.5f, 0.5f, 1);
            im.rect(28, 24, 36, 44, 0.85f, 0.1f, 0.1f, 1);
            im.rect(22, 30, 42, 38, 0.85f, 0.1f, 0.1f, 1);
            return im;
        }
        case SPR_BULB: { // an old camera flash bulb
            Image im(64, 64, 0);
            im.disc(32, 26, 15, 0.9f, 0.95f, 1.0f, 0.8f, 2);
            im.disc(28, 22, 5, 1, 1, 1, 0.9f, 2);
            im.rect(25, 38, 39, 52, 0.6f, 0.6f, 0.62f, 1);
            for (int y = 40; y < 52; y += 3) im.rect(25, y, 39, y + 1, 0.35f, 0.35f, 0.38f, 1);
            return im;
        }
        case SPR_GLOWSTICK: {
            Image im(64, 64, 0);
            im.line(18, 50, 46, 14, 9, 0.3f, 1, 0.4f, 0.25f);
            im.line(18, 50, 46, 14, 5, 0.5f, 1, 0.55f, 1);
            im.line(20, 47, 44, 17, 1.5f, 0.9f, 1, 0.9f, 1);
            return im;
        }
        case SPR_MUSICBOX: {
            Image im(64, 64, 0);
            im.rect(12, 30, 52, 54, 0.5f, 0.28f, 0.15f, 1);
            im.rect(12, 26, 52, 31, 0.65f, 0.4f, 0.22f, 1);
            im.rect(14, 33, 50, 35, 0.8f, 0.65f, 0.3f, 1);
            im.line(52, 40, 58, 40, 2, 0.8f, 0.7f, 0.3f);
            im.line(58, 36, 58, 44, 2, 0.8f, 0.7f, 0.3f);
            im.disc(26, 16, 3, 0.9f, 0.9f, 0.9f, 1, 1);
            im.line(28.5f, 16, 28.5f, 6, 1.5f, 0.9f, 0.9f, 0.9f);
            im.disc(38, 12, 3, 0.9f, 0.9f, 0.9f, 1, 1);
            im.line(40.5f, 12, 40.5f, 3, 1.5f, 0.9f, 0.9f, 0.9f);
            return im;
        }
        case SPR_RABBIT: { // a rabbit's foot on a little chain
            Image im(64, 64, 0);
            im.disc(34, 40, 12, 0.9f, 0.88f, 0.82f, 1, 2);
            im.disc(30, 50, 8, 0.88f, 0.86f, 0.8f, 1, 2);
            im.rect(32, 18, 36, 28, 0.75f, 0.65f, 0.3f, 1);
            im.disc(34, 14, 5, 0.75f, 0.65f, 0.3f, 1, 1);
            im.disc(34, 14, 2.5f, 0, 0, 0, 1, 1);
            return im;
        }
        case SPR_COMPASS: {
            Image im(64, 64, 0);
            im.disc(32, 32, 22, 0.75f, 0.6f, 0.25f, 1, 1);
            im.disc(32, 32, 18, 0.92f, 0.9f, 0.82f, 1, 1);
            im.line(32, 32, 32, 16, 3, 0.85f, 0.15f, 0.1f);
            im.line(32, 32, 32, 48, 3, 0.25f, 0.25f, 0.3f);
            im.disc(32, 32, 2.5f, 0.2f, 0.2f, 0.2f, 1, 1);
            return im;
        }
        case SPR_RADIO: { // a walkie-talkie
            Image im(64, 64, 0);
            im.rect(20, 18, 44, 56, 0.2f, 0.22f, 0.2f, 1);
            im.rect(36, 4, 40, 18, 0.15f, 0.15f, 0.15f, 1);
            im.rect(24, 22, 40, 32, 0.4f, 0.7f, 0.45f, 1);
            for (int y = 36; y < 52; y += 4) im.rect(24, y, 40, y + 2, 0.1f, 0.1f, 0.1f, 1);
            im.disc(26, 14, 2.5f, 0.8f, 0.1f, 0.1f, 1, 1);
            return im;
        }
        case SPR_PISTOL: {
            Image im(64, 64, 0);
            im.rect(10, 22, 54, 32, 0.18f, 0.18f, 0.2f, 1);    // slide
            im.rect(10, 22, 54, 24, 0.35f, 0.35f, 0.38f, 1);
            im.rect(36, 30, 50, 54, 0.14f, 0.12f, 0.12f, 1);   // grip
            im.line(30, 32, 36, 40, 2, 0.15f, 0.15f, 0.17f);   // trigger guard
            im.rect(10, 20, 13, 23, 0.6f, 0.6f, 0.6f, 1);      // sight
            return im;
        }
        case SPR_SHOTGUN: {
            Image im(64, 64, 0);
            im.rect(2, 24, 50, 28, 0.2f, 0.2f, 0.22f, 1);     // barrel
            im.rect(14, 28, 36, 33, 0.45f, 0.28f, 0.15f, 1);  // pump
            im.rect(36, 24, 52, 34, 0.18f, 0.18f, 0.2f, 1);   // receiver
            im.line(50, 32, 62, 44, 7, 0.45f, 0.28f, 0.15f);   // stock
            return im;
        }
        case SPR_RIFLE: {
            Image im(64, 64, 0);
            im.rect(2, 24, 40, 28, 0.2f, 0.2f, 0.22f, 1);
            im.rect(20, 22, 50, 32, 0.25f, 0.27f, 0.22f, 1);
            im.rect(30, 32, 36, 46, 0.18f, 0.18f, 0.18f, 1);  // magazine, slightly angled
            im.rect(24, 18, 40, 22, 0.15f, 0.15f, 0.15f, 1);  // scope rail
            im.rect(48, 26, 62, 36, 0.25f, 0.27f, 0.22f, 1);  // stock
            im.rect(40, 32, 46, 44, 0.15f, 0.15f, 0.15f, 1);  // grip
            return im;
        }
        case SPR_CONE: {
            Image im(64, 128, 0);
            for (int y = 0; y < 128; y++)
                for (int x = 0; x < 64; x++) {
                    float v = y / 127.0f;                       // 0 top (the lamp) .. 1 floor
                    float half = 0.18f + 0.32f * v;             // widens toward the floor
                    float u = std::fabs(x - 31.5f) / 32.0f;
                    float side = clampf(1.0f - u / (half * 2.0f), 0, 1);
                    float a = side * side * std::pow(1.0f - v, 1.6f) * (0.6f + 0.4f * clampf(v * 8, 0, 1));
                    im.set(x, y, 1, 1, 1, a);
                }
            return im;
        }
        case SPR_PLANET: {
            Image im(128, 128, 0);
            for (int y = 0; y < 128; y++)
                for (int x = 0; x < 128; x++) {
                    float dx = (x - 63.5f) / 62, dy = (y - 63.5f) / 62;
                    float d2 = dx * dx + dy * dy;
                    if (d2 > 1) continue;
                    float nz = std::sqrt(1 - d2);
                    float lit = clampf(dx * -0.55f + dy * -0.35f + nz * 0.75f, 0, 1); // lit from the upper left
                    float band = 0.85f + 0.15f * std::sin(dy * 9 + vnoise(dx * 3, dy * 3, 4, 4, 77) * 3);
                    float l = (0.08f + lit * 0.92f) * band;
                    im.set(x, y, l, l, l, clampf((1 - d2) * 30, 0, 1));
                }
            return im;
        }
        case SPR_STAR: {
            Image im(16, 16, 0);
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    float d = std::sqrt((x - 7.5f) * (x - 7.5f) + (y - 7.5f) * (y - 7.5f)) / 8;
                    float a = clampf(1 - d, 0, 1);
                    im.set(x, y, 1, 1, 1, a * a * a);
                }
            return im;
        }
        case SPR_SCANLINES: {
            Image im(4, 4, 0);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) im.set(x, y, 0, 0, 0, y < 2 ? 0.0f : 0.35f);
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
