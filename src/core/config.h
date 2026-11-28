// Backrooms Iris - shared constants. Client and server must agree on these (they're bumped
// together with IRIS_PROTOCOL_VERSION).
#pragma once

#define IRIS_VERSION "1.3"
#define IRIS_PROTOCOL_VERSION 10
#define IRIS_DEFAULT_PORT 27315
#define IRIS_MAX_PLAYERS 4

// Multiplayer (host a game / join a friend) is PC only. The Vita build has no network code.
#ifndef IRIS_MULTIPLAYER
#if defined(__vita__)
#define IRIS_MULTIPLAYER 0
#else
#define IRIS_MULTIPLAYER 1
#endif
#endif
// 1.3: proximity voice chat rides on the multiplayer code (PC only)
#ifndef IRIS_VOICE
#define IRIS_VOICE IRIS_MULTIPLAYER
#endif

namespace iris {

// World units are metres.
constexpr float CELL = 2.5f;              // grid cell size
constexpr float WALL_T = 0.16f;           // thin wall thickness
constexpr float POST_T = WALL_T + 0.02f;  // corner posts are slightly fatter than walls (hides seams)
constexpr int CHUNK_CELLS = 8;            // cells per chunk side (render streaming unit)
constexpr float CHUNK_SIZE = CHUNK_CELLS * CELL;

// Player
constexpr float PLAYER_RADIUS = 0.28f;
constexpr float EYE_STAND = 1.62f;
constexpr float EYE_CROUCH = 1.0f;
constexpr float WALK_SPEED = 2.3f;
constexpr float SPRINT_SPEED = 4.4f;
constexpr float CROUCH_SPEED = 1.2f;
constexpr float BOOST_MULT = 1.45f;       // energy drink
constexpr float STAMINA_DRAIN = 1.0f / 7.0f;  // full bar lasts ~7 s of sprinting
constexpr float STAMINA_REGEN = 1.0f / 9.0f;
constexpr float PICKUP_RANGE = 2.0f;

// Items
constexpr float ENERGY_TIME = 14.0f;      // seconds of speed boost
constexpr float JUICE_TIME = 48.0f;       // memory juice: seconds until it kills you
constexpr int MAX_ITEM_STACK = 5;

constexpr float TICK_RATE = 60.0f;        // player simulation rate (client and server)

} // namespace iris
