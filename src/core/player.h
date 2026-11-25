// Backrooms Iris - player movement. Pure simulation: (state, input, dt) -> state.
// The same function runs on the client (prediction) and on the server (authority).
#pragma once
#include <cstdint>

#include "math.h"
#include "world.h"

namespace iris {

enum InputButtons : uint8_t { BTN_SPRINT = 1, BTN_CROUCH = 2 };

// One tick of player intent. This is exactly what goes over the network.
struct InputCmd {
    float move_x = 0;  // strafe, -1..1 (right positive)
    float move_z = 0;  // forward, -1..1 (forward positive)
    float yaw = 0;     // radians
    float pitch = 0;   // radians
    uint8_t buttons = 0;
    // Action counters: the client bumps one per press and the sim acts on every change.
    // Counters (not edges) survive packet loss: any later packet still carries the bump.
    uint8_t pickup_ctr = 0;
    uint8_t use_ctr = 0;
    uint8_t use_item = 0;   // ItemType to use when use_ctr changes
    uint8_t light_ctr = 0;  // flashlight toggle
    uint8_t attack_ctr = 0; // crowbar swing
};

enum class DeathCause : uint8_t { None, Bacteria, Hound, Smiler, SkinStealer, Partygoer, Sanity, MemoryJuice };
const char* death_text(DeathCause c);

struct PlayerState {
    Vec3 pos;          // feet
    Vec2 vel;
    float yaw = 0, pitch = 0;
    float crouch = 0;  // 0 standing .. 1 crouched
    float stamina = 1; // 0..1
    bool exhausted = false;
    float bob = 0;     // head-bob phase (radians), advances with distance walked
    float speed = 0;   // current horizontal speed

    // survival
    float health = 100;
    float sanity = 100;
    float boost = 0;       // energy drink seconds left
    float slow = 0;        // cashew water seconds left
    float lucky = 0;       // lucky o' milk seconds left
    float juice = -1;      // seconds since drinking memory juice (-1 = sober)
    float hurt = 99;       // seconds since last damage (regen + red flash)
    float swing = 99;      // seconds since the last crowbar swing
    float battery = 1;     // flashlight charge 0..1
    bool light_on = false; // flashlight
    bool noclip = false;   // sandbox: walk through walls
    uint8_t inv[ITEM_COUNT] = {};
    bool alive = true;
    DeathCause cause = DeathCause::None;

    float eye_height() const;
    float noise() const;   // how far away hounds can hear you, metres
    bool has(ItemType t) const { return inv[(int)t] > 0; }
};

void player_tick(PlayerState& p, const InputCmd& cmd, const World& world, float dt);

} // namespace iris
