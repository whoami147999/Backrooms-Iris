// Backrooms Iris - player movement. Pure simulation: (state, input, dt) -> state.
// The same function runs on the client (prediction) and, later, on the dedicated server
// (authority), which is what makes server-authoritative netcode with lag compensation work.
#pragma once
#include <cstdint>

#include "level0.h"
#include "math.h"

namespace iris {

enum InputButtons : uint8_t { BTN_SPRINT = 1, BTN_CROUCH = 2 };

// One tick of player intent. This is what will go over the network.
struct InputCmd {
    float move_x = 0;  // strafe, -1..1 (right positive)
    float move_z = 0;  // forward, -1..1 (forward positive)
    float yaw = 0;     // radians
    float pitch = 0;   // radians
    uint8_t buttons = 0;
};

struct PlayerState {
    Vec3 pos;          // feet
    Vec2 vel;
    float yaw = 0, pitch = 0;
    float crouch = 0;  // 0 standing .. 1 crouched
    float stamina = 1; // 0..1
    bool exhausted = false;
    float bob = 0;     // head-bob phase (radians), advances with distance walked
    float speed = 0;   // current horizontal speed (for bob amplitude / footsteps later)

    float eye_height() const;
};

void player_tick(PlayerState& p, const InputCmd& cmd, const Level0& level, float dt);

} // namespace iris
