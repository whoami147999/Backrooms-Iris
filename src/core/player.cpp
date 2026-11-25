#include "player.h"

#include "collision.h"
#include "config.h"

namespace iris {

float PlayerState::eye_height() const { return lerpf(EYE_STAND, EYE_CROUCH, crouch); }

void player_tick(PlayerState& p, const InputCmd& cmd, const Level0& level, float dt) {
    p.yaw = cmd.yaw;
    p.pitch = clampf(cmd.pitch, -1.45f, 1.45f);

    bool crouching = (cmd.buttons & BTN_CROUCH) != 0;
    p.crouch = approach(p.crouch, crouching ? 1.0f : 0.0f, dt * 5.0f);

    float mx = clampf(cmd.move_x, -1, 1), mz = clampf(cmd.move_z, -1, 1);
    float mag = std::sqrt(mx * mx + mz * mz);
    if (mag > 1) { mx /= mag; mz /= mag; mag = 1; }

    bool wants_sprint = (cmd.buttons & BTN_SPRINT) && !crouching && mz > 0.3f && mag > 0.5f;
    if (p.exhausted && p.stamina > 0.35f) p.exhausted = false;
    bool sprinting = wants_sprint && !p.exhausted;
    if (sprinting) {
        p.stamina -= STAMINA_DRAIN * dt;
        if (p.stamina <= 0) { p.stamina = 0; p.exhausted = true; }
    } else {
        p.stamina = std::min(1.0f, p.stamina + STAMINA_REGEN * dt * (mag < 0.1f ? 1.6f : 1.0f));
    }

    float speed = crouching ? CROUCH_SPEED : (sprinting ? SPRINT_SPEED : WALK_SPEED);
    Vec2 fwd = yaw_forward(p.yaw), right = yaw_right(p.yaw);
    Vec2 target = (fwd * mz + right * mx) * speed;

    // snappy but not instant: accelerate fast, decelerate a little faster
    float accel = (mag > 0.01f ? 14.0f : 18.0f) * dt;
    p.vel.x = approach(p.vel.x, target.x, accel * std::max(speed, 2.0f));
    p.vel.z = approach(p.vel.z, target.z, accel * std::max(speed, 2.0f));

    Vec2 before{p.pos.x, p.pos.z};
    Vec2 pos = before + p.vel * dt;
    collide_circle(level, pos, PLAYER_RADIUS);
    p.pos.x = pos.x;
    p.pos.z = pos.z;

    // actual speed after collisions (walking into a wall shouldn't bob your head)
    float moved = (pos - before).len();
    p.speed = moved / dt;
    // stop velocity going into walls so you don't "stick"
    if (dt > 0) {
        p.vel.x = (pos.x - before.x) / dt * 0.5f + p.vel.x * 0.5f;
        p.vel.z = (pos.z - before.z) / dt * 0.5f + p.vel.z * 0.5f;
    }
    p.bob += moved * (sprinting ? 2.6f : 3.2f);
    if (p.bob > 2 * PI * 64) p.bob -= 2 * PI * 64;
}

} // namespace iris
