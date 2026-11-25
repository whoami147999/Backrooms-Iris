// Backrooms Iris - shared constants. Changing these changes the world for everyone, so
// client and server must agree (they're bumped together with IRIS_PROTOCOL_VERSION).
#pragma once

#define IRIS_VERSION "0.1.0"
#define IRIS_PROTOCOL_VERSION 1
#define IRIS_DEFAULT_PORT 27315
#define IRIS_MAX_PLAYERS 4

namespace iris {

// World units are metres.
constexpr float CELL = 2.5f;        // grid cell size
constexpr float CEIL_H = 2.7f;      // floor to ceiling
constexpr float WALL_T = 0.16f;     // wall thickness
constexpr float POST_T = WALL_T + 0.02f; // corner posts are slightly fatter than walls (hides seams)
constexpr float PILLAR_T = 0.6f;    // free-standing pillars
constexpr int CHUNK_CELLS = 8;      // cells per chunk side (render streaming unit)
constexpr float CHUNK_SIZE = CHUNK_CELLS * CELL;

// Player
constexpr float PLAYER_RADIUS = 0.28f;
constexpr float EYE_STAND = 1.62f;
constexpr float EYE_CROUCH = 1.0f;
constexpr float WALK_SPEED = 2.3f;
constexpr float SPRINT_SPEED = 4.4f;
constexpr float CROUCH_SPEED = 1.2f;
constexpr float STAMINA_DRAIN = 1.0f / 7.0f;   // full bar lasts ~7 s of sprinting
constexpr float STAMINA_REGEN = 1.0f / 9.0f;

constexpr float TICK_RATE = 60.0f;  // client simulation rate

} // namespace iris
