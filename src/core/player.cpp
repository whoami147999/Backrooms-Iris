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
        case DeathCause::Landlord: return "THE LANDLORD DOESN'T TAKE TRESPASSERS";
        case DeathCause::Faceling: return "THEY TOOK EVERYTHING. THEN YOU";
        case DeathCause::Mangled: return "SOMETHING VERY BIG STEPPED ON YOU";
        case DeathCause::Animation: return "YOU BECAME PART OF THE CARTOON";
        case DeathCause::Heat: return "HEATSTROKE, IN THE SNOW";
        case DeathCause::Cold: return "YOU FROZE IN FROIDETOWN";
        case DeathCause::Toxic: return "THE SEA ATE THROUGH YOU";
        default: return "YOU DIED";
    }
}

const PerkInfo& perk_info(Perk p) {
    static const PerkInfo P[PERK_COUNT] = {
        {"BIG LUNGS", "+25% STAMINA"},
        {"SECOND WIND", "+35% STAMINA RECOVERY"},
        {"STEADY NERVES", "-20% SANITY LOSS"},
        {"THICK SKIN", "-15% DAMAGE TAKEN"},
        {"LIGHT FEET", "-20% NOISE"},
        {"SPARE CELLS", "+40% FLASHLIGHT BATTERY"},
    };
    return P[(int)p < PERK_COUNT ? (int)p : 0];
}

float PlayerState::eye_height() const { return lerpf(EYE_STAND, EYE_CROUCH, crouch); }

static float raw_noise(float speed, float crouch) {
    if (speed < 0.3f) return 0.5f;
    if (crouch > 0.5f) return 2.0f;
    if (speed > WALK_SPEED * 1.2f) return 17.0f;
    return 7.0f;
}

float PlayerState::noise() const {
    if (!alive || silent > 0) return 0;
    return raw_noise(speed, crouch) * (lucky > 0 ? 0.3f : 1.0f) * std::pow(0.8f, (float)perk(Perk::Quiet)); // lucky o' milk: nobody notices you
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

    bool juiced = p.juice >= 0;           // memory juice: you're fast, you don't get tired... for now
    bool boosted = p.boost > 0 || juiced;
    bool wants_sprint = (cmd.buttons & BTN_SPRINT) && !crouching && mz > 0.3f && mag > 0.5f;
    if (p.exhausted && p.stamina > 0.35f) p.exhausted = false;
    bool sprinting = wants_sprint && !p.exhausted;
    if (sprinting && !boosted) {
        p.stamina -= STAMINA_DRAIN * dt / (1.0f + 0.25f * p.perk(Perk::Stamina));
        if (p.stamina <= 0) { p.stamina = 0; p.exhausted = true; }
    } else if (!sprinting) {
        p.stamina = std::min(1.0f, p.stamina + STAMINA_REGEN * dt * (mag < 0.1f ? 1.6f : 1.0f) * (1.0f + 0.35f * p.perk(Perk::Lungs)));
    }

    float speed = crouching ? CROUCH_SPEED : (sprinting ? SPRINT_SPEED : WALK_SPEED);
    if (boosted) speed *= juiced ? 1.65f : BOOST_MULT;
    if (p.slow > 0) speed *= 0.72f;
    if (world.water(floor_to_int(p.pos.x / CELL), floor_to_int(p.pos.z / CELL))) speed *= 0.62f; // wading through the sea
    if (world.terrain()) {
        float wy = world.water_y(p.pos.x, p.pos.z);
        if (wy > -1e8f) speed *= 1.0f - 0.45f * clampf((wy - p.pos.y) / 1.2f, 0, 1); // wading, then swimming
    }
    Vec2 fwd = yaw_forward(p.yaw), right = yaw_right(p.yaw);
    Vec2 target = (fwd * mz + right * mx) * speed;

    // snappy but not instant: accelerate fast, decelerate a little faster
    float accel = (mag > 0.01f ? 14.0f : 18.0f) * dt;
    p.vel.x = approach(p.vel.x, target.x, accel * std::max(speed, 2.0f));
    p.vel.z = approach(p.vel.z, target.z, accel * std::max(speed, 2.0f));

    Vec2 before{p.pos.x, p.pos.z};
    Vec2 pos = before + p.vel * dt;
    if (!p.noclip) collide_circle(world, pos, PLAYER_RADIUS);
    p.pos.x = pos.x;
    p.pos.z = pos.z;
    if (world.terrain()) { // pools and hills: follow the ground (a little smoothing on the way down)
        float gy = world.floor_y(pos.x, pos.z);
        p.pos.y = gy > p.pos.y ? gy : approach(p.pos.y, gy, dt * 6.0f);
    } else {
        p.pos.y = 0;
    }

    float moved = (pos - before).len();
    p.speed = moved / dt;
    // bleed velocity that went into walls so you don't "stick"
    p.vel.x = (pos.x - before.x) / dt * 0.5f + p.vel.x * 0.5f;
    p.vel.z = (pos.z - before.z) / dt * 0.5f + p.vel.z * 0.5f;
    p.bob += moved * (sprinting ? 2.6f : 3.2f);
    if (p.bob > 2 * PI * 64) p.bob -= 2 * PI * 64;
}

} // namespace iris
