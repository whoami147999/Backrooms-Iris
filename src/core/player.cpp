#include "player.h"

#include "collision.h"
#include "config.h"

namespace iris {

const char* death_text(DeathCause c) {
    switch (c) {
        case DeathCause::Bacteria: return "THE BACTERIA CAUGHT YOU";
        case DeathCause::Hound: return "THE HOUNDS GOT YOU";
        case DeathCause::Smiler: return "YOU STARED AT THE SMILE TOO LONG";
        case DeathCause::SkinStealer: return "SOMETHING IS WEARING YOU NOW";
        case DeathCause::Partygoer: return "YOU JOINED THE PARTY =)";
        case DeathCause::Sanity: return "YOU LOST YOUR MIND";
        case DeathCause::MemoryJuice: return "YOU NEVER WOKE UP";
        default: return "YOU DIED";
    }
}

float PlayerState::eye_height() const { return lerpf(EYE_STAND, EYE_CROUCH, crouch); }

float PlayerState::noise() const {
    if (!alive) return 0;
    if (speed < 0.3f) return 0.5f;
    if (crouch > 0.5f) return 2.0f;
    if (speed > WALK_SPEED * 1.2f) return 17.0f;
    return 7.0f;
}

void player_tick(PlayerState& p, const InputCmd& cmd, const World& world, float dt) {
    p.yaw = cmd.yaw;
    p.pitch = clampf(cmd.pitch, -1.45f, 1.45f);
    if (!p.alive) { p.vel = {0, 0}; p.speed = 0; return; }

    bool crouching = (cmd.buttons & BTN_CROUCH) != 0;
    p.crouch = approach(p.crouch, crouching ? 1.0f : 0.0f, dt * 5.0f);

    float mx = clampf(cmd.move_x, -1, 1), mz = clampf(cmd.move_z, -1, 1);
    float mag = std::sqrt(mx * mx + mz * mz);
    if (mag > 1) { mx /= mag; mz /= mag; mag = 1; }

    bool boosted = p.boost > 0;
    bool wants_sprint = (cmd.buttons & BTN_SPRINT) && !crouching && mz > 0.3f && mag > 0.5f;
    if (p.exhausted && p.stamina > 0.35f) p.exhausted = false;
    bool sprinting = wants_sprint && !p.exhausted;
    if (sprinting && !boosted) {
        p.stamina -= STAMINA_DRAIN * dt;
        if (p.stamina <= 0) { p.stamina = 0; p.exhausted = true; }
    } else if (!sprinting) {
        p.stamina = std::min(1.0f, p.stamina + STAMINA_REGEN * dt * (mag < 0.1f ? 1.6f : 1.0f));
    }

    float speed = crouching ? CROUCH_SPEED : (sprinting ? SPRINT_SPEED : WALK_SPEED);
    if (boosted) speed *= BOOST_MULT;
    Vec2 fwd = yaw_forward(p.yaw), right = yaw_right(p.yaw);
    Vec2 target = (fwd * mz + right * mx) * speed;

    // snappy but not instant: accelerate fast, decelerate a little faster
    float accel = (mag > 0.01f ? 14.0f : 18.0f) * dt;
    p.vel.x = approach(p.vel.x, target.x, accel * std::max(speed, 2.0f));
    p.vel.z = approach(p.vel.z, target.z, accel * std::max(speed, 2.0f));

    Vec2 before{p.pos.x, p.pos.z};
    Vec2 pos = before + p.vel * dt;
    collide_circle(world, pos, PLAYER_RADIUS);
    p.pos.x = pos.x;
    p.pos.z = pos.z;

    float moved = (pos - before).len();
    p.speed = moved / dt;
    // bleed velocity that went into walls so you don't "stick"
    p.vel.x = (pos.x - before.x) / dt * 0.5f + p.vel.x * 0.5f;
    p.vel.z = (pos.z - before.z) / dt * 0.5f + p.vel.z * 0.5f;
    p.bob += moved * (sprinting ? 2.6f : 3.2f);
    if (p.bob > 2 * PI * 64) p.bob -= 2 * PI * 64;
}

} // namespace iris
