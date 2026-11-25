#include "effects.h"

#include <cmath>

#include "../core/config.h"
#include "../core/hash.h"

namespace iris {
namespace {

float ramp(float t, float a, float b) { return clampf((t - a) / (b - a), 0, 1); }

const char* WHISPERS[] = {
    "WHO ARE YOU TALKING TO?",
    "THEY'RE STANDING RIGHT BEHIND YOU",
    "YOUR HANDS ARE NOT YOUR HANDS",
    "SOMEONE IS CALLING YOUR NAME",
    "THE WALLS ARE BREATHING",
    "DID YOU HEAR THAT?",
    "THAT ISN'T YOUR FRIEND",
    "YOU'VE BEEN HERE BEFORE",
    "DON'T LOOK AT THE CORNER",
    "IT'S FINE. IT'S FINE. IT'S FINE.",
    "WHAT WAS YOUR NAME AGAIN?",
};
const char* END_WHISPERS[] = {
    "YOU DON'T REMEMBER YOUR NAME",
    "THERE IS NO WAY OUT",
    "STOP. STOP. STOP.",
    "THEY'RE ALL HERE NOW",
};

// A phantom in the world that fades when you look straight at it (they never let you see them).
Phantom shy(PhantomKind kind, Vec3 pos, Vec3 eye, float yaw, float alpha, int variant) {
    Vec2 d{pos.x - eye.x, pos.z - eye.z};
    float dist = d.len();
    Vec2 f = yaw_forward(yaw);
    float look = dist > 0.01f ? (d.x * f.x + d.z * f.z) / dist : 1;
    if (kind == PhantomKind::Shadow && look > 0.93f && dist < 9.0f) alpha *= clampf((1.0f - look) / 0.07f, 0, 1);
    if (kind == PhantomKind::FakeItem && dist < 1.4f) alpha *= clampf((dist - 0.8f) / 0.6f, 0, 1);
    return {kind, pos, alpha, variant};
}

} // namespace

ScreenFx compute_fx(const PlayerState& me, Vec3 eye, float yaw, float time, int seed) {
    ScreenFx fx;
    if (!me.alive) return fx;
    uint32_t s = (uint32_t)seed;

    // ---------------------------------------------------------------- sanity
    float insanity = 1.0f - me.sanity / 100.0f;
    if (me.sanity < 60) {
        float k = ramp(me.sanity, 60, 0);
        fx.vignette = std::max(fx.vignette, k * 0.75f);
        fx.grain = k * 0.25f;
        fx.tint[0] = 0.05f; fx.tint[1] = 0.02f; fx.tint[2] = 0.08f; fx.tint[3] = k * 0.25f;
    }
    if (me.sanity < 35) {
        float k = ramp(me.sanity, 35, 0);
        fx.roll = std::sin(time * 0.6f) * 0.035f * k;
        fx.fov_add = std::sin(time * 0.9f) * 3.0f * k;
        fx.wobble = std::max(fx.wobble, k * 0.35f);
        // now and then something stands at the edge of your vision
        int slot = (int)(time / 7.0f);
        if (hash01(s, slot, 1, 11) < 0.5f + k * 0.4f) {
            float a = yaw + (hash01(s, slot, 2, 11) < 0.5f ? 1 : -1) * (0.55f + hash01(s, slot, 3, 11) * 0.35f);
            float d = 6.0f + hash01(s, slot, 4, 11) * 6.0f;
            Vec2 f = yaw_forward(a);
            fx.phantoms.push_back(shy(PhantomKind::Shadow, {eye.x + f.x * d, 0, eye.z + f.z * d}, eye, yaw, 0.85f * k, 0));
        }
    }
    (void)insanity;

    // ---------------------------------------------------------------- energy drink
    if (me.boost > 0) {
        float k = std::min(1.0f, me.boost / 2.0f);
        fx.fov_add += 7.0f * k;
        fx.tint[0] = 0.2f; fx.tint[1] = 0.9f; fx.tint[2] = 0.3f; fx.tint[3] = std::max(fx.tint[3], 0.05f * k);
    }

    // ---------------------------------------------------------------- hurt
    if (me.hurt < 0.6f) fx.red_flash = (0.6f - me.hurt) / 0.6f * 0.55f;
    if (me.health < 35) fx.red_flash = std::max(fx.red_flash, (0.5f + 0.5f * std::sin(time * 6)) * 0.25f * ramp(me.health, 35, 0));

    // ---------------------------------------------------------------- memory juice
    if (me.juice >= 0) {
        float t = me.juice;
        // phase 1 (0-8 s): warm, floaty, almost nice
        float p1 = ramp(t, 0, 4);
        fx.tint[0] = 1.0f; fx.tint[1] = 0.55f; fx.tint[2] = 0.15f; fx.tint[3] = 0.10f * p1;
        fx.wobble = std::max(fx.wobble, 0.35f * p1);
        fx.fov_add += std::sin(time * 0.8f) * 4.0f * p1;
        // phase 2 (8-22 s): pupils blown, double vision, shadow people, things on the floor
        float p2 = ramp(t, 8, 14);
        fx.ghost = p2 * 0.8f;
        fx.wobble = std::max(fx.wobble, 0.35f + 0.45f * p2);
        fx.vignette = std::max(fx.vignette, 0.35f * p2);
        if (p2 > 0) {
            int n = 2 + (int)(p2 * 3);
            for (int i = 0; i < n; i++) {
                float a = yaw + (i - n / 2.0f) * 0.9f + std::sin(time * 0.1f + i) * 0.3f;
                float d = 5.0f + 4.0f * hash01(s, i, 7, 21);
                Vec2 f = yaw_forward(a);
                fx.phantoms.push_back(shy(PhantomKind::Shadow, {eye.x + f.x * d, 0, eye.z + f.z * d}, eye, yaw, 0.8f * p2, i));
            }
            for (int i = 0; i < 3; i++) { // items that aren't there
                float a = yaw + (hash01(s, i, 8, 21) - 0.5f) * 1.6f;
                float d = 2.5f + 3.0f * hash01(s, i, 9, 21);
                Vec2 f = yaw_forward(a);
                fx.phantoms.push_back(shy(PhantomKind::FakeItem, {eye.x + f.x * d, 0, eye.z + f.z * d}, eye, yaw, p2, i));
            }
        }
        // phase 3 (22-36 s): spiders on the screen, someone standing there staring, whispers
        float p3 = ramp(t, 22, 26);
        fx.roll += std::sin(time * 0.7f) * 0.06f * p3;
        fx.tint[0] = lerpf(fx.tint[0], 0.6f, p3); fx.tint[1] = lerpf(fx.tint[1], 0.1f, p3);
        fx.tint[2] = lerpf(fx.tint[2], 0.5f, p3); fx.tint[3] = lerpf(fx.tint[3], 0.18f, p3);
        if (p3 > 0) {
            int n = (int)(p3 * 4 + ramp(t, 26, 40) * 10);
            for (int i = 0; i < n; i++) {
                float sp = 0.03f + 0.04f * hash01(s, i, 1, 31);
                float ph = hash01(s, i, 2, 31) * 10;
                float x = std::fmod(hash01(s, i, 3, 31) + (time + ph) * sp * (i % 2 ? 1 : -1) + 10.0f, 1.0f);
                float y = std::fmod(hash01(s, i, 4, 31) + (time + ph) * sp * 0.6f + 10.0f, 1.0f);
                float rot = std::atan2(sp * 0.6f, sp * (i % 2 ? 1 : -1)) + PI / 2 + std::sin(time * 9 + i) * 0.2f;
                fx.spiders.push_back({x, y, rot, 0.035f + 0.03f * hash01(s, i, 5, 31)});
            }
            // a figure that looks just like a friend, right there, watching
            float a = yaw + 0.25f * std::sin(time * 0.05f);
            Vec2 f = yaw_forward(a);
            fx.phantoms.push_back({PhantomKind::Figure, {eye.x + f.x * 6.5f, 0, eye.z + f.z * 6.5f}, p3, (int)(t / 6) & 3});
            int w = (int)(t / 3.5f);
            float local = std::fmod(t, 3.5f);
            if (hash01(s, w, 6, 31) < 0.75f) {
                fx.whisper = WHISPERS[hash3(s, w, 7, 31) % (sizeof(WHISPERS) / sizeof(WHISPERS[0]))];
                fx.whisper_alpha = p3 * std::sin(clampf(local / 3.5f, 0, 1) * PI);
                fx.wx = 0.2f + 0.6f * hash01(s, w, 8, 31);
                fx.wy = 0.25f + 0.5f * hash01(s, w, 9, 31);
            }
        }
        // phase 4 (36-48 s): the dark closes in, they all come closer, and then nothing
        float p4 = ramp(t, 36, JUICE_TIME);
        if (p4 > 0) {
            fx.vignette = std::max(fx.vignette, 0.5f + 0.5f * p4);
            fx.fog_mult = 1.0f + 2.5f * p4;
            fx.wobble = 1.0f;
            fx.ghost = 1.0f;
            fx.red_flash = std::max(fx.red_flash, (0.5f + 0.5f * std::sin(time * 4.5f)) * 0.35f * p4); // heartbeat
            int n = 8;
            for (int i = 0; i < n; i++) {
                float a = i * (2 * PI / n) + time * 0.05f;
                float d = 7.0f - 5.5f * p4;
                Vec2 f = yaw_forward(a);
                fx.phantoms.push_back({PhantomKind::Shadow, {eye.x + f.x * d, 0, eye.z + f.z * d}, 0.9f, i});
            }
            int w = (int)(t / 2.5f);
            fx.whisper = END_WHISPERS[hash3(s, w, 10, 31) % 4];
            fx.whisper_alpha = 0.6f + 0.4f * std::sin(time * 3);
            fx.wx = 0.5f;
            fx.wy = 0.5f;
            fx.blackout = ramp(t, JUICE_TIME - 3.0f, JUICE_TIME);
        }
    }
    return fx;
}

} // namespace iris
