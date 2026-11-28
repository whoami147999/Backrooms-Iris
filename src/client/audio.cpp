#include "audio.h"

#include <SDL.h>

#include "../platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace iris {
namespace {

const float TAU = 6.2831853f;
const int R = SYNTH_RATE;

struct Rand {
    uint32_t s;
    explicit Rand(uint32_t seed) : s(seed ? seed : 1) {}
    float f() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFF) / 16777216.0f * 2.0f - 1.0f; } // -1..1
};

// one-pole filters
struct LP { float y = 0, a; explicit LP(float hz) : a(1.0f - std::exp(-TAU * hz / R)) {} float operator()(float x) { y += a * (x - y); return y; } };
struct HP { LP lp; explicit HP(float hz) : lp(hz) {} float operator()(float x) { return x - lp(x); } };
// a resonant band-pass (state variable filter)
struct BP {
    float low = 0, band = 0, f, q;
    BP(float hz, float res) : f(2.0f * std::sin(3.14159f * std::min(hz, R * 0.2f) / R)), q(1.0f / res) {}
    void tune(float hz) { f = 2.0f * std::sin(3.14159f * std::min(hz, R * 0.2f) / R); }
    float operator()(float x) { low += f * band; float high = x - low - q * band; band += f * high; return band; }
};

std::vector<float> make(float secs) { return std::vector<float>((size_t)(secs * R), 0.0f); }
float env(float t, float attack, float decay) { return (t < attack ? t / attack : std::exp(-(t - attack) / decay)); }
void normalize(std::vector<float>& b, float peak) {
    float m = 1e-6f;
    for (float x : b) m = std::max(m, std::fabs(x));
    for (float& x : b) x = x / m * peak;
}
// make a loop seamless: crossfade the tail into the head
void seamless(std::vector<float>& b, float fade_secs) {
    size_t n = (size_t)(fade_secs * R);
    if (b.size() < n * 2) return;
    size_t L = b.size() - n;
    for (size_t i = 0; i < n; i++) {
        float t = (float)i / n;
        b[i] = b[i] * t + b[L + i] * (1 - t);
    }
    b.resize(L);
}

std::vector<float> step(int kind, uint32_t seed) {
    // kind: 0 carpet, 1 hard, 2 water, 3 metal, 4 wood
    auto b = make(kind == 2 ? 0.32f : 0.22f);
    Rand rnd(seed);
    LP lp(kind == 0 ? 700.0f : kind == 2 ? 1200.0f : 3000.0f);
    HP hp(kind == 0 ? 60.0f : 150.0f);
    BP knock(kind == 4 ? 520.0f : 260.0f, 3.0f);
    for (size_t i = 0; i < b.size(); i++) {
        float t = (float)i / R;
        float n = rnd.f();
        float e = env(t, 0.002f, kind == 0 ? 0.03f : 0.022f);
        float x = hp(lp(n)) * e * (kind == 0 ? 0.8f : 1.0f);
        x += std::sin(TAU * (kind == 0 ? 70.0f : 110.0f) * t) * env(t, 0.001f, 0.025f) * 0.7f; // the heel thump
        if (kind == 4) x += knock(n) * env(t, 0.001f, 0.05f) * 1.5f;
        if (kind == 3) x += (std::sin(TAU * 930 * t) + 0.6f * std::sin(TAU * 1410 * t)) * env(t, 0.001f, 0.09f) * 0.35f; // grate rings
        if (kind == 2) { // splash: noise and a few bubbles
            x = lp(n) * env(t, 0.01f, 0.08f);
            for (int k = 0; k < 3; k++) {
                float t0 = 0.03f + k * 0.06f + std::fabs(rnd.f()) * 0.02f;
                if (t > t0 && t < t0 + 0.05f) x += std::sin(TAU * (400 + k * 150 + (t - t0) * 6000) * (t - t0)) * env(t - t0, 0.003f, 0.015f) * 0.4f;
            }
        }
        b[i] = x;
    }
    normalize(b, 0.6f);
    return b;
}

std::vector<float> gunshot(float len, float boom_hz, float bright, uint32_t seed, bool pump) {
    auto b = make(len + (pump ? 0.5f : 0.0f));
    Rand rnd(seed);
    LP body(bright), tail(500);
    for (size_t i = 0; i < b.size(); i++) {
        float t = (float)i / R;
        float n = rnd.f();
        float crack = n * env(t, 0.0005f, 0.006f) * 1.5f;
        float blast = body(n) * env(t, 0.001f, len * 0.18f) * 1.4f;
        float rumble = tail(n) * env(t, 0.01f, len * 0.5f) * 0.9f;
        float boom = std::sin(TAU * boom_hz * t * (1.0f - t * 0.3f)) * env(t, 0.001f, len * 0.25f) * 1.2f;
        float x = std::tanh((crack + blast + rumble + boom) * 1.5f);
        if (pump) { // chk-chk
            for (int k = 0; k < 2; k++) {
                float t0 = len + 0.12f + k * 0.15f;
                if (t > t0) x += n * env(t - t0, 0.001f, 0.01f) * 0.5f + std::sin(TAU * 1800 * (t - t0)) * env(t - t0, 0.001f, 0.02f) * 0.3f;
            }
        }
        b[i] = x;
    }
    normalize(b, 0.95f);
    return b;
}

std::vector<float> voice_sweep(float len, float f0, float f1, float formant, float rough, uint32_t seed, float vib = 5) {
    auto b = make(len);
    Rand rnd(seed);
    BP f1b(formant, 4), f2b(formant * 2.3f, 5);
    float ph = 0;
    for (size_t i = 0; i < b.size(); i++) {
        float t = (float)i / R, u = t / len;
        float f = f0 + (f1 - f0) * u + std::sin(TAU * vib * t) * f0 * 0.03f;
        ph += f / R;
        float saw = 2.0f * (ph - std::floor(ph)) - 1.0f;
        float src = saw + rnd.f() * rough;
        float x = f1b(src) * 0.8f + f2b(src) * 0.4f;
        b[i] = x * std::sin(3.14159f * std::min(1.0f, u * 1.2f)) * (0.6f + 0.4f * std::sin(TAU * 3.0f * t));
    }
    normalize(b, 0.7f);
    return b;
}

} // namespace

void synth_all(std::vector<float> out[SFX_COUNT]) {
    out[SFX_STEP_CARPET] = step(0, 11);
    out[SFX_STEP_HARD] = step(1, 12);
    out[SFX_STEP_WATER] = step(2, 13);
    out[SFX_STEP_METAL] = step(3, 14);
    out[SFX_STEP_WOOD] = step(4, 15);
    { // pickup: two soft rising notes
        auto b = make(0.22f);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            float f = t < 0.08f ? 660 : 990;
            float tt = t < 0.08f ? t : t - 0.08f;
            b[i] = (std::sin(TAU * f * t) + 0.3f * std::sin(TAU * f * 2 * t)) * env(tt, 0.004f, 0.06f);
        }
        normalize(b, 0.45f);
        out[SFX_PICKUP] = b;
    }
    { // drink: three gulps
        auto b = make(0.9f);
        Rand rnd(21);
        LP lp(900);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            float x = 0;
            for (int k = 0; k < 3; k++) {
                float t0 = 0.05f + k * 0.27f;
                if (t > t0) x += std::sin(TAU * (190 - (t - t0) * 300) * (t - t0)) * env(t - t0, 0.01f, 0.05f);
            }
            x += lp(rnd.f()) * 0.3f * (0.5f + 0.5f * std::sin(TAU * 3.7f * t));
            b[i] = x;
        }
        normalize(b, 0.55f);
        out[SFX_DRINK] = b;
    }
    { // eat: crunches
        auto b = make(0.7f);
        Rand rnd(22);
        BP bp(1800, 2);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R, x = 0;
            for (int k = 0; k < 4; k++) { float t0 = k * 0.16f; if (t > t0) x += bp(rnd.f()) * env(t - t0, 0.002f, 0.03f); }
            b[i] = x;
        }
        normalize(b, 0.5f);
        out[SFX_EAT] = b;
    }
    { // click: flashlight switch
        auto b = make(0.06f);
        Rand rnd(23);
        HP hp(2000);
        for (size_t i = 0; i < b.size(); i++) { float t = (float)i / R; b[i] = hp(rnd.f()) * env(t, 0.0005f, 0.004f) + std::sin(TAU * 2600 * t) * env(t, 0.0005f, 0.008f) * 0.5f; }
        normalize(b, 0.5f);
        out[SFX_CLICK] = b;
    }
    { // UI: a soft tick and a two-note select
        auto a = make(0.05f), c = make(0.18f);
        for (size_t i = 0; i < a.size(); i++) { float t = (float)i / R; a[i] = std::sin(TAU * 1300 * t) * env(t, 0.001f, 0.01f); }
        for (size_t i = 0; i < c.size(); i++) { float t = (float)i / R; float f = t < 0.06f ? 520 : 780; c[i] = std::sin(TAU * f * t) * env(t < 0.06f ? t : t - 0.06f, 0.002f, 0.04f); }
        normalize(a, 0.3f);
        normalize(c, 0.35f);
        out[SFX_UI_MOVE] = a;
        out[SFX_UI_SELECT] = c;
    }
    out[SFX_PISTOL] = gunshot(0.35f, 85, 3500, 31, false);
    out[SFX_SHOTGUN] = gunshot(0.7f, 55, 2200, 32, true);
    out[SFX_RIFLE] = gunshot(0.22f, 95, 4500, 33, false);
    { // swing: a whoosh
        auto b = make(0.3f);
        Rand rnd(34);
        BP bp(500, 3);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            bp.tune(400 + t / 0.3f * 1400);
            b[i] = bp(rnd.f()) * std::sin(3.14159f * t / 0.3f);
        }
        normalize(b, 0.5f);
        out[SFX_SWING] = b;
    }
    { // hurt: a dull thud and a ring in the ears
        auto b = make(0.6f);
        Rand rnd(35);
        LP lp(300);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            b[i] = lp(rnd.f()) * env(t, 0.002f, 0.06f) * 1.5f + std::sin(TAU * 60 * t) * env(t, 0.002f, 0.08f) + std::sin(TAU * 3100 * t) * env(t, 0.05f, 0.25f) * 0.08f;
        }
        normalize(b, 0.7f);
        out[SFX_HURT] = b;
    }
    { // death: a falling drone swelling into noise
        auto b = make(2.6f);
        Rand rnd(36);
        LP lp(600);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R, u = t / 2.6f;
            float f = 110 * (1 - u * 0.6f);
            b[i] = (std::sin(TAU * f * t) + std::sin(TAU * f * 1.5f * t) * 0.5f + lp(rnd.f()) * u * 1.5f) * std::sin(3.14159f * std::min(1.0f, u * 1.3f));
        }
        normalize(b, 0.7f);
        out[SFX_DEATH] = b;
    }
    { // exit: the push bar clacks, the door groans open, air rushes past
        auto b = make(1.6f);
        Rand rnd(37);
        LP lp(1200);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R, x = 0;
            x += rnd.f() * env(t, 0.0005f, 0.01f) + std::sin(TAU * 1500 * t) * env(t, 0.0005f, 0.03f) * 0.5f;
            if (t > 0.1f) { float tt = t - 0.1f; x += std::sin(TAU * (260 - tt * 60 + std::sin(TAU * 7 * tt) * 15) * tt) * env(tt, 0.08f, 0.35f) * 0.5f; }
            x += lp(rnd.f()) * std::sin(3.14159f * std::min(1.0f, t / 1.6f)) * 0.6f;
            b[i] = x;
        }
        normalize(b, 0.7f);
        out[SFX_EXIT] = b;
    }
    { // breaker: a heavy clunk, then a hum comes up
        auto b = make(1.2f);
        Rand rnd(38);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            b[i] = rnd.f() * env(t, 0.001f, 0.02f) + std::sin(TAU * 90 * t) * env(t, 0.001f, 0.06f) +
                   (std::sin(TAU * 120 * t) + 0.5f * std::sin(TAU * 240 * t)) * 0.25f * std::min(1.0f, t * 3) * std::exp(-t * 1.5f);
        }
        normalize(b, 0.65f);
        out[SFX_BREAKER] = b;
    }
    { // tape: two plastic clacks and a whir
        auto b = make(0.6f);
        Rand rnd(39);
        BP bp(2500, 3);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R, x = 0;
            for (int k = 0; k < 2; k++) { float t0 = k * 0.09f; if (t > t0) x += bp(rnd.f()) * env(t - t0, 0.001f, 0.012f); }
            if (t > 0.2f) x += std::sin(TAU * (300 + 40 * std::sin(TAU * 30 * t)) * t) * 0.2f * env(t - 0.2f, 0.05f, 0.15f);
            b[i] = x;
        }
        normalize(b, 0.5f);
        out[SFX_TAPE] = b;
    }
    { // objective: three bell notes going up
        auto b = make(1.5f);
        const float F[3] = {523.25f, 659.25f, 783.99f};
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R, x = 0;
            for (int k = 0; k < 3; k++) {
                float t0 = k * 0.15f;
                if (t > t0) { float tt = t - t0; x += (std::sin(TAU * F[k] * tt) + 0.4f * std::sin(TAU * F[k] * 2.01f * tt) + 0.15f * std::sin(TAU * F[k] * 3.03f * tt)) * env(tt, 0.003f, 0.5f); }
            }
            b[i] = x;
        }
        normalize(b, 0.5f);
        out[SFX_OBJECTIVE] = b;
    }
    { // flash bulb: a pop and a dying whine
        auto b = make(1.0f);
        Rand rnd(40);
        for (size_t i = 0; i < b.size(); i++) { float t = (float)i / R; b[i] = rnd.f() * env(t, 0.0005f, 0.015f) * 1.2f + std::sin(TAU * (4200 - t * 1500) * t) * env(t, 0.01f, 0.35f) * 0.15f; }
        normalize(b, 0.7f);
        out[SFX_FLASH] = b;
    }
    { // throw: a short whoosh
        auto b = make(0.2f);
        Rand rnd(41);
        BP bp(900, 2);
        for (size_t i = 0; i < b.size(); i++) { float t = (float)i / R; b[i] = bp(rnd.f()) * std::sin(3.14159f * t / 0.2f); }
        normalize(b, 0.35f);
        out[SFX_THROW] = b;
    }
    { // bacteria: wet clicking
        auto b = make(0.9f);
        Rand rnd(42);
        BP bp(3200, 6);
        float next = 0;
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            if (t > next) next = t + 0.025f + std::fabs(rnd.f()) * 0.06f;
            float tt = t - (next - 0.03f);
            b[i] = bp(rnd.f()) * (tt > 0 && tt < 0.008f ? 1.0f : 0.05f);
        }
        normalize(b, 0.6f);
        out[SFX_BACTERIA_CLICK] = b;
    }
    out[SFX_BACTERIA_SCREECH] = voice_sweep(1.1f, 900, 1700, 2400, 0.6f, 43, 11);
    { // hound: a low wet growl
        auto b = voice_sweep(1.4f, 75, 62, 350, 0.9f, 44, 18);
        for (size_t i = 0; i < b.size(); i++) b[i] *= 0.7f + 0.3f * std::sin(TAU * 21 * (float)i / R);
        normalize(b, 0.7f);
        out[SFX_HOUND_GROWL] = b;
    }
    { // smiler: breathy whispering
        auto b = make(2.0f);
        Rand rnd(45);
        BP a(900, 6), c(2300, 7);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            a.tune(700 + 500 * std::sin(TAU * 2.3f * t));
            c.tune(2000 + 800 * std::sin(TAU * 3.1f * t + 1));
            float n = rnd.f();
            b[i] = (a(n) + c(n) * 0.6f) * (0.5f + 0.5f * std::sin(TAU * 4.5f * t)) * std::sin(3.14159f * t / 2.0f);
        }
        normalize(b, 0.55f);
        out[SFX_SMILER_WHISPER] = b;
    }
    out[SFX_SMILER_CHARGE] = voice_sweep(0.9f, 300, 1400, 1800, 0.8f, 46, 9);
    { // partygoer: a breathy "hee hee hee" that's somehow wrong
        auto b = make(1.3f);
        for (int k = 0; k < 3; k++) {
            auto v = voice_sweep(0.22f, 640 - k * 30, 520 - k * 30, 2700, 0.5f, 47 + k, 7);
            size_t o = (size_t)((0.08f + k * 0.32f) * R);
            for (size_t i = 0; i < v.size() && o + i < b.size(); i++) b[o + i] += v[i];
        }
        normalize(b, 0.55f);
        out[SFX_PARTY_GIGGLE] = b;
    }
    out[SFX_SKIN_GROAN] = voice_sweep(1.8f, 120, 95, 500, 0.4f, 50, 4);
    { // sting: a hit of dissonance when something sees you
        auto b = make(2.0f);
        Rand rnd(51);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            b[i] = (std::sin(TAU * 220 * t) + std::sin(TAU * 233 * t) + std::sin(TAU * 311 * t) * 0.6f + rnd.f() * 0.3f) * env(t, 0.005f, 0.6f);
        }
        normalize(b, 0.6f);
        out[SFX_STING] = b;
    }
    // ----- loops (all exactly periodic or crossfaded, so they don't click)
    { // fluorescent hum: 120 Hz with harmonics and a little buzz
        auto b = make(2.0f);
        Rand rnd(61);
        HP hp(2000);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            float buzz = std::sin(TAU * 120 * t) + 0.5f * std::sin(TAU * 240 * t) + 0.35f * std::sin(TAU * 360 * t) + 0.2f * std::sin(TAU * 600 * t);
            b[i] = buzz * (0.85f + 0.15f * std::sin(TAU * 0.5f * t)) + hp(rnd.f()) * 0.08f;
        }
        normalize(b, 0.5f);
        out[SFX_HUM] = b;
    }
    { // drone: a dark cluster beating slowly
        auto b = make(8.0f);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            b[i] = std::sin(TAU * 41.0f * t) + 0.8f * std::sin(TAU * 41.5f * t) + 0.5f * std::sin(TAU * 61.5f * t) + 0.3f * std::sin(TAU * 82.25f * t) * (0.5f + 0.5f * std::sin(TAU * 0.125f * t));
        }
        normalize(b, 0.5f);
        out[SFX_DRONE] = b;
    }
    { // wind
        auto b = make(6.5f);
        Rand rnd(62);
        BP bp(400, 1.5f);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            bp.tune(300 + 250 * std::sin(TAU * 0.17f * t) + 120 * std::sin(TAU * 0.41f * t));
            b[i] = bp(rnd.f()) * (0.6f + 0.4f * std::sin(TAU * 0.23f * t));
        }
        seamless(b, 0.5f);
        normalize(b, 0.5f);
        out[SFX_WIND] = b;
    }
    { // water: lapping and dripping
        auto b = make(5.5f);
        Rand rnd(63);
        LP lp(500);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            float x = lp(rnd.f()) * (0.5f + 0.5f * std::sin(TAU * 0.6f * t) * std::sin(TAU * 0.27f * t));
            for (int k = 0; k < 6; k++) {
                float t0 = 0.4f + k * 0.8f + (k % 3) * 0.13f;
                if (t > t0 && t < t0 + 0.06f) x += std::sin(TAU * (1200 + k * 200 - (t - t0) * 8000) * (t - t0)) * env(t - t0, 0.001f, 0.015f) * 0.5f;
            }
            b[i] = x;
        }
        seamless(b, 0.5f);
        normalize(b, 0.5f);
        out[SFX_WATER] = b;
    }
    { // alarm: a two-tone siren
        auto b = make(1.0f);
        float ph = 0;
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            float f = t < 0.5f ? 700 : 940;
            ph += f / R;
            float sq = std::sin(TAU * ph) > 0 ? 1.0f : -1.0f;
            b[i] = sq * 0.5f + std::sin(TAU * ph) * 0.5f;
        }
        normalize(b, 0.45f);
        out[SFX_ALARM] = b;
    }
    { // fire: crackling
        auto b = make(4.0f);
        Rand rnd(64);
        LP lp(800);
        for (size_t i = 0; i < b.size(); i++) {
            float n = rnd.f();
            float pop = std::fabs(rnd.f()) > 0.9993f ? 4.0f : 0.0f;
            b[i] = lp(n) * 0.6f + pop * n;
        }
        LP smooth(5000);
        for (float& x : b) x = smooth(x);
        seamless(b, 0.3f);
        normalize(b, 0.5f);
        out[SFX_FIRE] = b;
    }
    { // radio static
        auto b = make(3.0f);
        Rand rnd(65);
        BP bp(1800, 0.8f);
        for (size_t i = 0; i < b.size(); i++) { float t = (float)i / R; b[i] = bp(rnd.f()) * (0.7f + 0.3f * std::sin(TAU * 13 * t)); }
        seamless(b, 0.2f);
        normalize(b, 0.4f);
        out[SFX_STATIC] = b;
    }
    { // heartbeat: lub-dub, 75 bpm
        auto b = make(0.8f);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            b[i] = std::sin(TAU * 55 * t) * env(t, 0.005f, 0.05f) + (t > 0.22f ? std::sin(TAU * 48 * (t - 0.22f)) * env(t - 0.22f, 0.005f, 0.06f) * 0.7f : 0.0f);
        }
        normalize(b, 0.8f);
        out[SFX_HEART] = b;
    }
    { // music box: a little tune in a minor key, on a loop
        const float notes[16] = {523.25f, 622.25f, 783.99f, 622.25f, 523.25f, 466.16f, 392.0f, 466.16f,
                                 523.25f, 622.25f, 698.46f, 783.99f, 698.46f, 622.25f, 587.33f, 523.25f};
        const float beat = 0.3f;
        auto b = make(16 * beat);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R, x = 0;
            for (int k = 0; k < 16; k++) {
                float t0 = k * beat;
                if (t >= t0 && t < t0 + 1.5f) { float tt = t - t0; x += (std::sin(TAU * notes[k] * tt) + 0.3f * std::sin(TAU * notes[k] * 2.0f * tt) + 0.12f * std::sin(TAU * notes[k] * 5.2f * tt)) * env(tt, 0.002f, 0.35f); }
                float tw = t + 16 * beat; // the tail of the last notes rings into the start
                if (tw >= t0 && tw < t0 + 1.5f && k > 12) { float tt = tw - t0; x += std::sin(TAU * notes[k] * tt) * env(tt, 0.002f, 0.35f) * 0.6f; }
            }
            b[i] = x;
        }
        normalize(b, 0.5f);
        out[SFX_MUSICBOX] = b;
    }
    { // whispers: memory juice
        auto b = make(5.0f);
        Rand rnd(66);
        BP a(1100, 8), c(2600, 9);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            a.tune(800 + 600 * std::sin(TAU * 1.7f * t));
            c.tune(2200 + 900 * std::sin(TAU * 2.9f * t + 2));
            float n = rnd.f();
            float gate = std::sin(TAU * 0.9f * t) * std::sin(TAU * 2.3f * t) > 0.1f ? 1.0f : 0.15f;
            b[i] = (a(n) + c(n) * 0.5f) * gate;
        }
        seamless(b, 0.4f);
        normalize(b, 0.45f);
        out[SFX_WHISPERS] = b;
    }

    // ----- 1.3: doors, drawers, paper
    { // a door: the latch clicks, then the hinges creak as it swings
        auto b = make(1.1f);
        Rand rnd(1301);
        BP creak(700, 14);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            float latch = t < 0.03f ? rnd.f() * env(t, 0.001f, 0.008f) : 0.0f;
            creak.tune(500 + 380 * std::sin(TAU * 1.3f * t) + 120 * std::sin(TAU * 7.0f * t));
            float scrape = t > 0.08f ? creak(rnd.f()) * env(t - 0.08f, 0.15f, 0.35f) * 2.5f : 0.0f;
            float grain = (std::sin(TAU * 31.0f * t) > 0.6f ? 1.0f : 0.4f); // the creak stutters
            b[i] = latch * 0.8f + scrape * grain;
        }
        normalize(b, 0.55f);
        out[SFX_DOOR_OPEN] = b;
    }
    { // a door shutting: a wooden thud and the latch catching
        auto b = make(0.6f);
        Rand rnd(1302);
        LP lp(300);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            float thud = lp(rnd.f()) * env(t, 0.002f, 0.08f) * 3.0f + std::sin(TAU * 85 * t) * env(t, 0.002f, 0.1f);
            float latch = t > 0.06f && t < 0.09f ? rnd.f() * env(t - 0.06f, 0.001f, 0.01f) : 0.0f;
            b[i] = thud + latch * 0.6f;
        }
        normalize(b, 0.7f);
        out[SFX_DOOR_CLOSE] = b;
    }
    { // a jammed door: the handle rattles, the door thumps against its frame
        auto b = make(0.7f);
        Rand rnd(1303);
        BP r1(2400, 6);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            float x = 0;
            for (int k = 0; k < 3; k++) { float tt = t - k * 0.18f; if (tt > 0) x += r1(rnd.f()) * env(tt, 0.002f, 0.04f) * 2.0f + std::sin(TAU * 70 * tt) * env(tt, 0.002f, 0.05f) * 0.6f; }
            b[i] = x;
        }
        normalize(b, 0.6f);
        out[SFX_DOOR_JAM] = b;
    }
    { // a drawer sliding out on dry runners
        auto b = make(0.55f);
        Rand rnd(1304);
        BP slide(900, 3);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            slide.tune(700 + 500 * t);
            b[i] = slide(rnd.f()) * env(t, 0.05f, 0.25f) * 2.0f + (t > 0.42f ? std::sin(TAU * 140 * (t - 0.42f)) * env(t - 0.42f, 0.002f, 0.04f) : 0.0f);
        }
        normalize(b, 0.5f);
        out[SFX_DRAWER] = b;
    }
    { // paper: picked up, unfolded
        auto b = make(0.5f);
        Rand rnd(1305);
        HP hp(2500);
        for (size_t i = 0; i < b.size(); i++) {
            float t = (float)i / R;
            float crinkle = (rnd.f() > 0.85f ? 1.0f : 0.15f);
            b[i] = hp(rnd.f()) * crinkle * env(t, 0.02f, 0.18f);
        }
        normalize(b, 0.45f);
        out[SFX_PAPER] = b;
    }

    // ----- 1.2: music. All of it is generated here too: a slow, sad theme for the menus, something
    // calmer and emptier for the safe places, and a pulse that won't let up for Level !.
    {
        auto note_hz = [](int midi) { return 440.0f * std::pow(2.0f, (midi - 69) / 12.0f); };
        auto piano = [&](float hz, float t) { // a decaying tone with a few overtones: soft, glassy
            if (t < 0 || t > 4.0f) return 0.0f;
            float e = env(t, 0.004f, 1.1f);
            return (std::sin(TAU * hz * t) + 0.35f * std::sin(TAU * hz * 2.0f * t) * std::exp(-t * 2.0f) +
                    0.12f * std::sin(TAU * hz * 3.01f * t) * std::exp(-t * 4.0f)) * e;
        };
        auto reverb = [&](std::vector<float>& b, float mix) { // three feedback combs, a cheap hall
            const int D[3] = {(int)(0.0297f * R), (int)(0.0371f * R), (int)(0.0411f * R)};
            std::vector<float> wet(b.size(), 0.0f);
            for (int k = 0; k < 3; k++) {
                std::vector<float> line((size_t)D[k], 0.0f);
                size_t p = 0;
                LP damp(2500.0f);
                for (size_t i = 0; i < b.size(); i++) {
                    float y = line[p];
                    line[p] = b[i] + damp(y) * 0.84f;
                    p = (p + 1) % line.size();
                    wet[i] += y / 3.0f;
                }
            }
            for (size_t i = 0; i < b.size(); i++) b[i] = b[i] * (1 - mix) + wet[i] * mix;
        };
        { // the menu: A minor, Am - F - C - G, a slow arpeggio over a pad
            const float bpm = 66.0f, beat = 60.0f / bpm;
            const int chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
            const int bars = 8;
            auto b = make(bars * 4 * beat + 3.0f);
            for (size_t i = 0; i < b.size(); i++) {
                float t = (float)i / R;
                int bar = std::min(bars - 1, (int)(t / (4 * beat)));
                const int* ch = chords[bar % 4];
                float x = 0;
                // pad: three detuned tones per chord note, swelling with each bar
                float bt = t - bar * 4 * beat;
                float sw = std::min(1.0f, bt / 1.2f) * (bar == bars - 1 ? std::max(0.0f, 1.0f - std::max(0.0f, bt - 3 * beat) / beat) : 1.0f);
                for (int k = 0; k < 3; k++) {
                    float hz = note_hz(ch[k] - 12);
                    x += (std::sin(TAU * hz * t) + std::sin(TAU * hz * 1.003f * t) * 0.7f) * 0.06f * sw;
                }
                b[i] = x;
            }
            // the arpeggio: up through the chord, an octave up, every half beat
            for (int bar = 0; bar < bars; bar++)
                for (int k = 0; k < 8; k++) {
                    const int* ch = chords[bar % 4];
                    int m = ch[k % 3] + (k >= 3 ? 12 : 0) + (k == 7 ? 7 : 0);
                    float t0 = (bar * 4 + k * 0.5f) * beat;
                    float hz = note_hz(m);
                    size_t s0 = (size_t)(t0 * R);
                    float vel = 0.22f * (k == 0 ? 1.0f : 0.75f);
                    for (size_t i = s0; i < std::min(b.size(), s0 + (size_t)(3.5f * R)); i++) b[i] += piano(hz, (float)(i - s0) / R) * vel;
                }
            reverb(b, 0.35f);
            seamless(b, 2.5f);
            normalize(b, 0.55f);
            out[SFX_MUSIC_MENU] = b;
        }
        { // the safe places: sparse high notes, long gaps, a low hum under them
            auto b = make(40.0f);
            Rand rnd(4242);
            const int scale[6] = {69, 72, 74, 76, 79, 81};
            for (size_t i = 0; i < b.size(); i++) { float t = (float)i / R; b[i] = std::sin(TAU * 55.0f * t) * 0.03f * (0.6f + 0.4f * std::sin(TAU * 0.05f * t)); }
            float t0 = 0.5f;
            while (t0 < 36.0f) {
                int m = scale[(int)((rnd.f() * 0.5f + 0.5f) * 5.99f)];
                size_t s0 = (size_t)(t0 * R);
                for (size_t i = s0; i < std::min(b.size(), s0 + (size_t)(3.5f * R)); i++) b[i] += piano(note_hz(m), (float)(i - s0) / R) * 0.16f;
                t0 += 1.6f + (rnd.f() * 0.5f + 0.5f) * 2.6f;
            }
            reverb(b, 0.5f);
            seamless(b, 3.0f);
            normalize(b, 0.4f);
            out[SFX_MUSIC_CALM] = b;
        }
        { // Level !: 140 bpm, a pulsing bass, a kick, a nervous figure on top
            const float beat = 60.0f / 140.0f;
            const int bars = 8;
            auto b = make(bars * 4 * beat + 1.0f);
            const int roots[4] = {40, 40, 43, 38};
            LP bass_lp(420.0f);
            for (size_t i = 0; i < b.size(); i++) {
                float t = (float)i / R;
                int bar = std::min(bars - 1, (int)(t / (4 * beat)));
                float e8 = std::fmod(t, beat * 0.5f);
                float hz = note_hz(roots[bar % 4]);
                float saw = 2.0f * std::fmod(hz * t, 1.0f) - 1.0f;
                float x = bass_lp(saw) * 0.35f * env(e8, 0.003f, 0.12f);
                float kb = std::fmod(t, beat);
                x += std::sin(TAU * (50.0f + 90.0f * std::exp(-kb * 30.0f)) * kb) * env(kb, 0.001f, 0.12f) * 0.6f;
                float e16 = std::fmod(t, beat * 0.25f);
                int step16 = (int)(t / (beat * 0.25f)) % 16;
                static const int FIG[16] = {12, 0, 15, 0, 12, 19, 0, 15, 12, 0, 15, 17, 0, 15, 12, 10};
                if (FIG[step16]) x += std::sin(TAU * note_hz(roots[bar % 4] + 24 + FIG[step16]) * e16) * env(e16, 0.002f, 0.05f) * 0.12f;
                b[i] = x;
            }
            seamless(b, 0.5f);
            normalize(b, 0.6f);
            out[SFX_MUSIC_RUN] = b;
        }
    }
}

bool Audio::init() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::printf("[iris] audio: no audio (%s)\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = platform_is_vita() ? 22050 : 44100; // (sounds are synthesized at 22050: the Vita mixes at that, half the work)
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = callback;
    want.userdata = this;
    // synthesize in the background: the game starts right away, sounds join a moment later
    synth_ = std::thread([this] {
        platform_thread_background(2);
        synth_all(buf_);
        ready_ = true;
    });
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!dev_) {
        std::printf("[iris] audio: can't open a device (%s)\n", SDL_GetError());
        if (synth_.joinable()) synth_.join();
        return false;
    }
    rate_ = have.freq;
    SDL_PauseAudioDevice(dev_, 0);
    std::printf("[iris] audio: %d Hz, synthesizing %d sounds\n", rate_, (int)SFX_COUNT);
    return true;
}

void Audio::shutdown() {
    mic_close();
    if (synth_.joinable()) synth_.join();
    if (dev_) SDL_CloseAudioDevice(dev_);
    dev_ = 0;
}

void Audio::play(Sfx s, float vol, float pan, float pitch, float muffle) {
    if (!dev_ || !ready_ || vol <= 0.003f) return;
    SDL_LockAudioDevice(dev_);
    Voice* best = &voices_[0];
    for (Voice& v : voices_) {
        if (v.sfx < 0) { best = &v; break; }
        if (v.vol < best->vol) best = &v; // steal the quietest
    }
    *best = Voice{};
    best->sfx = s;
    best->vol = best->target = std::min(vol, 1.5f);
    best->pan = std::max(-1.0f, std::min(1.0f, pan));
    best->pitch = pitch;
    best->muffle = muffle;
    SDL_UnlockAudioDevice(dev_);
}

void Audio::loop(Loop l, Sfx s, float vol, float pan, float pitch, float muffle) {
    if (!dev_ || !ready_) return;
    SDL_LockAudioDevice(dev_);
    Voice& v = loops_[l];
    if (v.sfx != s) { v = Voice{}; v.sfx = s; v.looping = true; v.vol = 0; }
    v.target = std::max(0.0f, vol);
    v.pan = std::max(-1.0f, std::min(1.0f, pan));
    v.pitch = pitch;
    v.muffle = muffle;
    SDL_UnlockAudioDevice(dev_);
}

void Audio::stop_loops() {
    if (!dev_) return;
    SDL_LockAudioDevice(dev_);
    for (Voice& v : loops_) v.target = 0;
    SDL_UnlockAudioDevice(dev_);
}

// ----- 1.3 proximity chat
bool Audio::mic_open() {
    if (mic_) return true;
    if (!dev_) return false;
    SDL_AudioSpec want{}, have{};
    want.freq = 16000;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 320;
    want.callback = mic_callback;
    want.userdata = this;
    mic_ring_.assign(16000, 0);
    mic_w_ = mic_r_ = 0;
    mic_ = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0); // SDL converts to 16 kHz mono for us
    if (!mic_) { std::printf("[iris] voice: no microphone (%s)\n", SDL_GetError()); return false; }
    SDL_PauseAudioDevice(mic_, 0);
    std::printf("[iris] voice: microphone open\n");
    return true;
}

void Audio::mic_close() {
    if (!mic_) return;
    SDL_CloseAudioDevice(mic_);
    mic_ = 0;
}

void Audio::mic_callback(void* user, uint8_t* stream, int len) {
    Audio* a = (Audio*)user;
    const int16_t* in = (const int16_t*)stream;
    int n = len / 2;
    size_t cap = a->mic_ring_.size();
    for (int i = 0; i < n; i++) a->mic_ring_[(size_t)(a->mic_w_++ % cap)] = in[i];
}

int Audio::mic_read(int16_t* out, int max) {
    if (!mic_) return 0;
    SDL_LockAudioDevice(mic_);
    size_t cap = mic_ring_.size();
    if (mic_w_ - mic_r_ > cap) mic_r_ = mic_w_ - cap; // (fell behind: drop the oldest)
    int n = 0;
    while (mic_r_ < mic_w_ && n < max) out[n++] = mic_ring_[(size_t)(mic_r_++ % cap)];
    SDL_UnlockAudioDevice(mic_);
    return n;
}

void Audio::voice_push(int speaker, const int16_t* pcm, int n, float vol, float pan, float muffle, bool radio) {
    if (!dev_ || speaker < 0 || speaker >= 4) return;
    SDL_LockAudioDevice(dev_);
    VoiceOut& v = vout_[speaker];
    if (v.ring.empty()) v.ring.assign(16000, 0.0f);
    size_t cap = v.ring.size();
    if ((double)v.w - v.r > 8000) v.r = (double)v.w - 1600; // more than half a second behind: catch up
    for (int i = 0; i < n; i++) v.ring[(size_t)(v.w++ % cap)] = pcm[i] / 32768.0f;
    v.target = std::max(0.0f, std::min(vol, 2.5f));
    v.pan = std::max(-1.0f, std::min(1.0f, pan));
    v.muffle = muffle;
    v.radio = radio;
    SDL_UnlockAudioDevice(dev_);
}

void Audio::voice_clear() {
    if (!dev_) return;
    SDL_LockAudioDevice(dev_);
    for (VoiceOut& v : vout_) { v.r = (double)v.w; v.playing = false; }
    SDL_UnlockAudioDevice(dev_);
}

void Audio::callback(void* user, uint8_t* stream, int len) { ((Audio*)user)->mix((int16_t*)stream, len / 4); }

void Audio::mix(int16_t* out, int frames) {
    if (!ready_) { std::fill(out, out + frames * 2, (int16_t)0); return; }
    if (acc_.size() < (size_t)frames * 2) acc_.resize((size_t)frames * 2); // (normally sized once)
    std::fill(acc_.begin(), acc_.begin() + frames * 2, 0.0f);
    std::vector<float>& acc = acc_;
    const float step_base = (float)SYNTH_RATE / (float)rate_;
    auto run = [&](Voice& v) {
        if (v.sfx < 0) return;
        const std::vector<float>& b = buf_[v.sfx];
        if (b.empty()) { v.sfx = -1; return; }
        float lpa = 1.0f - std::exp(-TAU * (300.0f + (1.0f - v.muffle) * 9000.0f) / rate_); // muffle = a low-pass
        float gl = std::sqrt(0.5f * (1.0f - v.pan)), gr = std::sqrt(0.5f * (1.0f + v.pan));
        for (int i = 0; i < frames; i++) {
            if (v.looping) v.vol += (v.target - v.vol) * 0.0008f; // smooth fades
            size_t i0 = (size_t)v.pos;
            if (i0 + 1 >= b.size()) {
                if (v.looping) { v.pos -= (double)b.size(); i0 = (size_t)std::max(0.0, v.pos); }
                else { v.sfx = -1; return; }
            }
            float fr = (float)(v.pos - (double)i0);
            float x = b[i0] * (1 - fr) + b[std::min(i0 + 1, b.size() - 1)] * fr;
            v.lp += lpa * (x - v.lp);
            float s = v.lp * v.vol;
            acc[(size_t)i * 2] += s * gl;
            acc[(size_t)i * 2 + 1] += s * gr;
            v.pos += step_base * v.pitch;
        }
    };
    for (Voice& v : voices_) run(v);
    // voices: each speaker's stream, 60 ms buffered before it starts (so packets arriving unevenly don't stutter)
    for (VoiceOut& v : vout_) {
        if (v.ring.empty()) continue;
        double have = (double)v.w - v.r;
        if (!v.playing) { if (have >= 960) v.playing = true; else continue; }
        const size_t cap = v.ring.size();
        const double step = 16000.0 / rate_;
        float lpa = 1.0f - std::exp(-TAU * (v.radio ? 2600.0f : 300.0f + (1.0f - v.muffle) * 7000.0f) / rate_);
        float hpa = 1.0f - std::exp(-TAU * (v.radio ? 500.0f : 90.0f) / rate_);
        float gl = std::sqrt(0.5f * (1.0f - v.pan)), gr = std::sqrt(0.5f * (1.0f + v.pan));
        for (int i = 0; i < frames; i++) {
            if ((double)v.w - v.r < 2) { v.playing = false; break; } // ran dry: wait for more
            v.vol += (v.target - v.vol) * 0.002f;
            uint64_t i0 = (uint64_t)v.r;
            float fr = (float)(v.r - (double)i0);
            float x = v.ring[(size_t)(i0 % cap)] * (1 - fr) + v.ring[(size_t)((i0 + 1) % cap)] * fr;
            v.lp += lpa * (x - v.lp);
            v.hp += hpa * (v.lp - v.hp);
            float y = v.lp - v.hp;                              // band-limited
            if (v.radio) y = std::max(-0.3f, std::min(0.3f, y * 2.2f)) + ((float)((hiss_ = hiss_ * 1664525u + 1013904223u) >> 24) / 255.0f - 0.5f) * 0.01f; // handset crunch + hiss
            float s = y * v.vol;
            acc[(size_t)i * 2] += s * gl;
            acc[(size_t)i * 2 + 1] += s * gr;
            v.r += step;
        }
    }
    for (int l = 0; l < LOOP_COUNT; l++) {
        Voice& v = loops_[l];
        if (!(v.target > 0.0005f || v.vol > 0.0005f)) continue;
        if (l == LOOP_BGM) { // music: its own volume (applied by scaling the target, so changes stay smooth)
            float t = v.target;
            v.target = t * music_;
            float vol0 = v.vol;
            (void)vol0;
            run(v);
            v.target = t;
        } else run(v);
    }
    for (int i = 0; i < frames * 2; i++) {
        // soft limit (no harsh clipping): a rational tanh, much cheaper than std::tanh per sample
        float v = std::max(-3.0f, std::min(3.0f, acc[(size_t)i] * master_ * 1.6f));
        float x = v * (27.0f + v * v) / (27.0f + 9.0f * v * v);
        out[i] = (int16_t)(x * 32000.0f);
    }
}

} // namespace iris
