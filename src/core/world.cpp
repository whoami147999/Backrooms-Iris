#include "world.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "hash.h"

namespace iris {
namespace {

enum Salt : uint32_t {
    S_DISTRICT = 11, S_LINE = 21, S_DOOR = 22, S_JITTER = 23, S_ORIENT = 31, S_PILLAR = 41,
    S_LIGHT = 51, S_LIGHTSTATE = 52, S_SOLID = 61, S_HEIGHT = 62, S_TINT = 63, S_LOT = 64,
    S_EXIT = 71, S_ITEM = 81, S_ITEMTYPE = 82, S_ITEMPOS = 83, S_NOISE = 91, S_BALLOON = 95, S_ROOM = 97, S_PROP = 101, S_CAMP = 111,
};

// clang-format off
const LevelInfo LEVELS[(int)LevelId::COUNT] = {
    // code              name                 num        ceil   wall   out    dark   fog                      dens    vita    ambient                  light colour            pow   rad   y      pillar drain  items   next
    {"LEVEL 0",         "THE LOBBY",         "0",       2.7f,  2.7f,  false, false, {0.24f, 0.21f, 0.12f},   0.055f, 0.070f, {0.17f, 0.16f, 0.13f},   {1.00f, 0.95f, 0.82f},  1.35f, 7.5f, 2.65f, 0.6f, 0.10f, 0.011f, LevelId::L1},
    {"LEVEL 1",         "HABITABLE ZONE",    "1",       4.2f,  4.2f,  false, false, {0.20f, 0.22f, 0.24f},   0.065f, 0.080f, {0.13f, 0.14f, 0.15f},   {0.92f, 0.96f, 1.00f},  1.50f, 9.0f, 3.80f, 1.0f, 0.12f, 0.022f, LevelId::L2},
    {"LEVEL 2",         "PIPE DREAMS",       "2",       2.5f,  2.5f,  false, false, {0.07f, 0.045f, 0.03f},  0.090f, 0.100f, {0.10f, 0.08f, 0.06f},   {1.00f, 0.55f, 0.25f},  1.25f, 6.5f, 2.35f, 0.6f, 0.20f, 0.016f, LevelId::L3},
    {"LEVEL 3",         "ELECTRICAL STATION","3",       3.0f,  3.0f,  false, false, {0.06f, 0.05f, 0.04f},   0.080f, 0.095f, {0.15f, 0.13f, 0.11f},   {1.00f, 0.62f, 0.30f},  1.25f, 6.5f, 2.85f, 0.6f, 0.25f, 0.013f, LevelId::L4},
    {"LEVEL 4",         "ABANDONED OFFICE",  "4",       2.8f,  1.55f, false, false, {0.20f, 0.21f, 0.23f},   0.050f, 0.065f, {0.16f, 0.17f, 0.19f},   {0.95f, 0.98f, 1.00f},  1.30f, 7.5f, 2.75f, 0.6f, 0.06f, 0.018f, LevelId::L5},
    {"LEVEL 5",         "TERROR HOTEL",      "5",       2.9f,  2.9f,  false, false, {0.10f, 0.06f, 0.04f},   0.085f, 0.100f, {0.10f, 0.07f, 0.05f},   {1.00f, 0.72f, 0.42f},  1.30f, 6.5f, 2.30f, 0.6f, 0.20f, 0.014f, LevelId::L6},
    {"LEVEL 6",         "LIGHTS OUT",        "6",       2.7f,  2.7f,  false, true,  {0.00f, 0.00f, 0.00f},   0.240f, 0.260f, {0.30f, 0.30f, 0.32f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 2.65f, 0.6f, 0.40f, 0.012f, LevelId::L7},
    {"LEVEL 7",         "THALASSOPHOBIA",    "7",       2.7f,  2.7f,  true,  false, {0.05f, 0.10f, 0.15f},   0.050f, 0.065f, {0.20f, 0.26f, 0.32f},   {0.70f, 0.90f, 1.00f},  1.20f, 8.0f, 2.60f, 0.6f, 0.25f, 0.010f, LevelId::L8},
    {"LEVEL 8",         "CAVE SYSTEM",       "8",       3.0f,  3.0f,  false, true,  {0.00f, 0.00f, 0.00f},   0.150f, 0.170f, {0.62f, 0.56f, 0.50f},   {0.45f, 1.00f, 0.70f},  0.90f, 6.0f, 1.20f, 0.6f, 0.35f, 0.012f, LevelId::L9},
    {"LEVEL 9",         "THE SUBURBS",       "9",       6.0f,  6.0f,  true,  false, {0.03f, 0.035f, 0.07f},  0.060f, 0.075f, {0.15f, 0.16f, 0.24f},   {1.00f, 0.66f, 0.30f},  1.60f, 11.f, 5.00f, 0.6f, 0.25f, 0.008f, LevelId::L10},
    {"LEVEL 10",        "BUMPER CROP",       "10",      60.0f, 60.0f, true,  false, {0.60f, 0.58f, 0.48f},   0.035f, 0.050f, {0.68f, 0.66f, 0.56f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 3.00f, 0.6f, 0.05f, 0.008f, LevelId::L19},
    {"LEVEL 11",        "THE ENDLESS CITY",  "11",      60.0f, 60.0f, true,  false, {0.62f, 0.64f, 0.67f},   0.030f, 0.045f, {0.88f, 0.88f, 0.90f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 3.00f, 0.6f, -0.60f, 0.006f, LevelId::L13},
    {"LEVEL 13",        "INFINITE APARTMENTS","13",     2.7f,  2.7f,  false, false, {0.13f, 0.11f, 0.08f},   0.070f, 0.085f, {0.13f, 0.11f, 0.09f},   {1.00f, 0.85f, 0.60f},  1.30f, 6.5f, 2.62f, 0.6f, 0.15f, 0.014f, LevelId::TSG0},
    {"LEVEL 19",        "THE ATTIC",         "19",      2.75f, 2.75f, false, true,  {0.00f, 0.00f, 0.00f},   0.160f, 0.180f, {0.40f, 0.36f, 0.30f},   {1.00f, 0.75f, 0.45f},  0.95f, 5.5f, 2.15f, 0.6f, 0.30f, 0.014f, LevelId::L21},
    {"LEVEL 21",        "NUMBERED DOORS",    "21",      2.6f,  2.6f,  false, false, {0.16f, 0.17f, 0.15f},   0.060f, 0.075f, {0.15f, 0.16f, 0.14f},   {0.95f, 1.00f, 0.92f},  1.30f, 7.0f, 2.55f, 0.6f, 0.12f, 0.012f, LevelId::L22},
    {"LEVEL 22",        "THE CAR PARK",      "22",      2.8f,  2.8f,  false, false, {0.09f, 0.085f, 0.07f},  0.070f, 0.085f, {0.10f, 0.10f, 0.09f},   {1.00f, 0.70f, 0.35f},  1.40f, 8.0f, 2.75f, 0.7f, 0.18f, 0.012f, LevelId::L33},
    {"LEVEL 24",        "THE MOON",          "24",      60.0f, 60.0f, true,  false, {0.00f, 0.00f, 0.00f},   0.022f, 0.035f, {0.72f, 0.72f, 0.76f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 3.00f, 0.6f, 0.15f, 0.008f, LevelId::L11},
    {"LEVEL 33",        "THE INFINITE MALL", "33",      11.0f, 11.0f, false, false, {0.30f, 0.31f, 0.30f},   0.040f, 0.055f, {0.30f, 0.31f, 0.30f},   {1.00f, 0.92f, 0.80f},  1.30f, 9.0f, 7.40f, 0.8f, 0.10f, 0.012f, LevelId::L36},
    {"LEVEL 36",        "AIRPORT",           "36",      4.5f,  4.5f,  false, false, {0.14f, 0.15f, 0.17f},   0.045f, 0.060f, {0.17f, 0.18f, 0.20f},   {0.92f, 0.96f, 1.00f},  1.40f, 9.0f, 4.40f, 0.9f, 0.08f, 0.012f, LevelId::L37},
    {"LEVEL 37",        "SUBLIMITY",         "37",      4.2f,  4.2f,  false, false, {0.42f, 0.55f, 0.55f},   0.030f, 0.050f, {0.34f, 0.42f, 0.42f},   {0.96f, 1.00f, 0.94f},  0.90f, 8.5f, 4.15f, 1.6f, -0.05f, 0.009f, LevelId::LFun},
    {"LEVEL 51",        "THE FORGOTTEN CITY","51",      8.0f,  8.0f,  false, true,  {0.02f, 0.015f, 0.01f},  0.110f, 0.130f, {0.45f, 0.38f, 0.30f},   {1.00f, 0.60f, 0.25f},  1.30f, 8.0f, 1.70f, 0.7f, 0.25f, 0.012f, LevelId::L70},
    {"LEVEL 70",        "FROIDETOWN",        "70",      60.0f, 60.0f, true,  false, {0.74f, 0.77f, 0.82f},   0.032f, 0.048f, {0.80f, 0.82f, 0.88f},   {1.00f, 0.86f, 0.62f},  1.50f, 10.f, 5.00f, 0.6f, 0.10f, 0.009f, LevelId::L94},
    {"LEVEL 94",        "MOTION",            "94",      60.0f, 60.0f, true,  false, {0.70f, 0.78f, 0.88f},   0.024f, 0.040f, {0.88f, 0.86f, 0.80f},   {1.00f, 0.88f, 0.62f},  1.50f, 10.f, 4.20f, 0.6f, 0.04f, 0.010f, LevelId::L188},
    {"LEVEL 188",       "COURTYARD OF WINDOWS","188",   24.0f, 24.0f, true,  false, {0.70f, 0.70f, 0.72f},   0.022f, 0.036f, {0.64f, 0.64f, 0.67f},   {1.00f, 0.92f, 0.76f},  1.20f, 7.0f, 3.20f, 0.6f, 0.32f, 0.012f, LevelId::L24},
    {"LEVEL FUN",       "=)",                "FUN",     2.7f,  2.7f,  false, false, {0.30f, 0.20f, 0.24f},   0.060f, 0.075f, {0.22f, 0.18f, 0.19f},   {1.00f, 0.90f, 0.85f},  1.40f, 7.5f, 2.65f, 0.6f, 0.30f, 0.012f, LevelId::LRun},
    {"LEVEL !",         "RUN FOR YOUR LIFE", "!",       2.7f,  2.7f,  false, false, {0.15f, 0.01f, 0.01f},   0.045f, 0.060f, {0.16f, 0.04f, 0.04f},   {1.00f, 0.15f, 0.10f},  1.50f, 7.0f, 2.65f, 0.6f, 0.60f, 0.040f, LevelId::L51},
    {"TSG - 0Z3R0",     "TH3 GR3Y JUNGL3",   "0ZERO",   60.0f, 60.0f, true,  false, {0.30f, 0.31f, 0.30f},   0.048f, 0.064f, {0.44f, 0.44f, 0.44f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 3.00f, 0.6f, 0.15f, 0.011f, LevelId::TSG1},
    {"TSG - 1ON3",      "TH3 L4NDL0RD'S H0US3","1ONE",  3.2f,  3.2f,  false, false, {0.10f, 0.10f, 0.10f},   0.080f, 0.095f, {0.11f, 0.11f, 0.11f},   {1.00f, 1.00f, 1.00f},  1.25f, 6.5f, 2.60f, 0.6f, 0.30f, 0.014f, LevelId::TSG2},
    {"TSG - 2TW0",      "TH3 H0T SN0W",      "2TWO",    60.0f, 60.0f, true,  false, {0.80f, 0.80f, 0.80f},   0.036f, 0.052f, {0.86f, 0.86f, 0.86f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 3.00f, 0.6f, 0.05f, 0.012f, LevelId::TSG3},
    {"TSG - 3THR33",    "TH3 CL0CK S3A",     "3THREE",  60.0f, 60.0f, true,  false, {0.44f, 0.45f, 0.46f},   0.034f, 0.050f, {0.56f, 0.56f, 0.57f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 3.00f, 0.6f, 0.20f, 0.010f, LevelId::TSG4},
    {"TSG - 4F0UR",     "TH3 BR0K3N C1TY",   "4FOUR",   60.0f, 60.0f, true,  false, {0.17f, 0.17f, 0.18f},   0.046f, 0.060f, {0.30f, 0.30f, 0.31f},   {1.00f, 1.00f, 1.00f},  1.40f, 9.0f, 5.00f, 0.6f, 0.25f, 0.009f, LevelId::TSGHope},
    {"TSG - L0ST H0P3", "TH3 W0BBLY M4Z3",   "HOPE",    2.7f,  2.7f,  false, true,  {0.00f, 0.00f, 0.00f},   0.220f, 0.240f, {0.25f, 0.25f, 0.25f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 2.65f, 0.6f, 0.50f, 0.012f, LevelId::TSGFar},
    {"TSG - 2 F4R",     "TURN B4CK",         "2FAR",    2.7f,  2.7f,  false, true,  {0.00f, 0.00f, 0.00f},   0.200f, 0.220f, {0.30f, 0.30f, 0.30f},   {1.00f, 0.18f, 0.12f},  0.90f, 4.5f, 2.65f, 0.6f, 0.45f, 0.010f, LevelId::TSGEnd},
    {"TH3 3ND",         "W3LC0M3 T0 TH3 3ND","END",     60.0f, 60.0f, true,  false, {0.52f, 0.52f, 0.53f},   0.050f, 0.065f, {0.66f, 0.66f, 0.66f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 3.00f, 0.6f, 0.35f, 0.000f, LevelId::TheEnd},
    {"THE END",         "THE LAST LEVEL",    "FINAL",   3.6f,  3.6f,  false, false, {0.40f, 0.40f, 0.38f},   0.032f, 0.046f, {0.34f, 0.33f, 0.30f},   {1.00f, 0.97f, 0.88f},  1.60f, 9.0f, 3.55f, 0.9f, 0.05f, 0.006f, LevelId::L0},
    {"M.E.G. BASE",     "THE LOBBY",         "HUB",     4.2f,  4.2f,  false, false, {0.14f, 0.15f, 0.15f},   0.045f, 0.060f, {0.20f, 0.20f, 0.19f},   {1.00f, 0.93f, 0.80f},  1.40f, 9.0f, 4.00f, 1.0f, -0.40f, 0.000f, LevelId::L0},
};

static_assert(sizeof(LEVELS) / sizeof(LEVELS[0]) == (size_t)LevelId::COUNT, "one row per level");

const ItemInfo ITEMS[ITEM_COUNT] = {
    {"NOTHING", false, ""},
    {"ALMOND WATER", false, "REFILLS STAMINA, CALMS YOU DOWN"},
    {"ENERGY DRINK", false, "FAST FOR A LITTLE WHILE"},
    {"MEMORY JUICE", false, "FAST. UNKILLABLE. THEN IT KILLS YOU"},
    {"ROYAL RATION", false, "A REAL MEAL. HEALS YOU"},
    {"LIQUID PAIN", false, "HURTS. CLEARS YOUR HEAD"},
    {"CASHEW WATER", false, "CURES MEMORY JUICE. MAKES YOU SLOW"},
    {"LUCKY O' MILK", false, "THEY STOP NOTICING YOU FOR A BIT"},
    {"FIRESALT", false, "THROW IT: BURNS BRIGHT, KEEPS THEM AWAY"},
    {"FLASHLIGHT", true, "F / TRIANGLE: LIGHT. SMILERS HATE IT"},
    {"CROWBAR", true, "SWING TO STUN WHATEVER IS IN FRONT OF YOU"},
    {"SUPER ALMOND WATER", false, "THE GOOD STUFF. HEALS BODY AND MIND"},
    {"MOTH JELLY", false, "SLOWLY HEALS YOU FOR A WHILE"},
    {"LIQUID SILENCE", false, "30 S WHERE YOU MAKE NO SOUND AT ALL"},
    {"MED KIT", false, "PATCHES YOU UP. A LOT"},
    {"FLASH BULB", false, "THROW IT: A BLINDING FLASH STUNS THEM"},
    {"GLOW STICK", false, "DROP IT: GREEN LIGHT FOR 3 MINUTES"},
    {"MUSIC BOX", false, "THROW IT: THEY GO TO THE MUSIC"},
    {"RABBIT'S FOOT", false, "KEEP IT. IT SAVES YOU FROM DYING ONCE"},
    {"COMPASS", true, "HOLD IT: ROUGHLY POINTS TO A NEARBY EXIT"},
    {"RADIO", true, "STATIC GETS LOUDER WHEN THEY'RE NEAR"},
    {"BATTERIES", false, "A FULL FLASHLIGHT, RIGHT NOW"},
    {"NIGHT VISION", true, "USE TO TOGGLE: SEE IN THE DARK (USES BATTERY)"},
    {"CHALK", false, "MARK THE FLOOR SO YOU KNOW WHERE YOU'VE BEEN"},
    {"PAINKILLERS", false, "HALF DAMAGE FOR 30 S"},
    {"PISTOL", true, "SANDBOX. CLICK TO SHOOT", true},
    {"SHOTGUN", true, "EIGHT PELLETS, SHORT RANGE. NEEDS SHELLS"},
    {"RIFLE", true, "SANDBOX. HOLD TO FIRE", true},
    {"SHOTGUN SHELLS", false, "AMMO FOR THE SHOTGUN. MAKE THEM COUNT"},
};
const DiffInfo DIFFS[(int)Difficulty::COUNT] = {
    // name        count  speed  dmg    sanity items  exits  grace  regen  batt   hearing
    {"EASY",      0.60f, 0.85f, 0.60f, 0.60f, 1.50f, 0.55f, 60.0f, 2.00f, 0.70f, 0.80f, "SLOWER ENTITIES, MORE ITEMS, MORE EXITS"},
    {"MEDIUM",    1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 0.36f, 35.0f, 1.00f, 1.00f, 1.00f, "THE BACKROOMS AS INTENDED"},
    {"HARD",      1.50f, 1.08f, 1.35f, 1.30f, 0.75f, 0.27f, 20.0f, 0.50f, 1.30f, 1.20f, "MORE OF THEM, FEWER EXITS, LESS TO DRINK"},
    {"NIGHTMARE", 2.00f, 1.18f, 1.80f, 1.70f, 0.50f, 0.20f, 8.0f,  0.00f, 1.60f, 1.40f, "NO HEALING. THEY HEAR EVERYTHING. GOOD LUCK"},
};
// clang-format on

float smooth(float t) { return t * t * (3 - 2 * t); }

float vnoise2(uint32_t seed, float x, float z, uint32_t salt) {
    int x0 = floor_to_int(x), z0 = floor_to_int(z);
    float fx = smooth(x - x0), fz = smooth(z - z0);
    float a = hash01(seed, x0, z0, salt), b = hash01(seed, x0 + 1, z0, salt);
    float c = hash01(seed, x0, z0 + 1, salt), d = hash01(seed, x0 + 1, z0 + 1, salt);
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fz);
}

float fbm2(uint32_t seed, float x, float z, uint32_t salt) {
    return vnoise2(seed, x, z, salt) * 0.6f + vnoise2(seed, x * 2.1f, z * 2.1f, salt + 1) * 0.3f +
           vnoise2(seed, x * 4.3f, z * 4.3f, salt + 2) * 0.1f;
}

int pmod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }

struct Style { float line_prob, door_prob; int seg_len; float pillar_prob; };
const Style ROOM_STYLES[4] = {
    {0.50f, 0.34f, 3, 0.03f},  // Rooms
    {0.10f, 0.50f, 4, 0.32f},  // Hall
    {0.62f, 0.30f, 2, 0.00f},  // Maze
    {0.70f, 0.14f, 6, 0.00f},  // Corridors (along the corridor; cross walls use Hall)
};
constexpr int DISTRICT_CELLS = 10;
constexpr int EXIT_REGION = 12;
constexpr int PIPE_GRID = 3;     // Level 2 tunnels
constexpr int OFFICE_GRID = 6;   // Level 4 cubicle blocks
constexpr int HOTEL_GRID = 5;    // Level 5 corridors
constexpr int SUBURB_GRID = 8;   // Level 9 blocks
constexpr int CITY_GRID = 10;    // Level 11 blocks
constexpr int ATTIC_GRID = 4;    // Level 19 lanes
constexpr int DOOR_GRID = 4;     // Level 21 hallways
constexpr int MALL_GRID = 8;     // Level 33 halls + stores
constexpr int AIR_GRID = 10;     // Level 36 concourses + gates
constexpr int RUIN_GRID = 7;     // Level 51 streets
constexpr int RUN_LEN = 68;      // Level !: cells of corridor to the exit
constexpr int POWER_GRID = 6;    // Level 3: two-wide corridors, machine blocks
constexpr int FROST_GRID = 10;   // Level 70: roads, towers, houses with igloos in the yard
constexpr int FROST_RIVER = 50;  // Level 70: the frozen river crosses every 50 cells
constexpr int TOWN_GRID = 8;     // Level 94: the town (same lots as the suburbs)
constexpr int TOWN_REGION = 32;  // Level 94: a town here and there among the hills
constexpr int RIVER_GRID = 40;   // 4Four: the river runs along z every 40 cells
constexpr int HOTEL_HALF = 12;   // Level 188: one hotel, 24x24 cells, centred on its atrium

// Level 4: each 6x6 block holds four 2x2 cubicles separated by aisles. Every cubicle
// has one opening (side + which of its two cells) and a desk in the opposite corner.
struct Cubicle { int side; int idx; }; // side: 0 west, 1 east, 2 north (-z), 3 south (+z)
Cubicle cubicle(uint32_t seed, int bx, int bz, int qx, int qz) {
    uint32_t h = hash3(seed, bx * 2 + qx, bz * 2 + qz, S_LOT);
    return {(int)(h % 4u), (int)((h >> 2) & 1u)};
}
// cell -> cubicle coordinates; false if the cell is an aisle
bool cell_cubicle(int c, int& b, int& q, int& l) {
    int m = pmod(c, OFFICE_GRID);
    if (m == 0 || m == 3) return false;
    b = floor_div(c, OFFICE_GRID);
    q = m >= 3 ? 1 : 0;
    l = m - (q ? 4 : 1);
    return true;
}


// Room-local frame: u runs along the door wall, v from the far wall (0) to the door wall (2 cells).
struct RoomFrame {
    float x0, z0, x1, z1;
    int side;
    float u_door;
    template <typename R> explicit RoomFrame(const R& r) : x0(r.x0), z0(r.z0), x1(r.x1), z1(r.z1), side(r.door_side) {
        float dx = (r.dcx + 0.5f) * CELL, dz = (r.dcz + 0.5f) * CELL;
        switch (side) {
            case 3: u_door = dx - x0; break;   // door on the south (+z) wall
            case 2: u_door = x1 - dx; break;   // north
            case 1: u_door = z1 - dz; break;   // east
            default: u_door = dz - z0; break;  // west
        }
    }
    Vec2 to_world(float u, float v) const {
        switch (side) {
            case 3: return {x0 + u, z0 + v};
            case 2: return {x1 - u, z1 - v};
            case 1: return {x0 + v, z1 - u};
            default: return {x1 - v, z0 + u};
        }
    }
    Vec2 dir(float du, float dv) const { // a direction in the local frame, in world axes
        Vec2 o = to_world(0, 0), p = to_world(du, dv);
        return p - o;
    }
};
// a building's own frame: u along the front wall, v from the back wall (row 0) to the front (row S-1)
void bldg_local(const Bldg& b, int cx, int cz, int& u, int& v) {
    const int S = b.w;
    switch (b.door_side) {
        case 3: u = cx - b.x0; v = cz - b.z0; break;
        case 2: u = b.x0 + S - 1 - cx; v = b.z0 + S - 1 - cz; break;
        case 1: u = b.z0 + S - 1 - cz; v = cx - b.x0; break;
        default: u = cz - b.z0; v = b.x0 + S - 1 - cx; break;
    }
}
// Inside walls, between two neighbouring cells of one building (local coords): 0 none, 1 a wall,
// 2 a doorway (a gap with a door hung in it).
int bldg_partition(const Bldg& b, int ua, int va, int ub, int vb) {
    if (va > vb || ua > ub) { std::swap(ua, ub); std::swap(va, vb); }
    const bool across_v = ua == ub; // the cells are one behind the other: the wall runs along u
    switch (b.kind) {
        case 0: // shop: a storeroom along the back
            if (across_v && va == 0) return ua == 2 ? 2 : 1;
            return 0;
        case 1: // flat: a bedroom and a bathroom along the back
            if (across_v && va == 0) return (ua == 0 || ua == 2) ? 2 : 1;
            if (!across_v && va == 0 && ua == 1) return 1;
            return 0;
        case 2: // office: a manager's office in the back corner
            if (across_v && va == 0 && ua == 0) return 2;
            if (!across_v && va == 0 && ua == 0) return 1;
            return 0;
        case 3: // diner: the kitchen along the back
            if (across_v && va == 0) return ua == 1 ? 2 : 1;
            return 0;
        default: // tower: the lobby at the front, two flats behind it
            if (across_v && va == 2) return (ua == 1 || ua == 4) ? 2 : 1;
            if (!across_v && va <= 2 && ua == 2) return 1;
            return 0;
    }
}
} // namespace

const LevelInfo& level_info(LevelId id) { return LEVELS[(int)id < (int)LevelId::COUNT ? (int)id : 0]; }
const ItemInfo& item_info(ItemType t) { return ITEMS[(int)t < ITEM_COUNT ? (int)t : 0]; }
const DiffInfo& diff_info(Difficulty d) { return DIFFS[(int)d < (int)Difficulty::COUNT ? (int)d : 1]; }

bool difficulty_from_name(const char* s, Difficulty& out) {
    std::string v;
    for (const char* p = s; *p; p++) if (std::isalnum((unsigned char)*p)) v += (char)std::toupper((unsigned char)*p);
    for (int i = 0; i < (int)Difficulty::COUNT; i++)
        if (v == DIFFS[i].name || v == std::to_string(i)) { out = (Difficulty)i; return true; }
    if (v == "NORMAL") { out = Difficulty::Medium; return true; }
    return false;
}

const char* level_label(LevelId id) {
    static char buf[(int)LevelId::COUNT][48];
    char* b = buf[(int)id];
    if (!b[0]) {
        const LevelInfo& li = level_info(id);
        std::strncat(b, li.code, 47);
        std::strncat(b, " - ", 47 - std::strlen(b));
        std::strncat(b, li.name, 47 - std::strlen(b));
    }
    return b;
}

bool level_from_number(const char* s, LevelId& out) {
    if (std::strchr(s, '!')) { out = LevelId::LRun; return true; }
    std::string v;
    for (const char* p = s; *p; p++) if (std::isalnum((unsigned char)*p)) v += (char)std::toupper((unsigned char)*p);
    if (v.rfind("LEVEL", 0) == 0) v = v.substr(5);
    for (int i = 0; i < (int)LevelId::COUNT; i++)
        if (v == LEVELS[i].number) { out = (LevelId)i; return true; }
    if (v == "RUN") { out = LevelId::LRun; return true; }
    if (v == "TSG" || v == "TH3SH4DYGR3Y" || v == "SHADYGREY" || v == "GREY" || v == "TSG0" || v == "ZERO") { out = LevelId::TSG0; return true; }
    if (v == "TSG1" || v == "ONE" || v == "MANSION") { out = LevelId::TSG1; return true; }
    if (v == "TSG2" || v == "TWO") { out = LevelId::TSG2; return true; }
    if (v == "TSG3" || v == "THREE") { out = LevelId::TSG3; return true; }
    if (v == "TSG4" || v == "FOUR") { out = LevelId::TSG4; return true; }
    if (v == "LOSTHOPE" || v == "L0STH0P3") { out = LevelId::TSGHope; return true; }
    if (v == "TOOFAR" || v == "2F4R") { out = LevelId::TSGFar; return true; }
    if (v == "TH33ND") { out = LevelId::TSGEnd; return true; }
    if (v == "THEEND" || v == "LAST" || v == "FINAL" || v == "BOOKSHOP") { out = LevelId::TheEnd; return true; }
    if (v == "LOBBY" || v == "MEG" || v == "BASE") { out = LevelId::Hub; return true; }
    return false;
}

// ------------------------------------------------------------------ room-style levels
World::District World::district(int cx, int cz) const {
    // offset by half a district so spawn (cell 0,0) sits in the middle of one
    int dx = floor_div(cx + DISTRICT_CELLS / 2, DISTRICT_CELLS), dz = floor_div(cz + DISTRICT_CELLS / 2, DISTRICT_CELLS);
    if (dx == 0 && dz == 0) return District::Rooms;
    float r = hash01(seed_, dx, dz, S_DISTRICT);
    if (id_ == LevelId::LFun) return r < 0.75f ? District::Rooms : District::Hall; // party rooms
    if (id_ == LevelId::TSGFar) return District::Corridors;                             // 2 F4R: long dark halls
    if (id_ == LevelId::TSGHope) return r < 0.7f ? District::Maze : District::Corridors;   // the last maze
    if (r < 0.50f) return District::Rooms;
    if (r < 0.68f) return District::Hall;
    if (r < 0.84f) return District::Maze;
    return District::Corridors;
}

bool World::edge_wall_rooms(int line, int along, bool vertical) const {
    int cx = vertical ? line : along, cz = vertical ? along : line;
    District d = district(cx, cz);
    const Style* st = &ROOM_STYLES[(int)d];
    if (d == District::Corridors) {
        int dx = floor_div(cx + DISTRICT_CELLS / 2, DISTRICT_CELLS), dz = floor_div(cz + DISTRICT_CELLS / 2, DISTRICT_CELLS);
        bool corridor_vertical = hash3(seed_, dx, dz, S_ORIENT) & 1;
        if (corridor_vertical != vertical) st = &ROOM_STYLES[(int)District::Hall];
    }
    uint32_t dir = vertical ? 0u : 1000u;
    int jitter = (int)(hash3(seed_, line, 0, S_JITTER + dir) % (uint32_t)st->seg_len);
    int seg = floor_div(along + jitter, st->seg_len);
    if (hash01(seed_, line, seg, S_LINE + dir) >= st->line_prob) return false;
    return hash01(seed_, line, along, S_DOOR + dir) >= st->door_prob;
}

bool World::edge_wall_l1(int line, int along, bool vertical) const {
    // long, sparse concrete partition walls in a huge warehouse
    uint32_t dir = vertical ? 0u : 1000u;
    int jitter = (int)(hash3(seed_, line, 0, S_JITTER + dir) % 6u);
    int seg = floor_div(along + jitter, 6);
    if (hash01(seed_, line, seg, S_LINE + dir) >= 0.22f) return false;
    return hash01(seed_, line, along, S_DOOR + dir) >= 0.22f;
}

bool World::edge_wall_l37(int line, int along, bool vertical) const {
    // tiled partitions every 4 cells with wide archways: big airy pool halls
    if (pmod(line, 4) != 0) return false;
    uint32_t dir = vertical ? 0u : 1000u;
    int seg = floor_div(along, 3);
    if (hash01(seed_, line, seg, S_LINE + dir) >= 0.65f) return false;
    return hash01(seed_, line, along, S_DOOR + dir) >= 0.40f;
}

bool World::office_wall(int line, int along, bool vertical) const {
    int b, q, l;
    if (!cell_cubicle(along, b, q, l)) return false; // aisles have no walls across them
    int m = pmod(line, OFFICE_GRID);
    int ob, oq, side;
    if (m == 1) { ob = floor_div(line, OFFICE_GRID); oq = 0; side = 0; }
    else if (m == 3) { ob = floor_div(line, OFFICE_GRID); oq = 0; side = 1; }
    else if (m == 4) { ob = floor_div(line, OFFICE_GRID); oq = 1; side = 0; }
    else if (m == 0) { ob = floor_div(line, OFFICE_GRID) - 1; oq = 1; side = 1; }
    else return false;
    Cubicle c = vertical ? cubicle(seed_, ob, b, oq, q) : cubicle(seed_, b, ob, q, oq);
    int want = vertical ? side : side + 2;
    return !(c.side == want && c.idx == l);
}

// ------------------------------------------------------------------ geometry
bool World::solid(int cx, int cz) const {
    switch (id_) {
        case LevelId::L1: // stacks of supply crates scattered through the warehouse
            if (near_spawn(cx, cz, 2)) return false;
            return hash01(seed_, cx, cz, S_SOLID) < 0.05f;
        case LevelId::L2: { // maintenance tunnels in a 3-cell grid, the occasional boiler room
            int mx = pmod(cx, PIPE_GRID), mz = pmod(cz, PIPE_GRID);
            if (near_spawn(cx, cz, 1)) return false;
            if (mx == 0 || mz == 0) {
                if (mx == 0 && mz == 0) return false;
                return hash01(seed_, cx, cz, S_SOLID) < 0.05f; // dead ends
            }
            return hash01(seed_, floor_div(cx, PIPE_GRID), floor_div(cz, PIPE_GRID), S_LOT) >= 0.12f;
        }
        case LevelId::L4: { // a desk in the corner of every cubicle
            int bx, qx, lx, bz, qz, lz;
            if (!cell_cubicle(cx, bx, qx, lx) || !cell_cubicle(cz, bz, qz, lz)) return false;
            Cubicle c = cubicle(seed_, bx, bz, qx, qz);
            int ox = c.side == 0 ? 0 : c.side == 1 ? 1 : c.idx;
            int oz = c.side == 2 ? 0 : c.side == 3 ? 1 : c.idx;
            return lx == 1 - ox && lz == 1 - oz;
        }
        case LevelId::L5: case LevelId::TSG1: {
            int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
            if (mx == 0 || mz == 0) return false; // corridors stay clear: every room door opens onto one
            return hotel_block(floor_div(cx, HOTEL_GRID), floor_div(cz, HOTEL_GRID)) == 0;
        }
        case LevelId::L8: {
            if (near_spawn(cx, cz, 2)) return false;
            float n = fbm2(seed_, cx / 5.0f, cz / 5.0f, S_NOISE);
            float worm = fbm2(seed_, cx / 9.0f, cz / 9.0f, S_NOISE + 10);
            return n > 0.50f && std::fabs(worm - 0.5f) >= 0.05f; // winding tunnels link the caverns
        }
        case LevelId::L9: case LevelId::L94: case LevelId::L70: { // houses: locked ones are solid, the rest you can walk into
            int lx, lz, bx, bz;
            int lot = house_lot(cx, cz, lx, lz, bx, bz);
            if (lot == 2) return true;
            if (lot == 1) return false;
            if (id_ == LevelId::L70) {
                if (ice(cx, cz)) return false;
                int mx = pmod(cx, FROST_GRID), mz = pmod(cz, FROST_GRID);
                if (mx < 2 || mz < 2) return false; // roads
                bx = floor_div(cx, FROST_GRID); bz = floor_div(cz, FROST_GRID);
                if (!(bx == 0 && bz == 0) && hash01(seed_, bx, bz, S_LOT) < 0.42f) { // a tower (some have a lobby you can walk into)
                    Bldg tb;
                    if (building(cx, cz, tb)) return false;
                    return mx >= 3 && mx <= 8 && mz >= 3 && mz <= 8;
                }
                return mx == 8 && mz == 7 && hash01(seed_, bx, bz, S_LOT + 3) < 0.6f; // an igloo in the yard
            }
            return false; // (Motion's hills are ground you walk over)
        }
        case LevelId::L188: return court_cell(cx, cz) == 4; // stairwell cores and locked rooms
        case LevelId::TheEnd: { // THE END: an endless, emptied bookshop. Rows of shelves, wide aisles
            if (near_spawn(cx, cz, 2)) return false;
            int mz = pmod(cz, 4);
            if (mz != 1 && mz != 2) return false;
            int seg = floor_div(cx, 6);
            if (pmod(cx, 6) == 0) return false;                                  // gaps between the runs
            if (hash01(seed_, seg, floor_div(cz, 4), S_SOLID + 5) > 0.62f) return false; // open floor here
            return mz == 1 || hash01(seed_, seg, floor_div(cz, 4), S_SOLID + 6) < 0.5f;   // single or double rows
        }
        case LevelId::Hub: { // the base: one big concrete hall with a few columns of crates
            if (std::abs(cx) > 7 || std::abs(cz) > 7) return true;
            return (std::abs(cx) == 5 && std::abs(cz) == 5);
        }
        case LevelId::TSG0: case LevelId::TSG2: { // jungle / snowy forest: big trunks everywhere, the shack at spawn
            if (cx >= -3 && cx <= 3 && cz >= -3 && cz <= 3) return false;
            return hash01(seed_, cx, cz, S_SOLID) < (id_ == LevelId::TSG0 ? 0.20f : 0.13f);
        }
        case LevelId::TSG3: // the odd rock on an island
            if (near_spawn(cx, cz, 3)) return false;
            return floor_mat(cx, cz) == 0 && hash01(seed_, cx, cz, S_SOLID) < 0.04f;
        case LevelId::TSG4: { // the glitching city on both banks of the river (floating buildings: you can walk under them)
            float lift;
            int qx, qz, kind, ds;
            if (city_room(cx, cz, qx, qz, kind, ds)) return false; // a shop or a flat you can walk into
            return city4_building(cx, cz) && !floating_block(cx, cz, lift);
        }
        case LevelId::L11: {
            int mx = pmod(cx, CITY_GRID), mz = pmod(cz, CITY_GRID);
            if (mx < 3 || mx > 8 || mz < 3 || mz > 8) return false;
            int qx, qz, kind, ds;
            if (city_room(cx, cz, qx, qz, kind, ds)) return false;
            return hash01(seed_, floor_div(cx, CITY_GRID), floor_div(cz, CITY_GRID), S_LOT) >= 0.10f;
        }
        case LevelId::L3: { // machine rooms between two-wide corridors
            int mx = pmod(cx, POWER_GRID), mz = pmod(cz, POWER_GRID);
            if (mx <= 1 || mz <= 1) return false;
            int bx = floor_div(cx, POWER_GRID), bz = floor_div(cz, POWER_GRID);
            if (hash01(seed_, bx, bz, S_LOT) < 0.5f) return true;           // a sealed switch room
            return (mx == 3 || mx == 4) && (mz == 3 || mz == 4);            // a hall around a transformer
        }
        case LevelId::L10: { // barns, sheds and hay stacks in an endless field
            if (near_spawn(cx, cz, 2)) return false;
            int mx = pmod(cx, 12), mz = pmod(cz, 12);
            if (mx == 0 || mz == 0) return false; // dirt tracks
            int bx = floor_div(cx, 12), bz = floor_div(cz, 12);
            if (hash01(seed_, bx, bz, S_LOT) < 0.4f && mx >= 4 && mx <= 6 && mz >= 4 && mz <= 5) return true; // a barn
            if (hash01(seed_, bx, bz, S_LOT) < 0.4f && mx == 8 && mz == 4) return true;                          // its silo
            return hash01(seed_, cx, cz, S_SOLID) < 0.03f;                                                       // a shed / hay
        }
        case LevelId::L13: {
            int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
            if (mx == 0 || mz == 0) return false;
            return hotel_block(floor_div(cx, HOTEL_GRID), floor_div(cz, HOTEL_GRID)) == 0;
        }
        case LevelId::L7: // rocks rising out of the water (not near the drowned house)
            if (near_spawn(cx, cz, 5)) return false;
            return hash01(seed_, cx, cz, S_SOLID) < 0.03f;
        case LevelId::L19: { // long attic lanes between walls of beams, junk everywhere
            int mz = pmod(cz, ATTIC_GRID);
            if (near_spawn(cx, cz, 1)) return false;
            if (mz == 3) return hash01(seed_, cx, floor_div(cz, ATTIC_GRID), S_DOOR) >= 0.16f; // gaps between lanes
            return mz != 1 && hash01(seed_, cx, cz, S_SOLID) < 0.08f;                         // boxes, junk
        }
        case LevelId::L21: { // hallways lined with numbered doors (every block face is doors)
            int mx = pmod(cx, DOOR_GRID), mz = pmod(cz, DOOR_GRID);
            if (mz == 0) return false;                                             // long halls along x
            if (mx == 0) return hash01(seed_, floor_div(cx, DOOR_GRID), floor_div(cz, DOOR_GRID), S_LOT) >= 0.4f; // some cross halls
            bool north;
            return !door_room_block(floor_div(cx, DOOR_GRID), floor_div(cz, DOOR_GRID), north); // a room behind one of the doors
        }
        case LevelId::L22: { // parked cars in bays, stairwell cores
            if (near_spawn(cx, cz, 1)) return false;
            int rx = floor_div(cx, 8), rz = floor_div(cz, 8);
            if (pmod(cx, 8) >= 5 && pmod(cx, 8) <= 6 && pmod(cz, 8) >= 5 && pmod(cz, 8) <= 6 && hash01(seed_, rx, rz, S_LOT) < 0.55f) return true; // stairwell
            int mz = pmod(cz, 4);
            if (mz == 1 || mz == 2) return hash01(seed_, cx, cz, S_SOLID) < 0.5f && pmod(cx, 3) != 0; // a car in the bay
            return false;
        }
        case LevelId::L24: { // the odd great boulder (the rest are props: rounded rocks of every size)
            if (near_spawn(cx, cz, 3)) return false;
            return hash01(seed_, cx, cz, S_SOLID) < 0.012f;
        }
        case LevelId::L33: { // shop blocks between wide galleries
            int mx = pmod(cx, MALL_GRID), mz = pmod(cz, MALL_GRID);
            if (mx < 3 || mz < 3) return false; // the galleries (fountains, planters and escalators are props now)
            int bx = floor_div(cx, MALL_GRID), bz = floor_div(cz, MALL_GRID);
            bool open = hash01(seed_, bx, bz, S_LOT) < 0.65f;
            if (!open) return true; // shuttered store
            bool ring = mx == 3 || mx == 7 || mz == 3 || mz == 7;
            if (ring) return !((mx == 5 && mz == 3) || (mz == 5 && mx == 3)); // two doorways
            return (mx == 4 || mx == 6) && mz != 5;                            // shelves inside
        }
        case LevelId::L36: { // concourses, gate lounges with rows of seats, window walls
            int mx = pmod(cx, AIR_GRID), mz = pmod(cz, AIR_GRID);
            if (mz <= 3) return false;                       // concourse
            if (mz == 9) return mx != 5;                     // windows, with a jet bridge through
            if ((mz == 5 || mz == 7) && mx >= 2 && mx <= 8 && mx != 5) return true; // seat rows
            if (mz == 4 && mx == 1) return true;             // gate desk
            return false;
        }
        case LevelId::L51: { // ruined stone city blocks between streets
            int mx = pmod(cx, RUIN_GRID), mz = pmod(cz, RUIN_GRID);
            if (mx < 2 || mz < 2 || near_spawn(cx, cz, 1)) return false;
            int bx = floor_div(cx, RUIN_GRID), bz = floor_div(cz, RUIN_GRID);
            if (hash01(seed_, bx, bz, S_LOT) < 0.12f) return false;       // a plaza
            if (hash01(seed_, bx, bz, S_LOT + 1) < 0.10f) return true;    // a stepped temple
            return hash01(seed_, cx, cz, S_SOLID) >= 0.18f;                // buildings, some fallen
        }
        case LevelId::LRun: { // one corridor, three wide, to the exit far ahead
            if (cx < -1 || cx > 1 || cz > 2 || cz < -RUN_LEN) return true;
            return run_block(cx, cz);
        }
        default: return false;
    }
}

float World::block_h(int cx, int cz) const {
    switch (id_) {
        case LevelId::L1: return 1.2f * (1 + (int)(hash3(seed_, cx, cz, S_HEIGHT) % 3u)); // 1-3 crates tall
        case LevelId::L4: return 0.76f;  // desk
        case LevelId::L9: case LevelId::L94: {
            if (id_ == LevelId::L94 && hill(cx, cz)) { // rolling: higher in the middle of a hill
                float n = fbm2(seed_, cx / 3.5f, cz / 3.5f, S_NOISE + 50);
                return 1.4f + clampf((n - 0.6f) / 0.3f, 0, 1) * 3.4f;
            }
            return house_h();
        }
        case LevelId::L70: {
            int lx, lz, bx, bz;
            if (house_lot(cx, cz, lx, lz, bx, bz)) return house_h();
            int mx = pmod(cx, FROST_GRID), mz = pmod(cz, FROST_GRID);
            if (mx == 8 && mz == 7) return 1.8f; // igloo
            float r = hash01(seed_, floor_div(cx, FROST_GRID), floor_div(cz, FROST_GRID), S_HEIGHT);
            return 12.0f + r * r * 42.0f;
        }
        case LevelId::L188: return info().ceil_h;
        case LevelId::Hub: return info().ceil_h;
        case LevelId::TheEnd: return 1.9f; // bookshelves
        case LevelId::TSG0: case LevelId::TSG2: return 9.0f;  // a trunk: blocks the view
        case LevelId::TSG3: return 1.4f + hash01(seed_, cx, cz, S_HEIGHT) * 1.4f;
        case LevelId::TSG4: {
            int qx = city_quarter(cx), qz = city_quarter(cz);
            float r = hash01(seed_, qx, qz, S_HEIGHT);
            return 16.0f + r * r * 50.0f;
        }
        case LevelId::L7: return 2.6f + hash01(seed_, cx, cz, S_HEIGHT) * 2.0f;
        case LevelId::L3: {
            int mx = pmod(cx, POWER_GRID), mz = pmod(cz, POWER_GRID);
            bool hall = hash01(seed_, floor_div(cx, POWER_GRID), floor_div(cz, POWER_GRID), S_LOT) >= 0.5f;
            return hall && (mx == 3 || mx == 4) && (mz == 3 || mz == 4) ? 2.1f : info().ceil_h;
        }
        case LevelId::L10: {
            int mx = pmod(cx, 12), mz = pmod(cz, 12);
            bool farm = hash01(seed_, floor_div(cx, 12), floor_div(cz, 12), S_LOT) < 0.4f;
            if (farm && mx >= 4 && mx <= 6 && mz >= 4 && mz <= 5) return 5.5f; // barn
            if (farm && mx == 8 && mz == 4) return 9.0f;                        // silo
            return hash01(seed_, cx, cz, S_HEIGHT) < 0.5f ? 2.5f : 3.2f;        // hay stack / shed
        }
        case LevelId::L19: return pmod(cz, ATTIC_GRID) == 3 ? info().ceil_h : 0.5f + 0.8f * hash01(seed_, cx, cz, S_HEIGHT);
        case LevelId::L22: {
            int mz = pmod(cz, 4);
            return (mz == 1 || mz == 2) && !(pmod(cx, 8) >= 5 && pmod(cx, 8) <= 6 && pmod(cz, 8) >= 5 && pmod(cz, 8) <= 6) ? 1.35f : info().ceil_h;
        }
        case LevelId::L24: return 1.8f + hash01(seed_, cx, cz, S_HEIGHT) * 2.2f; // a boulder
        case LevelId::L33: {
            int mx = pmod(cx, MALL_GRID), mz = pmod(cz, MALL_GRID);
            bool open = hash01(seed_, floor_div(cx, MALL_GRID), floor_div(cz, MALL_GRID), S_LOT) < 0.65f;
            bool ring = mx == 3 || mx == 7 || mz == 3 || mz == 7;
            return (open && !ring) ? 1.6f : info().ceil_h;      // shelves / shop front
        }
        case LevelId::L36: {
            int mz = pmod(cz, AIR_GRID);
            if (mz == 9) return info().ceil_h;
            return mz == 4 ? 1.1f : 0.5f;                      // desk / seats
        }
        case LevelId::L51: {
            int bx = floor_div(cx, RUIN_GRID), bz = floor_div(cz, RUIN_GRID);
            int mx = pmod(cx, RUIN_GRID), mz = pmod(cz, RUIN_GRID);
            if (hash01(seed_, bx, bz, S_LOT + 1) < 0.10f) { // temple steps rise toward the middle
                int ring = std::min(std::min(mx - 2, 6 - mx), std::min(mz - 2, 6 - mz));
                return 1.2f + ring * 1.5f;
            }
            float r = hash01(seed_, cx, cz, S_HEIGHT);
            return r < 0.2f ? 0.7f : 2.6f + r * 3.0f; // rubble or a building
        }
        case LevelId::L11: {
            int qx = city_quarter(cx), qz = city_quarter(cz); // each 3x3 quarter is one building
            float r = hash01(seed_, qx, qz, S_HEIGHT);
            return 14.0f + r * r * 70.0f;
        }
        default: return info().ceil_h;
    }
}

bool World::wall_x(int gx, int cz) const {
    switch (id_) {
        case LevelId::L0: case LevelId::L6: case LevelId::LFun: case LevelId::TSGFar: case LevelId::TSGHope:
            if (near_spawn(gx, cz, 1) && gx != -1) return false; // keep the spawn room open
            return edge_wall_rooms(gx, cz, true);
        case LevelId::L7: // the drowned house's walls, and broken walls standing in the sea
            if (near_spawn(gx, cz, 1) && gx != -1) return false;
            if (std::abs(gx) <= 4 && std::abs(cz) <= 4) return edge_wall_rooms(gx, cz, true);
            return hash01(seed_, gx, cz, S_LINE) < 0.035f;
        case LevelId::L22: // concrete dividing walls here and there
            if (near_spawn(gx, cz, 2)) return false;
            return edge_wall_l1(gx, cz, true);
        case LevelId::L36: // walls between the gate lounges
            return pmod(gx, AIR_GRID) == 0 && pmod(cz, AIR_GRID) >= 4 && pmod(cz, AIR_GRID) <= 8 && pmod(cz, AIR_GRID) != 6;
        case LevelId::L1: case LevelId::TheEnd:
            if (near_spawn(gx, cz, 2)) return false;
            return edge_wall_l1(gx, cz, true);
        case LevelId::L4: return office_wall(gx, cz, true);
        case LevelId::L21: return door_room_wall(gx, cz, true);
        case LevelId::L5: case LevelId::TSG1: case LevelId::L13: case LevelId::L9: case LevelId::L94: case LevelId::L188: return hotel_wall(gx, cz, true);
        case LevelId::L70: return hotel_wall(gx, cz, true) || city_wall(gx, cz, true);
        case LevelId::L11: case LevelId::TSG4: return city_wall(gx, cz, true);
        case LevelId::TSG0: case LevelId::TSG2: // the shack / outpost around spawn (west and east walls)
            return (gx == -1 || gx == 2) && cz >= -1 && cz <= 1 && !(gx == -1 && cz == 0 && id_ == LevelId::TSG0);
        case LevelId::L37:
            if (near_spawn(gx, cz, 2)) return false;
            return edge_wall_l37(gx, cz, true);
        default: return false;
    }
}

bool World::wall_z(int cx, int gz) const {
    switch (id_) {
        case LevelId::L0: case LevelId::L6: case LevelId::LFun: case LevelId::TSGFar: case LevelId::TSGHope:
            if (near_spawn(cx, gz, 1) && gz != -1) return false;
            return edge_wall_rooms(gz, cx, false);
        case LevelId::L7:
            if (near_spawn(cx, gz, 1) && gz != -1) return false;
            if (std::abs(cx) <= 4 && std::abs(gz) <= 4) return edge_wall_rooms(gz, cx, false);
            return hash01(seed_, cx, gz, S_LINE + 1000) < 0.035f;
        case LevelId::L22:
            if (near_spawn(cx, gz, 2)) return false;
            return edge_wall_l1(gz, cx, false);
        case LevelId::L1: case LevelId::TheEnd:
            if (near_spawn(cx, gz, 2)) return false;
            return edge_wall_l1(gz, cx, false);
        case LevelId::L4: return office_wall(gz, cx, false);
        case LevelId::L21: return door_room_wall(gz, cx, false);
        case LevelId::L5: case LevelId::TSG1: case LevelId::L13: case LevelId::L9: case LevelId::L94: case LevelId::L188: return hotel_wall(gz, cx, false);
        case LevelId::L70: return hotel_wall(gz, cx, false) || city_wall(gz, cx, false);
        case LevelId::L11: case LevelId::TSG4: return city_wall(gz, cx, false);
        case LevelId::TSG0: case LevelId::TSG2: // north and south walls, the doorway (its door is gone) facing south
            return (gz == -1 || gz == 2) && cx >= -1 && cx <= 1 && !(gz == 2 && cx == 0);
        case LevelId::L37:
            if (near_spawn(cx, gz, 2)) return false;
            return edge_wall_l37(gz, cx, false);
        default: return false;
    }
}

bool World::post(int gx, int gz) const {
    switch (id_) {
        case LevelId::L0: case LevelId::L1: case LevelId::L4: case LevelId::L5: case LevelId::L6: case LevelId::L37: case LevelId::LFun: case LevelId::TheEnd:
        case LevelId::L7: case LevelId::L13: case LevelId::L21: case LevelId::L22: case LevelId::L36: case LevelId::TSGFar: case LevelId::TSG1: case LevelId::TSGHope:
        case LevelId::L9: case LevelId::L70: case LevelId::L94: case LevelId::TSG0: case LevelId::TSG2: case LevelId::L188:
        case LevelId::L11: case LevelId::TSG4:
            return wall_x(gx, gz - 1) || wall_x(gx, gz) || wall_z(gx - 1, gz) || wall_z(gx, gz);
        default: return false;
    }
}

bool World::pillar(int gx, int gz) const {
    switch (id_) {
        case LevelId::L22: { // the car park's grid of columns
            if (pmod(gx, 3) != 0 || pmod(gz, 4) != 0 || near_spawn(gx, gz, 1)) return false;
            for (int a = -1; a <= 0; a++)
                for (int b = -1; b <= 0; b++)
                    if (solid(gx + a, gz + b) && block_h(gx + a, gz + b) > 2.0f) return false;
            return !post(gx, gz);
        }
        case LevelId::L36: return pmod(gx, 5) == 0 && pmod(gz, AIR_GRID) == 0 && !near_spawn(gx, gz, 1);
        case LevelId::L33: { // the teal columns holding up the galleries
            int mx = pmod(gx, MALL_GRID), mz = pmod(gz, MALL_GRID);
            if (near_spawn(gx, gz, 1)) return false;
            bool edge_z = (mz == 0 || mz == 3) && (gx & 1) == 0, edge_x = (mx == 0 || mx == 3) && (gz & 1) == 0;
            if (!edge_z && !edge_x) return false;
            if (mx > 3 && mz > 3) return false; // inside a shop block
            return !post(gx, gz);
        }
        case LevelId::Hub: return (gx == -3 || gx == 3) && (gz == -3 || gz == 3);
        case LevelId::TheEnd: { // big square yellow pillars, now and then
            if (pmod(gx, 6) != 3 || pmod(gz, 8) != 0 || near_spawn(gx, gz, 1)) return false;
            for (int a = -1; a <= 0; a++) for (int b = -1; b <= 0; b++) if (solid(gx + a, gz + b)) return false;
            return !post(gx, gz) && hash01(seed_, gx, gz, S_PILLAR) < 0.7f;
        }
        case LevelId::L51: { // colonnades along the streets
            int mx = pmod(gx, RUIN_GRID), mz = pmod(gz, RUIN_GRID);
            if (near_spawn(gx, gz, 1)) return false;
            return ((mx == 1 && (gz & 1) == 0) || (mz == 1 && (gx & 1) == 0)) && hash01(seed_, gx, gz, S_PILLAR) < 0.6f;
        }
        case LevelId::L0: case LevelId::L6: case LevelId::LFun: case LevelId::TSGFar: {
            if (gx >= -1 && gx <= 2 && gz >= -1 && gz <= 2) return false;
            float p = ROOM_STYLES[(int)district(gx, gz)].pillar_prob;
            if (p <= 0 || hash01(seed_, gx, gz, S_PILLAR) >= p) return false;
            return !post(gx, gz);
        }
        case LevelId::L1: {
            if (pmod(gx, 3) != 0 || pmod(gz, 3) != 0) return false;
            if (hash01(seed_, gx, gz, S_PILLAR) < 0.12f) return false;
            for (int a = -1; a <= 0; a++)
                for (int b = -1; b <= 0; b++)
                    if (solid(gx + a, gz + b)) return false;
            return !post(gx, gz);
        }
        case LevelId::L5: case LevelId::TSG1: case LevelId::L13: {
            int mx = pmod(gx, HOTEL_GRID), mz = pmod(gz, HOTEL_GRID);
            if (mx == 0 || mz == 0) return false;
            return hotel_block(floor_div(gx, HOTEL_GRID), floor_div(gz, HOTEL_GRID)) == 1 && ((mx + mz) % 2 == 0);
        }
        case LevelId::L37: { // great round tiled columns standing in the water, one in the middle of most halls
            if (pmod(gx, 4) != 2 || pmod(gz, 4) != 2 || near_spawn(gx, gz, 2)) return false;
            if (pool_depth(gx * CELL, gz * CELL) > 0.5f) return false; // (not in a deep pool)
            return !post(gx, gz) && hash01(seed_, gx, gz, S_PILLAR) < 0.7f;
        }
        default: return false;
    }
}

bool World::pole(int cx, int cz) const {
    bool street = id_ == LevelId::L9 || id_ == LevelId::L7 || id_ == LevelId::L51 || id_ == LevelId::L70 || id_ == LevelId::L94 || id_ == LevelId::TSG4 ||
                  (id_ == LevelId::L188 && court_cell(cx, cz) == 0);
    if (street && houses() && house(cx, cz)) return false; // a lamp indoors hangs from the ceiling
    return street && light(cx, cz) != LightState::None &&
           !(id_ == LevelId::L7 && std::abs(cx) <= 4 && std::abs(cz) <= 4); // the drowned house has ceiling lamps
}

LightState World::light(int cx, int cz) const {
    if (solid(cx, cz)) return LightState::None;
    float r = hash01(seed_, cx, cz, S_LIGHTSTATE);
    switch (id_) {
        case LevelId::TSGFar: { // 2 F4R: red emergency lamps, far apart, most of them dead
            if (pmod(cx, 3) != 0 || pmod(cz, 3) != 0) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::Flicker;
            if (r < 0.55f) return LightState::Dead;
            return r < 0.8f ? LightState::Flicker : LightState::On;
        }
        case LevelId::TSGHope: return LightState::None;
        case LevelId::L7: {
            if (std::abs(cx) <= 4 && std::abs(cz) <= 4) { // lamps in the drowned house
                if ((cx & 1) || (cz & 1)) return LightState::None;
                return r < 0.25f ? LightState::Flicker : LightState::On;
            }
            return r < 0.012f ? (r < 0.004f ? LightState::Flicker : LightState::On) : LightState::None; // lamp posts in the water
        }
        case LevelId::L19: {
            if (pmod(cz, ATTIC_GRID) != 1 || pmod(cx, 3) != 0) return LightState::None;
            if (near_spawn(cx, cz, 2)) return LightState::On;
            if (r < 0.45f) return LightState::Dead;
            return r < 0.55f ? LightState::Flicker : LightState::On;
        }
        case LevelId::L21: {
            int rbx, rbz;
            bool rn;
            if (door_room(cx, cz, rbx, rbz, rn)) { // one light in the middle of each room
                if (pmod(cx, DOOR_GRID) != 2 || pmod(cz, DOOR_GRID) != 2) return LightState::None;
                return r < 0.2f ? LightState::Dead : (r < 0.3f ? LightState::Flicker : LightState::On);
            }
            if (((cx + cz) & 1) != 0) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (r < 0.12f) return LightState::Dead;
            return r < 0.18f ? LightState::Flicker : LightState::On;
        }
        case LevelId::L22: {
            if (pmod(cz, 4) != 0 && pmod(cz, 4) != 3) return LightState::None;
            if ((cx & 1) != 0) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (fbm2(seed_, cx / 10.0f, cz / 10.0f, S_LIGHT) < 0.30f) return LightState::Dead;
            if (r < 0.25f) return LightState::Dead;
            return r < 0.35f ? LightState::Flicker : LightState::On;
        }
        case LevelId::L33: {
            int mx = pmod(cx, MALL_GRID), mz = pmod(cz, MALL_GRID);
            if (mx == 5 && mz == 5) return r < 0.5f ? LightState::Dead : LightState::Flicker; // a shop, its lights half dead
            if (!(mx < 3 || mz < 3) || ((cx + cz) & 1)) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (r < 0.15f) return LightState::Dead;
            return r < 0.22f ? LightState::Flicker : LightState::On;
        }
        case LevelId::L36: {
            if ((cx & 1) || (cz & 1)) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (r < 0.08f) return LightState::Dead;
            return r < 0.12f ? LightState::Flicker : LightState::On;
        }
        case LevelId::L51: { // braziers along the streets
            int mx = pmod(cx, RUIN_GRID), mz = pmod(cz, RUIN_GRID);
            if (!((mx == 0 && pmod(cz, 3) == 0) || (mz == 0 && pmod(cx, 3) == 0)) || (mx == 0 && mz == 0)) return LightState::None;
            if (near_spawn(cx, cz, 3)) return LightState::Flicker;
            if (r < 0.35f) return LightState::Dead;
            return LightState::Flicker; // fire never holds still
        }
        case LevelId::L3: {
            int mx = pmod(cx, POWER_GRID), mz = pmod(cz, POWER_GRID);
            bool corridor = mx <= 1 || mz <= 1;
            if (corridor ? (((cx + cz) & 1) || !(mx == 0 || mz == 0)) : (mx != 2 || mz != 2)) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (r < 0.35f) return LightState::Dead;
            return r < 0.55f ? LightState::Flicker : LightState::On;
        }
        case LevelId::LRun: // alarm lights all the way down
            return (cx == 0 && (cz & 1) == 0) ? LightState::On : LightState::None;
        case LevelId::L0: case LevelId::LFun: {
            District d = district(cx, cz);
            bool slot = d == District::Hall ? ((cx + cz) & 1) == 0 : ((cx & 1) == 0 && (cz & 1) == 0);
            if (!slot) return LightState::None;
            if (cx == 0 && cz == 0) return LightState::On;
            float dead = id_ == LevelId::LFun ? 0.03f : (d == District::Maze ? 0.30f : 0.09f);
            if (r < dead) return LightState::Dead;
            if (r < dead + 0.05f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L6: // every panel is dead. bring your own light.
            return ((cx & 1) == 0 && (cz & 1) == 0) ? LightState::Dead : LightState::None;
        case LevelId::L1: {
            if ((cx & 1) || (cz & 1)) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (fbm2(seed_, cx / 14.0f, cz / 14.0f, S_LIGHT) < 0.33f) return LightState::Dead; // power out
            if (r < 0.10f) return LightState::Dead;
            if (r < 0.18f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L2: {
            if (((cx + cz) & 1) != 0) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (r < 0.30f) return LightState::Dead;
            if (r < 0.42f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L4: {
            if ((cx & 1) || (cz & 1)) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (fbm2(seed_, cx / 12.0f, cz / 12.0f, S_LIGHT) < 0.25f) return LightState::Dead;
            if (r < 0.12f) return LightState::Dead;
            if (r < 0.18f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L70: case LevelId::L94: case LevelId::TSG4: case LevelId::L11: {
            Bldg lb;
            if (building(cx, cz, lb)) { // lamps in every room of a building
                int lu, lv;
                bldg_local(lb, cx, cz, lu, lv);
                bool lit = lb.w == 3 ? (lv == 0 ? (lu == 0 || lu == 2 || lb.kind == 0 || lb.kind == 3) && lu != 1 : (lv == 1 && lu == 1))
                                     : ((lv == 1 && (lu == 1 || lu == 4)) || (lv == 4 && (lu % 2 == 1)));
                if (!lit) return LightState::None;
                if (id_ == LevelId::TSG4) return r < 0.4f ? LightState::Dead : (r < 0.7f ? LightState::Flicker : LightState::On);
                return r < 0.15f ? LightState::Dead : (r < 0.25f ? LightState::Flicker : LightState::On);
            }
            if (id_ == LevelId::L11) return LightState::None; // (broad daylight out on the streets)
            Room room;
            if (house_room(cx, cz, room)) { // one lamp per house
                RoomFrame f(room);
                Vec2 lp = f.to_world(f.u_door, CELL * 0.5f);
                if (floor_to_int(lp.x / CELL) != cx || floor_to_int(lp.z / CELL) != cz) return LightState::None;
                return r < 0.2f ? LightState::Dead : (r < 0.3f ? LightState::Flicker : LightState::On);
            }
            if (id_ == LevelId::L70 ? (pmod(cx, FROST_GRID) != 2 || pmod(cz, FROST_GRID) != 2)
                                    : (pmod(cx, id_ == LevelId::L94 ? TOWN_GRID : CITY_GRID) != 2 || pmod(cz, id_ == LevelId::L94 ? TOWN_GRID : CITY_GRID) != 2))
                return LightState::None;
            if (id_ == LevelId::L94 && !town(cx, cz)) return LightState::None;
            if (id_ == LevelId::L70 && ice(cx, cz)) return LightState::None;
            if (near_spawn(cx, cz, 3)) return LightState::On;
            if (id_ == LevelId::TSG4) return r < 0.45f ? LightState::Dead : (r < 0.75f ? LightState::Flicker : LightState::On);
            return r < 0.15f ? LightState::Dead : (r < 0.22f ? LightState::Flicker : LightState::On);
        }
        case LevelId::L188: { // the hotel ring: lamps down the corridors, one in each room; the courtyards are lit by the sky
            int c = court_cell(cx, cz);
            if (c == 0) { // lamp posts round the atrium's square and paths
                int u = cx + HOTEL_HALF - 5, v = cz + HOTEL_HALF - 5;
                bool post = ((u == 4 || u == 9) && (v == 4 || v == 9)) || ((u == 0 || u == 13) && (v == 3 || v == 10)) || ((v == 0 || v == 13) && (u == 3 || u == 10));
                return post ? (r < 0.15f ? LightState::Flicker : LightState::On) : LightState::None;
            }
            if (c == 4) return LightState::None;
            Room room;
            if (court_room(cx, cz, room)) {
                RoomFrame f(room);
                Vec2 lp = f.to_world(f.u_door, CELL * 0.5f);
                if (floor_to_int(lp.x / CELL) != cx || floor_to_int(lp.z / CELL) != cz) return LightState::None;
                return r < 0.3f ? LightState::Dead : (r < 0.42f ? LightState::Flicker : LightState::On);
            }
            if (c == 1 && ((cx + cz) & 1) != 0) return LightState::None;
            return r < 0.18f ? LightState::Dead : (r < 0.3f ? LightState::Flicker : LightState::On);
        }
        case LevelId::Hub:
            return ((cx & 1) == 0 && (cz & 1) == 0) ? LightState::On : LightState::None;
        case LevelId::TheEnd: // square panels in a grid, nearly all of them on
            if ((cx & 1) || (cz & 1)) return LightState::None;
            return r < 0.04f ? LightState::Dead : (r < 0.08f ? LightState::Flicker : LightState::On);
        case LevelId::L5: case LevelId::TSG1: case LevelId::L13: {
            int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
            bool corridor = mx == 0 || mz == 0;
            Room room;
            if (hotel_room(cx, cz, room)) { // one ceiling lamp per guest room, often dead
                RoomFrame f(room);
                Vec2 lp = f.to_world(f.u_door, CELL * 0.5f); // in the door's lane, by the far wall
                if (floor_to_int(lp.x / CELL) != cx || floor_to_int(lp.z / CELL) != cz) return LightState::None;
                if (r < 0.25f) return LightState::Dead;
                if (r < 0.37f) return LightState::Flicker;
                return LightState::On;
            }
            if (corridor) { if (((cx + cz) & 1) != 0) return LightState::None; }
            else if (mx != 2 || mz != 2) return LightState::None; // one chandelier per ballroom
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (fbm2(seed_, cx / 10.0f, cz / 10.0f, S_LIGHT) < 0.30f) return LightState::Dead;
            if (r < 0.15f) return LightState::Dead;
            if (r < 0.22f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L8:
            if (near_spawn(cx, cz, 0)) return LightState::On;
            return r < 0.035f ? LightState::On : LightState::None; // glowing fungus
        case LevelId::L9: {
            Room room;
            if (house_room(cx, cz, room)) { // a lamp left on in some of the houses
                RoomFrame f(room);
                Vec2 lp = f.to_world(f.u_door, CELL * 0.5f);
                if (floor_to_int(lp.x / CELL) != cx || floor_to_int(lp.z / CELL) != cz) return LightState::None;
                return r < 0.4f ? LightState::Dead : (r < 0.5f ? LightState::Flicker : LightState::On);
            }
            int mx = pmod(cx, SUBURB_GRID), mz = pmod(cz, SUBURB_GRID);
            if (mx != 2 || mz != 2) return LightState::None;
            if (near_spawn(cx, cz, 3)) return LightState::On;
            if (r < 0.45f) return LightState::Dead;
            if (r < 0.55f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L37:
            if ((cx & 1) || (cz & 1)) return LightState::None;
            return r < 0.03f ? LightState::Dead : LightState::On; // skylights
        default: return LightState::None;
    }
}

int World::floor_mat(int cx, int cz) const {
    switch (id_) {
        case LevelId::L1: return fbm2(seed_, cx / 3.0f, cz / 3.0f, S_NOISE + 20) > 0.66f ? 1 : 0; // standing water
        case LevelId::L2: { // metal grates at the junctions and in boiler rooms
            int mx = pmod(cx, PIPE_GRID), mz = pmod(cz, PIPE_GRID);
            return (mx == 0) == (mz == 0) ? 1 : 0;
        }
        case LevelId::L7: { // wooden floor in the house and on the piers, water everywhere else
            if (std::abs(cx) <= 4 && std::abs(cz) <= 4) return 0;
            if ((pmod(cz, 9) == 0 && hash01(seed_, floor_div(cx, 6), cz, S_LOT) < 0.6f) ||
                (pmod(cx, 9) == 0 && hash01(seed_, cx, floor_div(cz, 6), S_LOT) < 0.6f)) return 0; // piers
            return 1;
        }
        case LevelId::L3: return (pmod(cx, POWER_GRID) <= 1 || pmod(cz, POWER_GRID) <= 1) ? 1 : 0; // grates in the corridors
        case LevelId::L10: return (pmod(cx, 12) == 0 || pmod(cz, 12) == 0) ? 1 : 0;                 // dirt track / field
        case LevelId::L22: { // painted bays; rubble where the floor gave way
            int mz = pmod(cz, 4);
            if (mz == 1 || mz == 2) return 1;
            return hash01(seed_, cx, cz, S_NOISE) < 0.04f ? 2 : 0;
        }
        case LevelId::L24: return fbm2(seed_, cx / 4.0f, cz / 4.0f, S_NOISE + 40) > 0.64f ? 1 : 0; // craters
        case LevelId::L33: { // tiles in the halls, carpet in the open shops
            int mx = pmod(cx, MALL_GRID), mz = pmod(cz, MALL_GRID);
            return (mx < 3 || mz < 3) ? 0 : 1;
        }
        case LevelId::L36: return pmod(cz, AIR_GRID) <= 3 ? 1 : 0; // polished concourse, carpeted lounges
        case LevelId::L51: { // paved streets, dirt elsewhere
            int mx = pmod(cx, RUIN_GRID), mz = pmod(cz, RUIN_GRID);
            return (mx < 2 || mz < 2) ? 0 : 1;
        }
        case LevelId::L5: case LevelId::TSG1: case LevelId::L13: {
            int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
            if (mx == 0 || mz == 0) return 0;
            return guest_room(cx, cz) ? 2 : 1; // worn guest-room carpet / ballroom parquet
        }
        case LevelId::L70: {
            if (house(cx, cz)) return 3;
            if (ice(cx, cz)) return 2;
            return (pmod(cx, FROST_GRID) < 2 || pmod(cz, FROST_GRID) < 2) ? 1 : 0;
        }
        case LevelId::L94: {
            if (house(cx, cz)) return 3;
            if (!town(cx, cz)) return 0;
            return (pmod(cx, TOWN_GRID) < 2 || pmod(cz, TOWN_GRID) < 2) ? 1 : 0;
        }
        case LevelId::L188: { // the atrium: four lawns cut by diagonal paths, a paved square in the middle; carpet indoors
            int c = court_cell(cx, cz);
            if (c != 0) return 2;
            int u = cx + HOTEL_HALF - 5, v = cz + HOTEL_HALF - 5; // 0..13
            if (u <= 0 || u >= 13 || v <= 0 || v >= 13) return 0;  // the path round the edge
            if (std::abs(u - v) <= 0 || std::abs(u + v - 13) <= 0) return 0; // the diagonals
            if (u >= 5 && u <= 8 && v >= 5 && v <= 8) return 0;     // the square
            return 1;
        }
        case LevelId::TSG0: case LevelId::TSG2: {
            if (cx >= -1 && cx <= 1 && cz >= -1 && cz <= 1) return 2; // the shack's boards
            if (id_ == LevelId::TSG0 && !near_spawn(cx, cz, 4) && fbm2(seed_, cx / 6.0f, cz / 6.0f, S_NOISE + 60) > 0.70f) return 1; // almond water lakes
            return 0;
        }
        case LevelId::TSG3: { // islands in an endless toxic sea
            if (near_spawn(cx, cz, 3)) return 0;
            return fbm2(seed_, cx / 7.0f, cz / 7.0f, S_NOISE + 70) > 0.64f ? 0 : 1;
        }
        case LevelId::TSG4: {
            if (pmod(cx, RIVER_GRID) >= 18 && pmod(cx, RIVER_GRID) <= 21) return pmod(cz, CITY_GRID) < 2 ? 2 : 0; // river, bridges
            int mx = pmod(cx, CITY_GRID), mz = pmod(cz, CITY_GRID);
            return (mx < 2 || mz < 2) ? 1 : 2;
        }
        case LevelId::L9: {
            if (house(cx, cz)) return 3;
            int mx = pmod(cx, SUBURB_GRID), mz = pmod(cz, SUBURB_GRID);
            if (mx < 2 || mz < 2) return 1;
            if (mx == 2 || mx == 7 || mz == 2 || mz == 7) return 2;
            return 0;
        }
        case LevelId::L11: {
            int mx = pmod(cx, CITY_GRID), mz = pmod(cz, CITY_GRID);
            return (mx < 2 || mz < 2) ? 1 : 2;
        }
        case LevelId::L37: return pool(cx, cz) ? 1 : 0; // the pools' tiles
        case LevelId::TheEnd: return fbm2(seed_, cx / 6.0f, cz / 6.0f, S_NOISE + 80) > 0.66f ? 1 : 0; // patches of patterned blue carpet
        default: return 0;
    }
}

uint32_t World::tint(int cx, int cz) const {
    switch (id_) {
        case LevelId::L9: case LevelId::L70: case LevelId::L94: {
            int lx, lz, bx, bz;
            if (house_lot(cx, cz, lx, lz, bx, bz)) {
                static const uint32_t PAINT[] = {0xc9c2b0, 0x9fb3b8, 0xb8a98c, 0xa3aa8c, 0xc4b4a8, 0x8f9aa6, 0xd2cbb8};
                static const uint32_t TOON[] = {0xf2d38a, 0xe8a07a, 0x9fd0c8, 0xf0c0d0, 0xc8d890, 0xf4e8c0, 0xb0c8f0}; // Motion: candy colours
                return (id_ == LevelId::L94 ? TOON : PAINT)[hash3(seed_, bx, bz, S_TINT) % 7u];
            }
            if (id_ == LevelId::L70) {
                static const uint32_t TOWER[] = {0xd8d4cc, 0x9aa4b0, 0xc0b0a0, 0x8e98a2, 0xe0dcd4};
                return TOWER[hash3(seed_, floor_div(cx, FROST_GRID), floor_div(cz, FROST_GRID), S_TINT) % 5u];
            }
            return 0xffffff;
        }
        case LevelId::L11: {
            int qx = city_quarter(cx), qz = city_quarter(cz);
            static const uint32_t STONE[] = {0xd8d4cc, 0xb9bcc0, 0xc8b8a4, 0x9ea4aa, 0xe2ddd2, 0xaaa196};
            return STONE[hash3(seed_, qx, qz, S_TINT) % 6u];
        }
        case LevelId::L22: { // car colours
            static const uint32_t CAR[] = {0x8a2b25, 0x2c4a7a, 0xb8b8b0, 0x2a2a2a, 0x5a6b3a, 0x9a8a5a, 0x6b6f78};
            int mz = pmod(cz, 4);
            if ((mz != 1 && mz != 2) || block_h(cx, cz) > 2.0f) return 0xffffff; // stairwells stay concrete
            return CAR[hash3(seed_, cx, cz, S_TINT) % 7u];
        }
        case LevelId::L33: { // shop fronts in different colours
            int mx = pmod(cx, MALL_GRID), mz = pmod(cz, MALL_GRID);
            if (mx < 3 || mz < 3) return 0xffffff;
            static const uint32_t SHOP[] = {0xffd0e0, 0xd0f0ff, 0xfff0c0, 0xd8ffd0, 0xe8d8ff, 0xffffff};
            return SHOP[hash3(seed_, floor_div(cx, MALL_GRID), floor_div(cz, MALL_GRID), S_TINT) % 6u];
        }
        case LevelId::L10: { // red barns, grey silos, straw-coloured hay
            float h = block_h(cx, cz);
            return h > 8.0f ? 0xb8b8b4 : (h > 5.0f ? 0xa83a2c : (h < 2.8f ? 0xe8d088 : 0xb09070));
        }
        case LevelId::L51: {
            float r = hash01(seed_, floor_div(cx, RUIN_GRID), floor_div(cz, RUIN_GRID), S_TINT);
            return r < 0.5f ? 0xd8ccb0 : 0xc0b8a8;
        }
        default: return 0xffffff;
    }
}

bool World::balloon(int cx, int cz) const {
    return id_ == LevelId::LFun && !solid(cx, cz) && !near_spawn(cx, cz, 0) && hash01(seed_, cx, cz, S_BALLOON) < 0.14f;
}

bool World::pipe_run(int cx, int cz, bool& along_x) const {
    if (id_ != LevelId::L2 || solid(cx, cz)) return false;
    int mx = pmod(cx, PIPE_GRID), mz = pmod(cz, PIPE_GRID);
    if (mz == 0 && mx != 0) { along_x = true; return true; }
    if (mx == 0 && mz != 0) { along_x = false; return true; }
    return false;
}

// ------------------------------------------------------------------ 1.1 levels
bool World::water(int cx, int cz) const {
    switch (id_) {
        case LevelId::L7: case LevelId::TSG0: case LevelId::TSG3: return floor_mat(cx, cz) == 1;
        case LevelId::TSG4: return floor_mat(cx, cz) == 0;
        default: return false;
    }
}

bool World::ice(int cx, int cz) const {
    if (id_ != LevelId::L70) return false;
    int m = pmod(cz, FROST_RIVER);
    return m >= 24 && m <= 26 && !near_spawn(cx, cz, 4);
}

float World::darkness(float t) const {
    auto fade = [](float x, float day, float edge, float night) { // day -> fade -> night -> fade
        float c = day + edge + night + edge;
        x = std::fmod(x, c);
        if (x < day) return 0.0f;
        if (x < day + edge) return (x - day) / edge;
        if (x < day + edge + night) return 1.0f;
        return 1.0f - (x - day - edge - night) / edge;
    };
    switch (id_) {
        case LevelId::TSG0: return std::fmod(t, 210.0f) >= 150.0f ? 1.0f : 0.0f; // no dusk: the sky just goes black
        case LevelId::L70: return fade(t, 300.0f, 15.0f, 90.0f);
        case LevelId::L94: return fade(t, 250.0f, 12.0f, 86.0f);
        default: return 0.0f;
    }
}

ExitStyle World::exit_style() const {
    switch (id_) {
        case LevelId::TSG0: return ExitStyle::Hole;     // dig ten metres down
        case LevelId::TSG2: return ExitStyle::Angel;    // ask a fallen angel
        case LevelId::TSG3: return ExitStyle::Clock;    // touch an analog clock
        case LevelId::TSGEnd: return ExitStyle::LoneDoor;
        default: return ExitStyle::Door;
    }
}

bool World::town(int cx, int cz) const {
    int rx = floor_div(cx + TOWN_REGION / 2, TOWN_REGION), rz = floor_div(cz + TOWN_REGION / 2, TOWN_REGION);
    if (rx == 0 && rz == 0) return true;
    return hash01(seed_, rx, rz, S_DISTRICT) < 0.4f;
}

bool World::hill(int cx, int cz) const {
    (void)cx; (void)cz;
    return false; // 1.2: the hills are real ground now (hill_h), not blocks in the way
}

// ------------------------------------------------------------------ 1.2 terrain
// Level 94: big rolling hills between the towns. A town sits on flat ground; the hills rise
// from its edges.
float World::hill_h(float x, float z) const {
    if (id_ != LevelId::L94) return 0;
    const float RS = TOWN_REGION * CELL;              // region size in metres
    float ox = x + RS * 0.5f, oz = z + RS * 0.5f;     // regions are offset by half (spawn in the middle of one)
    int rx = floor_to_int(ox / RS), rz = floor_to_int(oz / RS);
    auto is_town = [&](int a, int b) { return (a == 0 && b == 0) || hash01(seed_, a, b, S_DISTRICT) < 0.4f; };
    if (is_town(rx, rz)) return 0;
    // distance to the nearest town region around this one
    float lx = ox - rx * RS, lz = oz - rz * RS;       // 0..RS inside this region
    float dmin = 1e9f;
    for (int b = -1; b <= 1; b++)
        for (int a = -1; a <= 1; a++) {
            if ((a == 0 && b == 0) || !is_town(rx + a, rz + b)) continue;
            float dx = a < 0 ? lx : (a > 0 ? RS - lx : 0.0f), dz = b < 0 ? lz : (b > 0 ? RS - lz : 0.0f);
            dmin = std::min(dmin, std::sqrt(dx * dx + dz * dz));
        }
    float mask = smooth(clampf(dmin / (7.0f * CELL), 0, 1));
    float n = fbm2(seed_, x / (9.0f * CELL), z / (9.0f * CELL), S_NOISE + 50);
    float d = vnoise2(seed_, x / (3.5f * CELL), z / (3.5f * CELL), S_NOISE + 55);
    float h = clampf((n - 0.28f) / 0.5f, 0, 1);
    return (h * h * (3 - 2 * h) * 11.0f + d * 1.2f) * mask;
}

// Level 37: round pools that follow the lie of the noise, a tiled deck all around, never
// touching a wall line (they sit in the middle of the halls) or the spawn room.
float World::pool_depth(float x, float z) const {
    if (id_ != LevelId::L37) return 0;
    float gx = x / CELL, gz = z / CELL;
    // 1.3: most halls are flooded a hand's depth, with a dry tiled ledge along every wall line and
    // the deep pools sunk into the flood
    auto line_dist = [](float g) { float m = g - std::floor(g / 4.0f) * 4.0f; return std::min(m, 4.0f - m); };
    float wl = std::min(line_dist(gx), line_dist(gz));
    float wet = smooth(clampf((fbm2(seed_, gx / 9.0f, gz / 9.0f, S_NOISE + 31) - 0.36f) / 0.12f, 0, 1));
    float ledge = smooth(clampf((wl - 0.32f) / 0.22f, 0, 1));
    float flood = 0.34f * wet * ledge;
    float n = fbm2(seed_, gx / 4.0f, gz / 4.0f, S_NOISE + 30);
    float d = clampf((n - 0.58f) / 0.10f, 0, 1) * smooth(clampf((wl - 0.35f) / 0.6f, 0, 1));
    float spawn = smooth(clampf((std::max(std::fabs(gx - 0.5f), std::fabs(gz - 0.5f)) - 2.6f) / 1.2f, 0, 1)); // the spawn room is dry
    return (flood + smooth(d) * 1.85f) * spawn;
}

// The moon: soft rolling regolith pocked with craters (bowls with a raised rim), flat where you land.
float World::moon_h(float x, float z) const {
    const float RG = 8 * CELL; // one crater chance per 8x8 cells
    float h = (fbm2(seed_, x / (12 * CELL), z / (12 * CELL), S_NOISE + 90) - 0.5f) * 2.4f;
    int rx = floor_to_int(x / RG), rz = floor_to_int(z / RG);
    for (int b = -1; b <= 1; b++)
        for (int a = -1; a <= 1; a++) {
            int qx = rx + a, qz = rz + b;
            if (hash01(seed_, qx, qz, S_NOISE + 91) > 0.6f) continue;
            float ccx = (qx + 0.2f + 0.6f * hash01(seed_, qx, qz, S_NOISE + 92)) * RG, ccz = (qz + 0.2f + 0.6f * hash01(seed_, qx, qz, S_NOISE + 93)) * RG;
            float R = (0.18f + 0.3f * hash01(seed_, qx, qz, S_NOISE + 94)) * RG, D = R * 0.28f;
            float d = std::sqrt((x - ccx) * (x - ccx) + (z - ccz) * (z - ccz)) / R;
            if (d > 2.0f) continue;
            float bowl = d < 1.0f ? -D * (1.0f - d * d) : 0.0f;
            float rim = D * 0.4f * std::exp(-((d - 1.0f) / 0.22f) * ((d - 1.0f) / 0.22f));
            h += bowl + rim;
        }
    float spawn = smooth(clampf((std::sqrt(x * x + z * z) - 6.0f) / 10.0f, 0, 1));
    return h * spawn;
}

float World::floor_y(float x, float z) const {
    switch (id_) {
        case LevelId::L37: return -pool_depth(x, z);
        case LevelId::L94: return hill_h(x, z);
        case LevelId::L24: return moon_h(x, z);
        default: return 0;
    }
}

float World::water_y(float x, float z) const {
    if (id_ == LevelId::L37 && pool_depth(x, z) > 0.07f) return -0.06f;
    return -1e9f;
}

bool World::pool(int cx, int cz) const {
    if (id_ != LevelId::L37) return false;
    for (int k = 0; k < 5; k++) { // the middle and the four corners
        float fx = k == 0 ? 0.5f : (k & 1 ? 0.02f : 0.98f), fz = k == 0 ? 0.5f : (k & 2 ? 0.02f : 0.98f);
        if (pool_depth((cx + fx) * CELL, (cz + fz) * CELL) > 0.07f) return true;
    }
    return false;
}

bool World::city4_building(int cx, int cz) const {
    if (id_ != LevelId::TSG4) return false;
    if (pmod(cx, RIVER_GRID) >= 18 && pmod(cx, RIVER_GRID) <= 21) return false; // the river
    int mx = pmod(cx, CITY_GRID), mz = pmod(cz, CITY_GRID);
    if (mx < 3 || mx > 8 || mz < 3 || mz > 8) return false;
    return hash01(seed_, floor_div(cx, CITY_GRID), floor_div(cz, CITY_GRID), S_LOT) >= 0.08f;
}

bool World::floating_block(int cx, int cz, float& lift) const {
    float tilt;
    lift = 0;
    if (!city4_building(cx, cz)) return false;
    return glitch(cx, cz, lift, tilt) && lift > 0;
}

bool World::glitch(int cx, int cz, float& lift, float& tilt) const {
    lift = 0; tilt = 0;
    if (id_ != LevelId::TSG4) return false;
    int qx = city_quarter(cx), qz = city_quarter(cz); // one building per 3x3 quarter
    float r = hash01(seed_, qx, qz, S_LOT + 9);
    if (r < 0.18f) { lift = 2.0f + hash01(seed_, qx, qz, S_LOT + 10) * 7.0f; return true; }     // floating
    if (r < 0.32f) { tilt = (hash01(seed_, qx, qz, S_LOT + 11) - 0.5f) * 0.5f; return true; }  // leaning
    return false;
}

int World::house_lot(int cx, int cz, int& lx, int& lz, int& bx, int& bz) const {
    if (!houses()) return 0;
    int mx, mz, lot;
    float empty = 0.15f, locked = 0.3f;
    if (id_ == LevelId::L70) {
        if (ice(cx, cz)) return 0;
        mx = pmod(cx, FROST_GRID); mz = pmod(cz, FROST_GRID);
        int blx = floor_div(cx, FROST_GRID), blz = floor_div(cz, FROST_GRID);
        if (!(blx == 0 && blz == 0) && hash01(seed_, blx, blz, S_LOT) < 0.42f) return 0; // downtown block
        if (mz < 4 || mz > 5 || !((mx >= 3 && mx <= 4) || (mx >= 6 && mx <= 7))) return 0;
        lot = mx >= 6 ? 1 : 0;
        lx = mx - (lot ? 6 : 3); lz = mz - 4;
        bx = blx * 2 + lot; bz = blz;
        empty = 0.0f; locked = 0.25f;
    } else {
        if (id_ == LevelId::L94 && !town(cx, cz)) return 0;
        mx = pmod(cx, SUBURB_GRID); mz = pmod(cz, SUBURB_GRID);
        if (mx < 3 || mx > 6 || mz < 4 || mz > 5) return 0;
        lot = (mx - 3) / 2;
        lx = (mx - 3) % 2; lz = mz - 4;
        bx = floor_div(cx, SUBURB_GRID) * 2 + lot; bz = floor_div(cz, SUBURB_GRID);
        if (id_ == LevelId::L94) { empty = 0.08f; locked = 0.15f; }
    }
    if (hash01(seed_, bx, bz, S_LOT) < empty) return 0;                 // an empty lot
    return hash01(seed_, bx, bz, S_LOT + 5) < locked ? 2 : 1;           // locked up, or open
}

bool World::house_room(int cx, int cz, Room& r) const {
    int lx, lz, bx, bz;
    if (house_lot(cx, cz, lx, lz, bx, bz) != 1) return false;
    int c0x = cx - lx, c0z = cz - lz;
    r.bx = bx; r.bz = bz; r.qx = 0; r.qz = 0;
    r.x0 = c0x * CELL; r.x1 = r.x0 + 2 * CELL;
    r.z0 = c0z * CELL; r.z1 = r.z0 + 2 * CELL;
    uint32_t h = hash3(seed_, bx, bz, S_ROOM + 3);
    r.door_side = (h & 1) ? 2 : 3;  // front door onto the yard to the north or south
    r.dcx = c0x + (int)((h >> 1) & 1u);
    r.dcz = r.door_side == 2 ? c0z : c0z + 1;
    return true;
}

bool World::house(int cx, int cz) const {
    Room r;
    return house_room(cx, cz, r);
}


// ------------------------------------------------------------------ 1.2 buildings
// Level 188: around every courtyard a ring of hotel: a corridor down the middle of a five-cell
// band, two-cell rooms on both sides of it with their windows on the courtyards, a passage
// through to the courtyard halfway along, stairwell cores where the bands cross.
// 1.3: Level 188 is one hotel now, not an endless grid of them: a great atrium (a lawn, paths,
// a fountain, a glass roof 24 m up, windows on every side) ringed by five cells of hotel - rooms
// facing the atrium, a corridor, rooms facing outward - and nothing beyond its outer walls.
int World::court_cell(int cx, int cz) const {
    if (id_ != LevelId::L188) return 0;
    int lx = cx + HOTEL_HALF, lz = cz + HOTEL_HALF;
    if (lx < 0 || lx > 23 || lz < 0 || lz > 23) return 4; // outside the hotel: solid
    bool bx = lx <= 4 || lx >= 19, bz = lz <= 4 || lz >= 19;
    if (!bx && !bz) return 0;
    int ax = lx <= 4 ? lx : 23 - lx, az = lz <= 4 ? lz : 23 - lz;
    if (bx && bz) return (ax == 2 || az == 2) ? 1 : 4; // the corners: the corridors meet, stairwells fill the rest
    int a = bx ? ax : az, b = bx ? lz : lx;
    if (a == 2) return 1;
    if (a >= 3 && (b == 8 || b == 15)) return 3;      // the ways through to the atrium
    if (b == 7 || b == 18) return 4;                  // linen cupboards (locked)
    int side = bx ? (lx <= 4 ? 0 : 1) : (lz <= 4 ? 2 : 3);
    int pair = b < 7 ? 0 : (b < 11 ? 1 : (b < 13 ? 2 : (b < 15 ? 3 : 4)));
    if (hash01(seed_, side * 8 + (a >= 3 ? 4 : 0) + pair, 188, S_ROOM + 20) < 0.15f) return 4; // a locked room
    return 2;
}

bool World::court_room(int cx, int cz, Room& r) const {
    if (court_cell(cx, cz) != 2) return false;
    int lx = cx + HOTEL_HALF, lz = cz + HOTEL_HALF;
    bool bx = lx <= 4 || lx >= 19;
    bool low = bx ? lx <= 4 : lz <= 4;                // the west / north band
    int a = bx ? (low ? lx : 23 - lx) : (low ? lz : 23 - lz);
    int b = bx ? lz : lx;
    int a0 = a < 2 ? 0 : 3;
    int b0 = b < 7 ? 5 : (b < 11 ? 9 : (b < 13 ? 11 : (b < 15 ? 13 : 16)));
    // the two local band rows a0, a0+1 as cells; the room spans b0, b0+1 along the band
    int ra = low ? a0 : 23 - (a0 + 1);                // the lower of the two cells across the band
    int c0x = bx ? ra - HOTEL_HALF : b0 - HOTEL_HALF;
    int c0z = bx ? b0 - HOTEL_HALF : ra - HOTEL_HALF;
    r.bx = c0x; r.bz = c0z; r.qx = 0; r.qz = 0;
    r.x0 = c0x * CELL; r.x1 = r.x0 + 2 * CELL;
    r.z0 = c0z * CELL; r.z1 = r.z0 + 2 * CELL;
    int idx = (int)(hash3(seed_, c0x, c0z, S_ROOM + 21) & 1u);
    // the door faces the corridor (band row 2)
    bool corridor_up = low ? (a0 == 0) : (a0 == 3);   // is the corridor on the + side of the room?
    if (bx) {
        r.door_side = corridor_up ? 1 : 0;
        r.dcx = corridor_up ? c0x + 1 : c0x;
        r.dcz = c0z + idx;
    } else {
        r.door_side = corridor_up ? 3 : 2;
        r.dcz = corridor_up ? c0z + 1 : c0z;
        r.dcx = c0x + idx;
    }
    return true;
}

bool World::window_edge(int line, int along, bool vertical) const {
    if (id_ != LevelId::L188) return false;
    int ax = vertical ? line - 1 : along, az = vertical ? along : line - 1;
    int bx = vertical ? line : along, bz = vertical ? along : line;
    int ca = court_cell(ax, az), cb = court_cell(bx, bz);
    return (ca == 2 && cb == 0) || (ca == 0 && cb == 2);
}

// Buildings you can walk into. Level 11 / 4Four: one per 3x3 quarter of a block (a shop, a flat,
// an office, a diner). Froidetown: some of the downtown towers, a 6x6 lobby with two flats behind
// it. Each has a real front door on a street and rooms inside, with doorways between them.
bool World::building(int cx, int cz, Bldg& b) const {
    if (!city()) return false;
    int mx = pmod(cx, CITY_GRID), mz = pmod(cz, CITY_GRID);
    if (mx < 3 || mx > 8 || mz < 3 || mz > 8) return false;
    if (id_ == LevelId::L70) { // Froidetown's towers (FROST_GRID is the same 10)
        int bx = floor_div(cx, FROST_GRID), bz = floor_div(cz, FROST_GRID);
        if ((bx == 0 && bz == 0) || hash01(seed_, bx, bz, S_LOT) >= 0.42f) return false; // not a tower block
        if (ice(cx, cz) || hash01(seed_, bx, bz, S_LOT + 22) >= 0.55f) return false;
        b.x0 = bx * FROST_GRID + 3; b.z0 = bz * FROST_GRID + 3; b.w = b.d = 6;
        b.kind = 4;
        b.door_side = (int)(hash3(seed_, bx, bz, S_LOT + 23) % 4u);
    } else {
        if (id_ == LevelId::L11 && hash01(seed_, floor_div(cx, CITY_GRID), floor_div(cz, CITY_GRID), S_LOT) < 0.10f) return false; // a park
        if (id_ == LevelId::TSG4) {
            if (!city4_building(cx, cz)) return false;
            float lift, tilt;
            if (glitch(cx, cz, lift, tilt)) return false; // (nobody goes into the floating ones)
        }
        int kq = city_quarter(cx), lq = city_quarter(cz);
        if (hash01(seed_, kq, lq, S_LOT + 20) >= (id_ == LevelId::L11 ? 0.55f : 0.4f)) return false;
        b.x0 = floor_div(cx, CITY_GRID) * CITY_GRID + 3 + (mx >= 6 ? 3 : 0);
        b.z0 = floor_div(cz, CITY_GRID) * CITY_GRID + 3 + (mz >= 6 ? 3 : 0);
        b.w = b.d = 3;
        int qlx = (mx - 3) / 3, qlz = (mz - 3) / 3;
        uint32_t h = hash3(seed_, kq, lq, S_LOT + 21);
        b.door_side = (h & 1) ? (qlx == 0 ? 0 : 1) : (qlz == 0 ? 2 : 3); // always onto a street
        b.kind = (int)((h >> 1) % 4u);
    }
    int mid = b.w / 2 - (b.w == 6 ? 1 : 0) + (int)(hash3(seed_, b.x0, b.z0, S_LOT + 24) % (b.w == 6 ? 3u : 1u));
    switch (b.door_side) {
        case 0: b.dcx = b.x0; b.dcz = b.z0 + mid; break;
        case 1: b.dcx = b.x0 + b.w - 1; b.dcz = b.z0 + mid; break;
        case 2: b.dcx = b.x0 + mid; b.dcz = b.z0; break;
        default: b.dcx = b.x0 + mid; b.dcz = b.z0 + b.d - 1; break;
    }
    return true;
}

bool World::city_room(int cx, int cz, int& qx, int& qz, int& kind, int& door_side) const {
    Bldg b;
    if (!building(cx, cz, b)) return false;
    qx = b.x0; qz = b.z0; kind = b.kind; door_side = b.door_side;
    return true;
}


bool World::city_wall(int line, int along, bool vertical) const {
    int ax = vertical ? line - 1 : along, az = vertical ? along : line - 1;
    int bx = vertical ? line : along, bz = vertical ? along : line;
    Bldg ba, bb;
    bool ina = building(ax, az, ba), inb = building(bx, bz, bb);
    if (!ina && !inb) return false;
    if (ina && inb && ba.x0 == bb.x0 && ba.z0 == bb.z0) { // inside one building
        int ua, va, ub, vb;
        bldg_local(ba, ax, az, ua, va);
        bldg_local(ba, bx, bz, ub, vb);
        return bldg_partition(ba, ua, va, ub, vb) == 1;
    }
    for (int k = 0; k < 2; k++) { // the front door: a gap (its frame and door are furniture)
        if (!(k == 0 ? ina : inb)) continue;
        const Bldg& b = k == 0 ? ba : bb;
        if (vertical && b.door_side == 0 && line == b.dcx && along == b.dcz) return false;
        if (vertical && b.door_side == 1 && line == b.dcx + 1 && along == b.dcz) return false;
        if (!vertical && b.door_side == 2 && line == b.dcz && along == b.dcx) return false;
        if (!vertical && b.door_side == 3 && line == b.dcz + 1 && along == b.dcx) return false;
    }
    return true;
}

bool World::indoors(int cx, int cz) const {
    switch (id_) {
        case LevelId::L9: case LevelId::L94: return house(cx, cz);
        case LevelId::L70: { Bldg b; return house(cx, cz) || building(cx, cz, b); }
        case LevelId::L188: { int c = court_cell(cx, cz); return c == 1 || c == 2 || c == 3; }
        case LevelId::L11: case LevelId::TSG4: { Bldg b; return building(cx, cz, b); }
        case LevelId::L33: { // inside a shop: a normal ceiling, not the gallery's glass roof
            int mx = pmod(cx, MALL_GRID), mz = pmod(cz, MALL_GRID);
            return mx >= 3 && mz >= 3;
        }
        default: return false;
    }
}

float World::indoor_h(int cx, int cz) const {
    switch (id_) {
        case LevelId::L9: case LevelId::L94: return house_h();
        case LevelId::L70: return house(cx, cz) ? house_h() : 3.2f;
        case LevelId::L188: return 3.4f;
        case LevelId::L33: return 3.8f;
        default: (void)cx; (void)cz; return 3.2f;
    }
}

// Level !: every third row something is in the way: one lane blocked, sometimes two. There's
// always a way through, but you have to see it coming.
bool World::run_block(int cx, int cz) const {
    if (id_ != LevelId::LRun || cz > -5 || cz <= -RUN_LEN + 1) return false;
    if (pmod(cz, 3) != 0) return false;
    uint32_t h = hash3(seed_, 0, cz, S_SOLID);
    int free_lane = (int)(h % 3u) - 1;
    if ((h >> 3) % 100u < 45u) return cx != free_lane;          // two lanes blocked: one gap
    int o1 = free_lane == -1 ? 0 : -1, o2 = free_lane == 1 ? 0 : 1; // the other two lanes
    return cx == (((h >> 8) & 1u) ? o1 : o2);
}

// ------------------------------------------------------------------ M.E.G. outposts
bool World::has_outposts() const {
    switch (id_) {
        case LevelId::L0: case LevelId::L1: case LevelId::L2: case LevelId::L3: case LevelId::L4: case LevelId::L9: case LevelId::L11:
        case LevelId::L13: case LevelId::L21: case LevelId::L22: case LevelId::L33: case LevelId::L36: case LevelId::L51: case LevelId::L70:
        case LevelId::L188: case LevelId::Hub: case LevelId::L24: return true;
        default: return false;
    }
}

constexpr int OUTPOST_REGION = 24;

bool World::outpost_region(int rx, int rz, int& ocx, int& ocz) const {
    if (!has_outposts()) return false;
    if (id_ == LevelId::Hub) { if (rx != 0 || rz != 0) return false; ocx = 3; ocz = 0; return true; }
    if (rx == 0 && rz == 0) return false;
    if (hash01(seed_, rx, rz, S_CAMP + 40) >= (id_ == LevelId::L1 ? 0.55f : 0.30f)) return false;
    for (int t = 0; t < 3; t++) {
        int x = rx * OUTPOST_REGION - OUTPOST_REGION / 2 + 6 + (int)(hash3(seed_, rx * 3 + t, rz, S_CAMP + 41) % 12u);
        int z = rz * OUTPOST_REGION - OUTPOST_REGION / 2 + 6 + (int)(hash3(seed_, rx * 3 + t, rz, S_CAMP + 42) % 12u);
        bool ok = true;
        for (int b = -1; b <= 1 && ok; b++)
            for (int a = -1; a <= 1 && ok; a++) {
                int cx = x + a, cz = z + b;
                if (solid(cx, cz) || exit_cell(cx, cz) || water(cx, cz) || pool(cx, cz) || indoors(cx, cz) || pole(cx, cz)) ok = false;
                if (ok && a < 1 && (wall_x(cx + 1, cz))) ok = false;
                if (ok && b < 1 && (wall_z(cx, cz + 1))) ok = false;
            }
        if (!ok) continue;
        ocx = x; ocz = z;
        return true;
    }
    return false;
}

bool World::outpost(int cx, int cz, int& ocx, int& ocz) const {
    if (!has_outposts()) return false;
    int rx = floor_div(cx + OUTPOST_REGION / 2, OUTPOST_REGION), rz = floor_div(cz + OUTPOST_REGION / 2, OUTPOST_REGION);
    if (!outpost_region(rx, rz, ocx, ocz)) return false;
    return std::abs(cx - ocx) <= 1 && std::abs(cz - ocz) <= 1;
}

// ------------------------------------------------------------------ Level 5 guest rooms
// Each hotel block (4x4 cells between corridors) is solid (locked rooms: painted doors), a
// ballroom, or four enterable 2x2-cell guest rooms, each with one doorway onto a corridor.
int World::hotel_block(int bx, int bz) const {
    if (bx == 0 && bz == 0) return 2; // a room block right next to spawn: you'll find one
    float r = hash01(seed_, bx, bz, S_LOT);
    if (r < 0.12f) return 1;
    return r < 0.85f ? 2 : 0; // most blocks can be walked into now
}

bool World::hotel_room(int cx, int cz, Room& r) const {
    if (!hotel()) return false;
    int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
    if (mx == 0 || mz == 0) return false;
    r.bx = floor_div(cx, HOTEL_GRID);
    r.bz = floor_div(cz, HOTEL_GRID);
    if (hotel_block(r.bx, r.bz) != 2) return false;
    r.qx = mx <= 2 ? 0 : 1;
    r.qz = mz <= 2 ? 0 : 1;
    int c0x = r.bx * HOTEL_GRID + 1 + r.qx * 2, c0z = r.bz * HOTEL_GRID + 1 + r.qz * 2;
    r.x0 = c0x * CELL; r.x1 = r.x0 + 2 * CELL;
    r.z0 = c0z * CELL; r.z1 = r.z0 + 2 * CELL;
    uint32_t h = hash3(seed_, r.bx * 2 + r.qx, r.bz * 2 + r.qz, S_ROOM);
    bool vertical = h & 1;
    int idx = (int)((h >> 1) & 1u);
    if (vertical) {
        r.door_side = r.qx == 0 ? 0 : 1;
        r.dcx = r.qx == 0 ? c0x : c0x + 1;
        r.dcz = c0z + idx;
    } else {
        r.door_side = r.qz == 0 ? 2 : 3;
        r.dcz = r.qz == 0 ? c0z : c0z + 1;
        r.dcx = c0x + idx;
    }
    return true;
}

bool World::guest_room(int cx, int cz) const {
    Room r;
    return room_at(cx, cz, r);
}

bool World::hotel_wall(int line, int along, bool vertical) const {
    // cells on both sides of this edge
    int ax = vertical ? line - 1 : along, az = vertical ? along : line - 1;
    int bx = vertical ? line : along, bz = vertical ? along : line;
    Room ra, rb;
    bool ina = room_at(ax, az, ra), inb = room_at(bx, bz, rb);
    if (!ina && !inb) return false;
    if (ina && inb && ra.x0 == rb.x0 && ra.z0 == rb.z0) return false; // inside one room
    // a room's own doorway
    for (int k = 0; k < 2; k++) {
        const Room& r = k == 0 ? ra : rb;
        if (!(k == 0 ? ina : inb)) continue;
        if (vertical && r.door_side == 0 && line == r.dcx && along == r.dcz) return false;
        if (vertical && r.door_side == 1 && line == r.dcx + 1 && along == r.dcz) return false;
        if (!vertical && r.door_side == 2 && line == r.dcz && along == r.dcx) return false;
        if (!vertical && r.door_side == 3 && line == r.dcz + 1 && along == r.dcx) return false;
    }
    return true;
}

// Doors are part of the furniture, swung open or shut according to the game's state.
int World::furniture(int cx, int cz, Furn* out, bool* owner) const {
    int n = furniture_raw(cx, cz, out, owner);
    for (int k = 0; k < n; k++) {
        Furn& d = out[k];
        if (d.kind != FurnKind::DoorLeaf || d.width <= 0) continue;
        Vec2 dir = door_open(door_id(d)) ? d.open_dir : d.shut_dir;
        Vec2 tip = d.hinge + dir * d.width, side{-dir.z * 0.025f, dir.x * 0.025f};
        d.x0 = std::min(d.hinge.x, tip.x) - std::fabs(side.x) - 0.005f; d.x1 = std::max(d.hinge.x, tip.x) + std::fabs(side.x) + 0.005f;
        d.z0 = std::min(d.hinge.z, tip.z) - std::fabs(side.z) - 0.005f; d.z1 = std::max(d.hinge.z, tip.z) + std::fabs(side.z) + 0.005f;
    }
    return n;
}

uint32_t World::door_id(const Furn& f) {
    return mix32((uint32_t)(int)std::lround(f.hinge.x * 20.0f) * 0x9e3779b1u ^ (uint32_t)(int)std::lround(f.hinge.z * 20.0f) * 0x85ebca77u) | 1u;
}
bool World::door_default_open(uint32_t id) const { return hash01(seed_, (int)(id & 0xffff), (int)(id >> 16), S_DOOR + 77) < 0.28f; }
bool World::door_jammed(uint32_t id) const {
    return !door_default_open(id) && hash01(seed_, (int)(id & 0xffff), (int)(id >> 16), S_DOOR + 78) < 0.12f;
}

bool World::searchable(FurnKind k) {
    switch (k) {
        case FurnKind::Cabinet: case FurnKind::Dresser: case FurnKind::Nightstand: case FurnKind::Wardrobe: case FurnKind::Locker:
        case FurnKind::Crate: case FurnKind::Fridge: case FurnKind::Desk: case FurnKind::Boxes: case FurnKind::Reception: return true;
        default: return false;
    }
}
uint32_t World::furn_id(const Furn& f) {
    return mix32((uint32_t)(int)std::lround((f.x0 + f.x1) * 10.0f) * 0x27d4eb2fu ^ (uint32_t)(int)std::lround((f.z0 + f.z1) * 10.0f) * 0x165667b1u ^ (uint32_t)f.kind) | 1u;
}
// What you find when you go through a drawer, a cabinet, a locker or a crate (pure, like items).
ItemType World::container_item(const Furn& f) const {
    uint32_t id = furn_id(f);
    float r = hash01(seed_, (int)(id & 0xffff), (int)(id >> 16), S_ITEM + 40);
    float chance = (f.kind == FurnKind::Crate || f.kind == FurnKind::Locker) ? 0.5f : (f.kind == FurnKind::Boxes ? 0.3f : 0.36f);
    if (r >= chance * diff().items) return ItemType::None;
    float t = hash01(seed_, (int)(id & 0xffff), (int)(id >> 16), S_ITEM + 41);
    switch (f.kind) {
        case FurnKind::Fridge: return t < 0.45f ? ItemType::AlmondWater : (t < 0.7f ? ItemType::EnergyDrink : (t < 0.85f ? ItemType::RoyalRation : ItemType::LuckyMilk));
        case FurnKind::Locker: case FurnKind::Crate:
            return t < 0.2f ? ItemType::Batteries : t < 0.35f ? ItemType::Shells : t < 0.45f ? ItemType::Crowbar : t < 0.6f ? ItemType::MedKit :
                   t < 0.7f ? ItemType::FlashBulb : t < 0.8f ? ItemType::GlowStick : t < 0.9f ? ItemType::Firesalt : (t < 0.93f ? ItemType::Shotgun : ItemType::Flashlight);
        case FurnKind::Nightstand: case FurnKind::Dresser: case FurnKind::Wardrobe:
            return t < 0.25f ? ItemType::Painkillers : t < 0.45f ? ItemType::AlmondWater : t < 0.6f ? ItemType::Batteries : t < 0.7f ? ItemType::RabbitFoot :
                   t < 0.85f ? ItemType::MothJelly : ItemType::Chalk;
        default: // desks, cabinets, reception, boxes
            return t < 0.25f ? ItemType::Batteries : t < 0.4f ? ItemType::Chalk : t < 0.55f ? ItemType::AlmondWater : t < 0.7f ? ItemType::Painkillers :
                   t < 0.8f ? ItemType::Compass : t < 0.9f ? ItemType::Radio : ItemType::EnergyDrink;
    }
}

int World::furniture_raw(int cx, int cz, Furn* out, bool* owner) const {
    Room room;
    Bldg bd;
    if (building(cx, cz, bd)) { // a building: the front door, the doorways inside, every room furnished
        Room r;
        const int S = bd.w;
        const float R = S * CELL, H = indoor_h(cx, cz), hw = WALL_T * 0.5f;
        r.bx = bd.x0; r.bz = bd.z0; r.qx = r.qz = 0;
        r.x0 = bd.x0 * CELL; r.x1 = r.x0 + R; r.z0 = bd.z0 * CELL; r.z1 = r.z0 + R;
        r.door_side = bd.door_side; r.dcx = bd.dcx; r.dcz = bd.dcz;
        RoomFrame f(r);
        if (owner) *owner = cx == bd.x0 && cz == bd.z0;
        int n = 0;
        auto add = [&](FurnKind k, float u0, float v0, float u1, float v1, float y0, float y1, bool solid_, float fu = 0, float fv = 1) {
            if (n >= MAX_FURN) return;
            Vec2 a = f.to_world(u0, v0), b = f.to_world(u1, v1);
            Furn& p = out[n++];
            p = Furn{};
            p.kind = k;
            p.x0 = std::min(a.x, b.x); p.x1 = std::max(a.x, b.x);
            p.z0 = std::min(a.z, b.z); p.z1 = std::max(a.z, b.z);
            p.y0 = y0; p.y1 = y1; p.solid = solid_;
            p.face = f.dir(fu, fv);
        };
        // a doorway in a wall running along u at depth L, centred on U: two wall pieces, a lintel, a door
        auto doorway = [&](float U, float L, bool front) {
            FurnKind side = front ? FurnKind::DoorFrame : FurnKind::WallPiece;
            add(side, U - CELL * 0.5f, L - hw, U - 0.55f, L + hw, 0, front ? 2.45f : H, true);
            add(side, U + 0.55f, L - hw, U + CELL * 0.5f, L + hw, 0, front ? 2.45f : H, true);
            add(FurnKind::Lintel, U - 0.55f, L - hw, U + 0.55f, L + hw, 2.2f, front ? 2.45f : H, false);
            if (n >= MAX_FURN) return;
            Furn& d = out[n++];
            d = Furn{};
            d.kind = FurnKind::DoorLeaf;
            d.hinge = f.to_world(U - 0.55f, L);
            d.shut_dir = f.dir(1, 0);
            d.open_dir = f.dir(0, -1);
            d.width = 1.1f;
            d.y0 = 0; d.y1 = 2.15f;
            d.solid = true;
            d.face = f.dir(0, 1);
        };
        int du, dv;
        bldg_local(bd, bd.dcx, bd.dcz, du, dv);
        doorway((du + 0.5f) * CELL, R, true);
        for (int u = 0; u < S; u++) // the doorways between the rooms
            for (int v = 0; v + 1 < S; v++)
                if (bldg_partition(bd, u, v, u, v + 1) == 2) doorway((u + 0.5f) * CELL, (v + 1) * CELL, false);
        const float C = CELL;
        switch (bd.kind) {
            case 0: // a corner shop: shelves out front, the till by the door, fridges, a storeroom behind
                add(FurnKind::Rack, 1.75f, C + 0.7f, 2.25f, C + 2.6f, 0, 1.75f, true, 1, 0);
                add(FurnKind::Rack, 4.75f, C + 0.7f, 5.25f, C + 2.6f, 0, 1.75f, true, 1, 0);
                add(FurnKind::Rack, 1.75f, C + 2.8f, 2.25f, C + 4.0f, 0, 1.75f, true, 1, 0);
                add(FurnKind::Counter, 5.8f, R - 1.15f, 7.2f, R - 0.55f, 0, 1.0f, true, -1, 0);
                add(FurnKind::Fridge, 0.2f, C + hw + 0.03f, 0.9f, C + 0.75f, 0, 1.9f, true, 0, 1);
                add(FurnKind::Fridge, 0.95f, C + hw + 0.03f, 1.65f, C + 0.75f, 0, 1.9f, true, 0, 1);
                add(FurnKind::Rack, 0.3f, hw + 0.03f, 2.0f, 0.5f, 0, 1.9f, true, 0, 1);
                add(FurnKind::Boxes, 3.2f, 0.3f, 3.9f, 1.0f, 0, 0.9f, true);
                add(FurnKind::Crate, 4.3f, 0.3f, 5.1f, 1.1f, 0, 0.8f, true);
                break;
            case 1: // a flat: a bedroom and a bathroom at the back, the living room at the front
                add(FurnKind::Bed, 0.6f, 0.14f, 2.2f, 2.2f, 0, 0.55f, true, 0, 1);
                add(FurnKind::Headboard, 0.55f, hw + 0.01f, 2.25f, 0.14f, 0, 1.15f, true, 0, 1);
                add(FurnKind::Nightstand, 2.35f, hw + 0.02f, 2.85f, 0.5f, 0, 0.6f, true, 0, 1);
                add(FurnKind::Lamp, 2.48f, 0.15f, 2.72f, 0.39f, 0.6f, 0.98f, false);
                add(FurnKind::Wardrobe, 3.9f, hw + 0.02f, 4.9f, 0.62f, 0, 2.0f, true, 0, 1);
                add(FurnKind::Toilet, 6.7f, 0.2f, 7.2f, 0.85f, 0, 0.8f, true, 0, 1);
                add(FurnKind::Bathtub, 5.15f, 1.05f, 7.3f, 2.25f, 0, 0.6f, true, 0, -1);
                add(FurnKind::Sink, 5.2f, hw + 0.02f, 5.9f, 0.55f, 0, 0.9f, true, 0, 1);
                add(FurnKind::Mirror, 5.25f, hw + 0.005f, 5.85f, hw + 0.03f, 1.2f, 1.9f, false, 0, 1);
                add(FurnKind::Couch, 1.6f, C + hw + 0.1f, 3.6f, C + 0.95f, 0, 0.85f, true, 0, 1);
                add(FurnKind::Table, 2.15f, C + 1.5f, 3.05f, C + 2.2f, 0, 0.45f, true);
                add(FurnKind::Tv, 2.1f, C + 2.9f, 3.1f, C + 3.25f, 0, 1.15f, true, 0, -1);
                add(FurnKind::Rug, 1.4f, C + 1.1f, 3.8f, C + 2.7f, 0, 0.012f, false);
                add(FurnKind::Table2, 5.0f, C + 1.2f, 6.6f, C + 2.1f, 0, 0.76f, true);
                add(FurnKind::Chair, 5.55f, C + 0.6f, 6.03f, C + 1.08f, 0, 0.92f, true, 0, 1);
                add(FurnKind::Chair, 5.55f, C + 2.22f, 6.03f, C + 2.7f, 0, 0.92f, true, 0, -1);
                add(FurnKind::Fridge, R - hw - 0.75f, R - 1.6f, R - hw - 0.05f, R - 0.9f, 0, 1.8f, true, -1, 0);
                break;
            case 2: // an office: the manager's room in the back corner, desks out front
                add(FurnKind::Desk, 0.5f, 0.3f, 2.0f, 1.0f, 0, 0.76f, true, 0, 1);
                add(FurnKind::Chair, 1.0f, 1.15f, 1.48f, 1.63f, 0, 0.92f, true, 0, -1);
                add(FurnKind::Cabinet, hw + 0.02f, 1.7f, 0.62f, 2.2f, 0, 1.32f, true, 1, 0);
                for (int k = 0; k < 4; k++) {
                    float u = (k & 1) ? 4.9f : 1.2f, v = C + ((k & 2) ? 2.6f : 0.6f);
                    add(FurnKind::Desk, u, v, u + 1.4f, v + 0.72f, 0, 0.76f, true, 0, 1);
                    add(FurnKind::Chair, u + 0.46f, v + 0.85f, u + 0.94f, v + 1.33f, 0, 0.92f, true, 0, -1);
                }
                for (int k = 0; k < 3; k++) add(FurnKind::Cabinet, 3.2f + k * 0.55f, hw + 0.02f, 3.7f + k * 0.55f, 0.64f, 0, 1.32f, true, 0, 1);
                add(FurnKind::Plant, R - 0.8f, R - 0.8f, R - 0.25f, R - 0.25f, 0, 1.4f, true);
                break;
            case 3: // a diner: tables out front, the kitchen behind the pass
                add(FurnKind::Stove, 0.3f, hw + 0.02f, 0.95f, 0.65f, 0, 0.92f, true, 0, 1);
                add(FurnKind::Stove, 1.05f, hw + 0.02f, 1.7f, 0.65f, 0, 0.92f, true, 0, 1);
                add(FurnKind::Sink, 4.0f, hw + 0.02f, 4.8f, 0.6f, 0, 0.92f, true, 0, 1);
                add(FurnKind::Fridge, R - hw - 0.8f, 0.2f, R - hw - 0.05f, 0.9f, 0, 1.9f, true, -1, 0);
                add(FurnKind::Counter, 1.8f, 1.5f, 4.4f, 2.1f, 0, 1.0f, true, 0, -1);
                add(FurnKind::Counter, 0.4f, C + hw + 0.05f, 1.6f, C + 0.7f, 0, 1.05f, true, 0, 1);
                add(FurnKind::Counter, 3.4f, C + hw + 0.05f, 7.1f, C + 0.7f, 0, 1.05f, true, 0, 1);
                for (int k = 0; k < 3; k++) {
                    float u = k == 0 ? 1.8f : (k == 1 ? 5.6f : 3.7f), v = C + (k == 2 ? 3.6f : 2.1f);
                    add(FurnKind::RoundTable, u - 0.55f, v - 0.55f, u + 0.55f, v + 0.55f, 0, 0.75f, true);
                    add(FurnKind::Chair, u - 0.24f, v - 1.15f, u + 0.24f, v - 0.67f, 0, 0.92f, true, 0, 1);
                    add(FurnKind::Chair, u - 0.24f, v + 0.67f, u + 0.24f, v + 1.15f, 0, 0.92f, true, 0, -1);
                }
                break;
            default: { // a tower lobby: reception, sofas, mailboxes, the lifts; two flats behind
                const float L = 3 * C; // the lobby starts here
                add(FurnKind::Reception, 5.5f, L + 2.2f, 9.5f, L + 3.0f, 0, 1.1f, true, 0, 1);
                add(FurnKind::Sofa, 1.0f, L + 4.6f, 3.2f, L + 5.4f, 0, 0.8f, true, 1, 0);
                add(FurnKind::Sofa, 11.8f, L + 4.6f, 14.0f, L + 5.4f, 0, 0.8f, true, -1, 0);
                add(FurnKind::Table, 1.6f, L + 3.4f, 2.6f, L + 4.2f, 0, 0.45f, true);
                add(FurnKind::Mailboxes, hw + 0.02f, L + 1.0f, 0.4f, L + 2.6f, 0.4f, 1.9f, true, 1, 0);
                add(FurnKind::Elevator, R - hw - 0.06f, L + 1.0f, R - hw - 0.01f, L + 2.4f, 0, 2.3f, false, -1, 0);
                add(FurnKind::Elevator, R - hw - 0.06f, L + 3.0f, R - hw - 0.01f, L + 4.4f, 0, 2.3f, false, -1, 0);
                add(FurnKind::Plant, 4.2f, R - 0.9f, 4.8f, R - 0.3f, 0, 1.4f, true);
                add(FurnKind::Plant, 10.2f, R - 0.9f, 10.8f, R - 0.3f, 0, 1.4f, true);
                add(FurnKind::Rug, 5.0f, L + 3.4f, 10.0f, R - 1.2f, 0, 0.012f, false);
                for (int a2 = 0; a2 < 2; a2++) { // the two flats
                    float o = a2 * L;
                    add(FurnKind::Bed, o + 0.6f, 0.14f, o + 2.2f, 2.2f, 0, 0.55f, true, 0, 1);
                    add(FurnKind::Headboard, o + 0.55f, hw + 0.01f, o + 2.25f, 0.14f, 0, 1.15f, true, 0, 1);
                    add(FurnKind::Couch, o + 4.3f, 3.6f, o + 6.3f, 4.4f, 0, 0.85f, true, 0, -1);
                    add(FurnKind::Tv, o + 4.8f, hw + 0.02f, o + 5.8f, 0.4f, 0, 1.15f, true, 0, 1);
                    add(FurnKind::Fridge, o + 0.3f, 5.5f, o + 1.0f, 6.2f, 0, 1.8f, true, 1, 0);
                }
                break;
            }
        }
        return n;
    }
    if (!room_at(cx, cz, room)) {
        if (owner) *owner = true;
        return cell_props(cx, cz, out);
    }
    RoomFrame f(room);
    if (owner) *owner = floor_to_int((room.x0 + 0.1f) / CELL) == cx && floor_to_int((room.z0 + 0.1f) / CELL) == cz;
    int n = 0;
    const float H = houses() ? house_h() : (court() ? indoor_h(cx, cz) : info().ceil_h), hw = WALL_T * 0.5f, R = 2 * CELL;
    auto add = [&](FurnKind k, float u0, float v0, float u1, float v1, float y0, float y1, bool solid) {
        if (n >= MAX_FURN) return;
        if (court() && k == FurnKind::Painting) return; // (that wall is all windows)
        Vec2 a = f.to_world(u0, v0), b = f.to_world(u1, v1);
        Furn& p = out[n++];
        p = Furn{};
        p.kind = k;
        p.x0 = std::min(a.x, b.x); p.x1 = std::max(a.x, b.x);
        p.z0 = std::min(a.z, b.z); p.z1 = std::max(a.z, b.z);
        p.y0 = y0; p.y1 = y1;
        p.solid = solid;
        p.face = f.dir(0, 1); // into the room from the far wall; overridden below where needed
    };
    uint32_t h = hash3(seed_, room.bx * 2 + room.qx, room.bz * 2 + room.qz, S_ROOM + 1);
    const float ud = f.u_door;
    // doorway: wall either side of a 1.1 m opening, a lintel over it, the door swung open
    add(FurnKind::WallPiece, ud - CELL * 0.5f, R - hw, ud - 0.55f, R + hw, 0, H, true);
    add(FurnKind::WallPiece, ud + 0.55f, R - hw, ud + CELL * 0.5f, R + hw, 0, H, true);
    add(FurnKind::Lintel, ud - 0.55f, R - hw, ud + 0.55f, R + hw, 2.2f, H, false);
    add(FurnKind::DoorLeaf, ud - 0.55f, R - 1.0f, ud - 0.50f, R - hw, 0, 2.15f, true);
    if (n > 0 && out[n - 1].kind == FurnKind::DoorLeaf) { // it swings on a hinge at the doorway's edge
        out[n - 1].hinge = f.to_world(ud - 0.55f, R);
        out[n - 1].shut_dir = f.dir(1, 0);
        out[n - 1].open_dir = f.dir(0, -1);
        out[n - 1].width = 1.1f;
    }
    float bc = ud < CELL ? 3.6f : 1.4f;
    if (houses()) { // a home: a living room, a bedroom or a kitchen (Motion's are from the 1930s)
        const bool old = id_ == LevelId::L94;
        const int kind = (int)((h >> 3) % 3u);
        float side_u = ud < CELL ? hw + 0.03f : R - hw - 0.03f; // the side wall away from the door
        float sd = ud < CELL ? 1.0f : -1.0f;
        if (kind == 0) {
            add(FurnKind::Couch, bc - 1.0f, 0.12f, bc + 1.0f, 0.95f, 0, 0.85f, true);
            if (old) { add(FurnKind::Radio, bc - 0.32f, 3.25f, bc + 0.32f, 3.6f, 0, 1.05f, true); }
            else { add(FurnKind::Tv, bc - 0.5f, 3.2f, bc + 0.5f, 3.55f, 0, 1.15f, true); }
            out[n - 1].face = f.dir(0, -1);
            add(FurnKind::Table, bc - 0.45f, 1.6f, bc + 0.45f, 2.3f, 0, 0.45f, true);
            float bu = ud < CELL ? side_u : side_u - 0.45f;
            add(FurnKind::Bookcase, bu, 1.3f, bu + 0.45f, 2.7f, 0, 1.9f, true);
            out[n - 1].face = f.dir(sd, 0);
            add(FurnKind::Lamp, bc + 1.1f, 0.15f, bc + 1.34f, 0.39f, 0.0f, 1.38f, false);
            add(FurnKind::Painting, bc - 0.45f, hw + 0.005f, bc + 0.45f, hw + 0.03f, 1.35f, 1.95f, false);
            add(FurnKind::Rug, 0.9f, 1.1f, R - 0.9f, 2.6f, 0, 0.012f, false);
        } else if (kind == 1) {
            add(FurnKind::Bed, bc - 0.8f, 0.14f, bc + 0.8f, 2.2f, 0, 0.55f, true);
            add(FurnKind::Headboard, bc - 0.85f, hw + 0.01f, bc + 0.85f, 0.14f, 0, 1.15f, true);
            add(FurnKind::Nightstand, bc - 1.3f, hw + 0.02f, bc - 0.86f, 0.5f, 0, 0.6f, true);
            add(FurnKind::Lamp, bc - 1.2f, 0.15f, bc - 0.96f, 0.39f, 0.6f, 0.98f, false);
            float wu = ud < CELL ? side_u : side_u - 0.6f;
            add(FurnKind::Wardrobe, wu, 2.6f, wu + 0.6f, 3.7f, 0, 2.0f, true);
            out[n - 1].face = f.dir(sd, 0);
            add(FurnKind::Painting, bc - 0.45f, hw + 0.005f, bc + 0.45f, hw + 0.03f, 1.35f, 1.95f, false);
            add(FurnKind::Rug, 0.9f, 2.45f, R - 0.9f, 3.9f, 0, 0.012f, false);
        } else {
            add(FurnKind::Table2, 1.6f, 1.6f, 3.4f, 2.6f, 0, 0.76f, true);
            add(FurnKind::Chair, 1.95f, 1.0f, 2.43f, 1.48f, 0, 0.92f, true); out[n - 1].face = f.dir(0, 1);
            add(FurnKind::Chair, 2.6f, 1.0f, 3.08f, 1.48f, 0, 0.92f, true);  out[n - 1].face = f.dir(0, 1);
            add(FurnKind::Chair, 1.95f, 2.72f, 2.43f, 3.2f, 0, 0.92f, true); out[n - 1].face = f.dir(0, -1);
            add(FurnKind::Stove, bc - 1.0f, hw + 0.02f, bc - 0.35f, 0.62f, 0, 0.92f, true);
            add(FurnKind::Sink, bc - 0.3f, hw + 0.02f, bc + 0.5f, 0.6f, 0, 0.92f, true);
            float fu = ud < CELL ? R - hw - 0.75f : hw + 0.05f;
            add(FurnKind::Fridge, fu, R - hw - 0.8f, fu + 0.7f, R - hw - 0.05f, 0, old ? 1.45f : 1.8f, true);
            out[n - 1].face = f.dir(0, -1);
            if (id_ == LevelId::L70) add(FurnKind::Clock, bc + 0.6f, hw + 0.005f, bc + 0.95f, hw + 0.04f, 1.7f, 2.05f, false); // (everyone watches the time here)
        }
        return n;
    }
    const int court_kind = court() ? (int)((h >> 3) % 3u) : -1; // Level 188: a bedroom, a flat or an office
    if (court_kind == 2) {
        add(FurnKind::Desk, bc - 0.7f, 0.12f, bc + 0.7f, 0.84f, 0, 0.76f, true);
        add(FurnKind::Chair, bc - 0.24f, 1.0f, bc + 0.24f, 1.48f, 0, 0.92f, true); out[n - 1].face = f.dir(0, -1);
        float cu = ud < CELL ? R - hw - 0.55f : hw + 0.03f;
        add(FurnKind::Cabinet, cu, 0.4f, cu + 0.52f, 0.92f, 0, 1.32f, true); out[n - 1].face = f.dir(ud < CELL ? -1.0f : 1.0f, 0);
        add(FurnKind::Cabinet, cu, 0.95f, cu + 0.52f, 1.47f, 0, 1.32f, true); out[n - 1].face = f.dir(ud < CELL ? -1.0f : 1.0f, 0);
        add(FurnKind::Plant, bc + 1.1f, 2.6f, bc + 1.6f, 3.1f, 0, 1.4f, true);
        add(FurnKind::Painting, bc - 0.45f, hw + 0.005f, bc + 0.45f, hw + 0.03f, 1.4f, 2.0f, false);
        return n;
    }
    if (id_ == LevelId::L13 || court_kind == 1) { // an apartment: a couch facing a TV, a table, a fridge by the door
        add(FurnKind::Couch, bc - 1.0f, 0.12f, bc + 1.0f, 0.95f, 0, 0.85f, true);
        add(FurnKind::Tv, bc - 0.5f, 3.2f, bc + 0.5f, 3.55f, 0, 1.15f, true);
        out[n - 1].face = f.dir(0, -1);
        add(FurnKind::Table, bc - 0.45f, 1.6f, bc + 0.45f, 2.3f, 0, 0.45f, true); // coffee table
        float fu = ud < CELL ? R - hw - 0.75f : hw + 0.05f;
        add(FurnKind::Fridge, fu, R - hw - 0.8f, fu + 0.7f, R - hw - 0.05f, 0, 1.8f, true);
        out[n - 1].face = f.dir(0, -1);
        if (h & 1) add(FurnKind::Lamp, bc + 1.1f, 0.15f, bc + 1.34f, 0.39f, 0.0f, 0.38f + 1.0f, false);
        add(FurnKind::Painting, bc - 0.45f, hw + 0.005f, bc + 0.45f, hw + 0.03f, 1.35f, 1.95f, false);
        add(FurnKind::Rug, 0.9f, 1.1f, R - 0.9f, 2.6f, 0, 0.012f, false);
        return n;
    }
    // the bed against the far wall, on the other side from the door
    add(FurnKind::Bed, bc - 0.8f, 0.14f, bc + 0.8f, 2.2f, 0, 0.55f, true);
    add(FurnKind::Headboard, bc - 0.85f, hw + 0.01f, bc + 0.85f, 0.14f, 0, 1.15f, true);
    add(FurnKind::Nightstand, bc - 1.3f, hw + 0.02f, bc - 0.86f, 0.5f, 0, 0.6f, true);
    add(FurnKind::Nightstand, bc + 0.86f, hw + 0.02f, bc + 1.3f, 0.5f, 0, 0.6f, true);
    if (h & 1) add(FurnKind::Lamp, bc - 1.2f, 0.15f, bc - 0.96f, 0.39f, 0.6f, 0.98f, false);
    add(FurnKind::Painting, bc - 0.45f, hw + 0.005f, bc + 0.45f, hw + 0.03f, 1.35f, 1.95f, false);
    out[n - 1].face = f.dir(0, 1);
    // a dresser on the side wall away from the bed, a wardrobe in the corner by the door
    float du0 = ud < CELL ? hw + 0.02f : R - hw - 0.55f;
    add(FurnKind::Dresser, du0, 1.0f, du0 + 0.53f, 2.3f, 0, 0.9f, true);
    out[n - 1].face = f.dir(ud < CELL ? 1.0f : -1.0f, 0);
    if (h & 2) {
        float wu = ud < CELL ? R - hw - 0.62f : hw + 0.02f;
        add(FurnKind::Wardrobe, wu, R - hw - 1.1f, wu + 0.6f, R - hw - 0.05f, 0, 2.0f, true);
        out[n - 1].face = f.dir(ud < CELL ? -1.0f : 1.0f, 0);
    }
    add(FurnKind::Rug, 0.9f, 2.45f, R - 0.9f, 3.9f, 0, 0.012f, false);
    return n;
}

bool World::crop(int cx, int cz) const {
    return id_ == LevelId::L10 && !solid(cx, cz) && floor_mat(cx, cz) == 0 && !near_spawn(cx, cz, 0);
}

// ------------------------------------------------------------------ Level 21 rooms
// Some 3x3 blocks between the halls are hollow: an office or a storeroom behind a real door on
// the hall to the north or south.
bool World::door_room_block(int bx, int bz, bool& north) const {
    if (id_ != LevelId::L21) return false;
    uint32_t h = hash3(seed_, bx, bz, S_ROOM + 7);
    north = h & 1;
    return (h >> 1) % 100u < 45u;
}

bool World::door_room(int cx, int cz, int& bx, int& bz, bool& north) const {
    int mx = pmod(cx, DOOR_GRID), mz = pmod(cz, DOOR_GRID);
    if (mx == 0 || mz == 0) return false;
    bx = floor_div(cx, DOOR_GRID);
    bz = floor_div(cz, DOOR_GRID);
    return door_room_block(bx, bz, north);
}

bool World::door_room_wall(int line, int along, bool vertical) const {
    int ax = vertical ? line - 1 : along, az = vertical ? along : line - 1;
    int bxx = vertical ? line : along, bzz = vertical ? along : line;
    int b1x, b1z, b2x, b2z;
    bool n1, n2;
    bool ina = door_room(ax, az, b1x, b1z, n1), inb = door_room(bxx, bzz, b2x, b2z, n2);
    if (ina == inb) return false; // both outside, or both in the same room
    int rbx = ina ? b1x : b2x, rbz = ina ? b1z : b2z;
    bool north = ina ? n1 : n2;
    // the doorway: the middle cell of the north or south side
    if (!vertical && along == rbx * DOOR_GRID + 2 && line == (north ? rbz * DOOR_GRID + 1 : rbz * DOOR_GRID + DOOR_GRID)) return false;
    return true;
}

// ------------------------------------------------------------------ props
// Wanderers' camps: one chance per 16x16 region on indoor levels, a bedroll, boxes, a lantern,
// a fire barrel, and supplies left behind.
bool World::camp(int cx, int cz, int& ccx, int& ccz) const {
    switch (id_) {
        case LevelId::L0: case LevelId::L1: case LevelId::L2: case LevelId::L4: case LevelId::L6: case LevelId::L8:
        case LevelId::L19: case LevelId::L22: case LevelId::L33: case LevelId::L36: case LevelId::L51: case LevelId::L3: break;
        default: return false;
    }
    int rx = floor_div(cx + 8, 16), rz = floor_div(cz + 8, 16);
    if (rx == 0 && rz == 0) return false;
    if (hash01(seed_, rx, rz, S_CAMP) > 0.30f) return false;
    ccx = rx * 16 - 8 + 4 + (int)(hash3(seed_, rx, rz, S_CAMP + 1) % 8u);
    ccz = rz * 16 - 8 + 4 + (int)(hash3(seed_, rx, rz, S_CAMP + 2) % 8u);
    if (std::abs(cx - ccx) > 1 || std::abs(cz - ccz) > 1) return false;
    // the camp needs its 2x2 corner to be open floor
    for (int a = 0; a <= 1; a++)
        for (int b = 0; b <= 1; b++)
            if (solid(ccx + a, ccz + b)) return false;
    return true;
}

// Furniture and clutter on ordinary cells: chairs, boxes, barrels, plants, cars, trees... Each
// level has its own mix. Props hug a wall where there is one and never block a narrow hall.
int World::cell_props(int cx, int cz, Furn* out) const {
    int ccx, ccz;
    const float H = info().ceil_h;
    int n = 0;
    auto put = [&](FurnKind k, float x0, float z0, float x1, float z1, float y0, float y1, bool solid_, Vec2 face) {
        if (n >= MAX_FURN) return;
        Furn& f = out[n++];
        f = Furn{};
        f.kind = k; f.x0 = x0; f.z0 = z0; f.x1 = x1; f.z1 = z1; f.y0 = y0; f.y1 = y1; f.solid = solid_; f.face = face;
    };
    float x0 = cx * CELL, z0 = cz * CELL;
    int ocx, ocz;
    if (outpost(cx, cz, ocx, ocz)) { // M.E.G.: a tent, a cot, crates, sandbags, a floodlight, the flag
        const int lx = cx - ocx, lz = cz - ocz;
        if (lx == 0 && lz == 0) {
            put(FurnKind::Tent, x0 + 0.1f, z0 + 0.1f, x0 + CELL - 0.1f, z0 + CELL - 0.1f, 0, 2.3f, false, {0, 1});
            put(FurnKind::Cot, x0 + 0.5f, z0 + 0.45f, x0 + 2.0f, z0 + 1.15f, 0, 0.45f, true, {0, 1});
        } else if (lx == 1 && lz == 0) {
            put(FurnKind::Crate, x0 + 0.5f, z0 + 0.6f, x0 + 1.5f, z0 + 1.6f, 0, 0.8f, true, {-1, 0});
            put(FurnKind::Radio2, x0 + 1.6f, z0 + 0.7f, x0 + 2.1f, z0 + 1.0f, 0, 0.75f, true, {-1, 0});
        } else if (lx == -1 && lz == 0) {
            put(FurnKind::Floodlight, x0 + 0.9f, z0 + 0.9f, x0 + 1.6f, z0 + 1.6f, 0, 2.4f, false, {1, 0});
        } else if (lx == 0 && lz == -1) {
            put(FurnKind::Banner, x0 + 1.0f, z0 + 0.6f, x0 + 1.5f, z0 + 0.9f, 0, 3.0f, true, {0, 1});
        } else if (lx == -1 && lz == -1) {
            put(FurnKind::Crate, x0 + 1.0f, z0 + 1.0f, x0 + 2.0f, z0 + 2.0f, 0, 0.8f, true, {0, 1});
        } else if (lx == 1 && lz == 1) {
            put(FurnKind::Crate, x0 + 0.4f, z0 + 0.4f, x0 + 1.3f, z0 + 1.3f, 0, 0.8f, true, {0, 1});
        }
        // sandbags along part of the outside edge (gaps on the sides to walk in)
        if (lz == 1 && lx != 0) put(FurnKind::Sandbags, x0 + 0.1f, z0 + CELL - 0.55f, x0 + CELL - 0.1f, z0 + CELL - 0.1f, 0, 0.8f, true, {0, -1});
        if (lz == -1 && lx == 1) put(FurnKind::Sandbags, x0 + 0.1f, z0 + 0.1f, x0 + CELL - 0.1f, z0 + 0.55f, 0, 0.8f, true, {0, 1});
        if (lx == 1 && lz == -1) put(FurnKind::Sandbags, x0 + CELL - 0.55f, z0 + 0.6f, x0 + CELL - 0.1f, z0 + CELL - 0.1f, 0, 0.8f, true, {-1, 0});
        return n;
    }
    if (id_ == LevelId::Hub) { // the lobby: sofas, a table, a board with every level pinned to it
        if (cx == -2 && cz == -4) put(FurnKind::Couch, x0 + 0.2f, z0 + 0.15f, x0 + 2.3f, z0 + 1.0f, 0, 0.85f, true, {0, 1});
        if (cx == -2 && cz == -3) put(FurnKind::Table, x0 + 0.7f, z0 + 0.6f, x0 + 1.8f, z0 + 1.3f, 0, 0.45f, true, {0, 1});
        if (cx == 0 && cz == -7) put(FurnKind::KioskSign, x0 + 0.1f, z0 + 0.1f, x0 + 2.4f, z0 + 0.3f, 0.8f, 2.6f, false, {0, 1});
        if (cx == -6 && cz == 2) put(FurnKind::Rack, x0 + 0.1f, z0 + 0.2f, x0 + 0.6f, z0 + 2.3f, 0, 1.9f, true, {1, 0});
        if (cx == 6 && cz == -2) put(FurnKind::Cot, x0 + 0.4f, z0 + 0.5f, x0 + 2.1f, z0 + 1.2f, 0, 0.45f, true, {0, 1});
        return n;
    }
    if (camp(cx, cz, ccx, ccz)) {
        if (cx == ccx && cz == ccz) {
            put(FurnKind::Mattress, x0 + 0.3f, z0 + 0.4f, x0 + 2.2f, z0 + 1.3f, 0, 0.18f, false, {0, 1});
            put(FurnKind::Lantern, x0 + 2.0f, z0 + 1.6f, x0 + 2.25f, z0 + 1.85f, 0, 0.32f, false, {0, 1});
        } else if (cx == ccx + 1 && cz == ccz) {
            put(FurnKind::Boxes, x0 + 0.2f, z0 + 0.2f, x0 + 0.9f, z0 + 0.9f, 0, 0.9f, true, {1, 0});
        } else if (cx == ccx && cz == ccz + 1) {
            put(FurnKind::FireBarrel, x0 + 1.0f, z0 + 0.8f, x0 + 1.6f, z0 + 1.4f, 0, 0.9f, true, {0, 1});
        }
        return n;
    }
    { // Level 21 rooms: a door hung in the doorway, the room furnished
        int rbx, rbz;
        bool north;
        if (id_ == LevelId::L21 && door_room(cx, cz, rbx, rbz, north)) {
            int dz = north ? rbz * DOOR_GRID + 1 : rbz * DOOR_GRID + DOOR_GRID - 1;
            if (cx == rbx * DOOR_GRID + 2 && cz == dz) {
                float zl = north ? z0 : z0 + CELL, hw = WALL_T * 0.5f, in = north ? 1.0f : -1.0f;
                float xm = x0 + CELL * 0.5f;
                put(FurnKind::WallPiece, x0, zl - hw, xm - 0.55f, zl + hw, 0, H, true, {0, in});
                put(FurnKind::WallPiece, xm + 0.55f, zl - hw, x0 + CELL, zl + hw, 0, H, true, {0, in});
                put(FurnKind::Lintel, xm - 0.55f, zl - hw, xm + 0.55f, zl + hw, 2.0f, H, false, {0, in});
                float d0 = north ? zl + hw : zl - 0.95f, d1 = north ? zl + 0.95f : zl - hw;
                put(FurnKind::DoorLeaf, xm - 0.55f, d0, xm - 0.5f, d1, 0, 1.98f, true, {1, 0});
                out[n - 1].hinge = {xm - 0.55f, zl};
                out[n - 1].shut_dir = {1, 0};
                out[n - 1].open_dir = {0, in};
                out[n - 1].width = 1.1f;
                return n;
            }
        }
    }
    // ----- 1.1 set pieces
    if ((id_ == LevelId::TSG0 || id_ == LevelId::TSG2) && cx >= -1 && cx <= 1 && cz >= -1 && cz <= 1) {
        const float hw = WALL_T * 0.5f;
        if (cx == 0 && cz == -1) // the clock on the shack wall: the only warning before night
            put(FurnKind::Clock, x0 + 1.0f, z0 + hw + 0.005f, x0 + 1.5f, z0 + hw + 0.05f, 1.6f, 2.1f, false, {0, 1});
        if (cx == -1 && cz == -1) put(FurnKind::Mattress, x0 + 0.2f, z0 + 0.3f, x0 + 2.1f, z0 + 1.2f, 0, 0.18f, false, {0, 1});
        if (cx == 1 && cz == -1) put(FurnKind::Table, x0 + 0.6f, z0 + 0.3f, x0 + 1.9f, z0 + 1.05f, 0, 0.75f, true, {0, 1});
        if (cx == 1 && cz == 0) put(FurnKind::Chair, x0 + 1.0f, z0 + 0.6f, x0 + 1.48f, z0 + 1.08f, 0, 0.92f, true, {-1, 0});
        if (id_ == LevelId::TSG2 && cx == -1 && cz == 1) put(FurnKind::Stove, x0 + 0.3f, z0 + 1.6f, x0 + 0.95f, z0 + 2.25f, 0, 0.92f, true, {1, 0});
        if (id_ == LevelId::TSG2 && cx == 1 && cz == 1) put(FurnKind::Lantern, x0 + 1.9f, z0 + 1.9f, x0 + 2.15f, z0 + 2.15f, 0, 0.32f, false, {0, 1});
        return n;
    }
    if (id_ == LevelId::L188) {
        int c = court_cell(cx, cz);
        if (c == 0) { // the atrium
            int u = cx + HOTEL_HALF - 5, v = cz + HOTEL_HALF - 5;
            if (u == 6 && v == 6) // the fountain in the square
                put(FurnKind::Fountain, x0 + 0.3f, z0 + 0.3f, x0 + 2 * CELL - 0.3f, z0 + 2 * CELL - 0.3f, 0, 2.2f, true, {0, 1});
            else if ((u == 3 || u == 10) && (v == 3 || v == 10)) // a tree in each lawn
                put(FurnKind::Planter, x0 + 0.3f, z0 + 0.3f, x0 + 2.2f, z0 + 2.2f, 0, 0.6f, true, {0, 1});
            else if ((u == 5 && (v == 2 || v == 11)) || (v == 5 && (u == 2 || u == 11)))
                put(FurnKind::Bench, x0 + 0.4f, z0 + 1.0f, x0 + 2.1f, z0 + 1.5f, 0, 0.48f, true, {0, 1});
            else if ((u == 8 && (v == 2 || v == 11)) || (v == 8 && (u == 2 || u == 11)))
                put(FurnKind::Bench, x0 + 1.0f, z0 + 0.4f, x0 + 1.5f, z0 + 2.1f, 0, 0.48f, true, {1, 0});
            else if (u == 9 && v == 4) // a table, set for nobody
                put(FurnKind::Table2, x0 + 0.4f, z0 + 0.4f, x0 + 2.0f, z0 + 1.3f, 0, 0.76f, true, {0, 1});
            return n;
        }
        if (c == 1 && hash01(seed_, cx, cz, S_PROP + 30) < 0.07f) { // a plant or a bin in the corridor, against the wall
            static const int DXs[4] = {-1, 1, 0, 0}, DZs[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; k++) {
                if (court_cell(cx + DXs[k], cz + DZs[k]) == 1 || !wall_face(cx, cz, k)) continue;
                float px = x0 + CELL * 0.5f + DXs[k] * (CELL * 0.5f - 0.4f), pz = z0 + CELL * 0.5f + DZs[k] * (CELL * 0.5f - 0.4f);
                bool plant = hash3(seed_, cx, cz, S_PROP + 31) & 1;
                put(plant ? FurnKind::Plant : FurnKind::Bin, px - 0.27f, pz - 0.27f, px + 0.27f, pz + 0.27f, 0, plant ? 1.4f : 0.8f, true, {(float)-DXs[k], (float)-DZs[k]});
                break;
            }
        }
        return n;
    }
    if (id_ == LevelId::L33) { // the galleries: fountains and planters at the crossings, escalators, benches, kiosks, a food court
        int mx = pmod(cx, MALL_GRID), mz = pmod(cz, MALL_GRID);
        if (mx < 3 || mz < 3) {
            int bx = floor_div(cx, MALL_GRID), bz = floor_div(cz, MALL_GRID);
            float r = hash01(seed_, bx, bz, S_PROP + 40);
            bool food = hash01(seed_, bx, bz, S_PROP + 41) < 0.3f && !near_spawn(cx, cz, 2);
            if (mx == 1 && mz == 1 && !near_spawn(cx, cz, 2)) { // the crossing
                if (r < 0.3f) put(FurnKind::Fountain, x0 + 0.05f, z0 + 0.05f, x0 + CELL - 0.05f, z0 + CELL - 0.05f, 0, 1.9f, true, {0, 1});
                else if (r < 0.55f) put(FurnKind::GlassLift, x0 + 0.2f, z0 + 0.2f, x0 + CELL - 0.2f, z0 + CELL - 0.2f, 0, 9.5f, true, {0, 1}); // the glass lift in the rotunda
                else if (!food) put(FurnKind::Planter, x0 + 0.2f, z0 + 0.2f, x0 + CELL - 0.2f, z0 + CELL - 0.2f, 0, 0.7f, true, {0, 1});
                return n;
            }
            if (mz == 1 && mx == 4 && r > 0.6f) { // an escalator up to the gallery (it hasn't moved in years)
                put(FurnKind::Escalator, x0 + 0.1f, z0 + 0.6f, x0 + 4.7f, z0 + 1.9f, 0, 4.6f, true, {1, 0});
                return n;
            }
            if (mx == 1 && mz == 4 && r < 0.3f) {
                put(FurnKind::Escalator, x0 + 0.6f, z0 + 0.1f, x0 + 1.9f, z0 + 4.7f, 0, 4.6f, true, {0, 1});
                return n;
            }
            if (food && mx < 3 && mz < 3 && !(mx == 1 && mz == 1)) { // the food court: tables and chairs left as they were
                put(FurnKind::RoundTable, x0 + 0.7f, z0 + 0.7f, x0 + 1.8f, z0 + 1.8f, 0, 0.75f, true, {0, 1});
                put(FurnKind::Chair, x0 + 1.01f, z0 + 0.05f, x0 + 1.49f, z0 + 0.53f, 0, 0.92f, true, {0, 1});
                if (hash3(seed_, cx, cz, S_PROP + 42) & 1) put(FurnKind::Chair, x0 + 1.01f, z0 + 1.97f, x0 + 1.49f, z0 + 2.45f, 0, 0.92f, true, {0, -1});
                return n;
            }
            float q = hash01(seed_, cx, cz, S_PROP + 43);
            bool lane = (mz == 1 && mx >= 3) || (mx == 1 && mz >= 3);
            if (lane && q < 0.10f) { // a kiosk in the middle of the gallery, its shutter down
                put(FurnKind::Shutter, x0 + 0.5f, z0 + 0.6f, x0 + 2.0f, z0 + 1.9f, 0, 1.3f, true, {0, 1});
                put(FurnKind::KioskSign, x0 + 0.5f, z0 + 1.2f, x0 + 2.0f, z0 + 1.3f, 1.3f, 2.2f, false, {0, 1});
            } else if (lane && q < 0.22f) {
                put(FurnKind::Planter, x0 + 0.6f, z0 + 0.6f, x0 + 1.9f, z0 + 1.9f, 0, 0.6f, true, {0, 1});
            } else if (!lane && q < 0.12f) {
                bool ax = mz < 3 && mx >= 3;
                if (ax) put(FurnKind::Bench, x0 + 0.4f, z0 + 1.0f, x0 + 2.1f, z0 + 1.5f, 0, 0.48f, true, {0, 1});
                else put(FurnKind::Bench, x0 + 1.0f, z0 + 0.4f, x0 + 1.5f, z0 + 2.1f, 0, 0.48f, true, {1, 0});
            } else if (!lane && q < 0.17f) {
                put(FurnKind::Bin, x0 + 1.0f, z0 + 1.0f, x0 + 1.5f, z0 + 1.5f, 0, 0.8f, true, {0, 1});
            }
            return n;
        }
    }
    if (id_ == LevelId::L37) { // the poolrooms: loungers and palms on the deck, rings floating, ladders, the odd slide
        float cxm = x0 + CELL * 0.5f, czm = z0 + CELL * 0.5f;
        float dep = pool_depth(cxm, czm);
        float q = hash01(seed_, cx, cz, S_PROP + 50);
        if (near_spawn(cx, cz, 2)) return n;
        if (pmod(cx, 4) == 2 && pmod(cz, 4) == 2 && pillar(cx, cz) && hash01(seed_, cx, cz, S_PROP + 51) < 0.3f) { // a spiral stair round the column
            float gx = cx * CELL, gz = cz * CELL;
            put(FurnKind::SpiralStair, gx - 1.75f, gz - 1.75f, gx + 1.75f, gz + 1.75f, 0, info().ceil_h, true, {0, 1});
            return n;
        }
        if (dep > 0.9f) { // out in the water
            if (q < 0.10f) put(FurnKind::Float, cxm - 0.5f, czm - 0.5f, cxm + 0.5f, czm + 0.5f, -0.18f, -0.02f, false, {0, 1});
            return n;
        }
        if (pool(cx, cz)) { // wading: a ladder rail at a deep edge now and then, a ring drifting
            if (q < 0.08f && dep > 0.6f) put(FurnKind::Ladder, cxm - 0.3f, czm - 0.05f, cxm + 0.3f, czm + 0.05f, -dep, 1.0f, false, {0, 1});
            else if (q > 0.97f) put(FurnKind::Float, cxm - 0.5f, czm - 0.5f, cxm + 0.5f, czm + 0.5f, -0.1f, 0.02f, false, {0, 1});
            return n;
        }
        // on the deck: is there a pool next door?
        static const int DXs[4] = {-1, 1, 0, 0}, DZs[4] = {0, 0, -1, 1};
        int wet = -1;
        for (int k = 0; k < 4; k++) if (pool(cx + DXs[k], cz + DZs[k]) && pool_depth((cx + DXs[k] + 0.5f) * CELL, (cz + DZs[k] + 0.5f) * CELL) > 1.0f) wet = k;
        if (wet >= 0 && q < 0.06f) { // a slide down into the deep end
            Vec2 face{(float)DXs[wet], (float)DZs[wet]};
            put(FurnKind::Slide, x0 + 0.2f, z0 + 0.2f, x0 + CELL - 0.2f, z0 + CELL - 0.2f, 0, 3.6f, true, face);
            return n;
        }
        if (wet >= 0 && q < 0.28f) {
            Vec2 face{(float)DXs[wet], (float)DZs[wet]};
            bool ax = face.x == 0;
            if (ax) put(FurnKind::Lounger, cxm - 0.35f, czm - 0.95f, cxm + 0.35f, czm + 0.95f, 0, 0.45f, true, face);
            else put(FurnKind::Lounger, cxm - 0.95f, czm - 0.35f, cxm + 0.95f, czm + 0.35f, 0, 0.45f, true, face);
            return n;
        }
        if (wet >= 0 && q < 0.32f) {
            put(FurnKind::LifeguardChair, cxm - 0.45f, czm - 0.45f, cxm + 0.45f, czm + 0.45f, 0, 2.2f, true, {(float)DXs[wet], (float)DZs[wet]});
            return n;
        }
        if (q > 0.93f) { put(FurnKind::Palm, cxm - 0.35f, czm - 0.35f, cxm + 0.35f, czm + 0.35f, 0, 2.6f, true, {0, 1}); return n; }
        if (q > 0.90f) { put(FurnKind::Bench, cxm - 0.85f, czm - 0.25f, cxm + 0.85f, czm + 0.25f, 0, 0.48f, true, {0, 1}); return n; }
        return n;
    }
    if (id_ == LevelId::L24) { // the moon: rounded rocks, the odd lander left behind, an antenna, a flag
        if (near_spawn(cx, cz, 2) || solid(cx, cz)) return n;
        float q = hash01(seed_, cx, cz, S_PROP + 70);
        float cxm = x0 + CELL * 0.5f + (hash01(seed_, cx, cz, S_PROP + 71) - 0.5f) * 1.0f, czm = z0 + CELL * 0.5f + (hash01(seed_, cx, cz, S_PROP + 72) - 0.5f) * 1.0f;
        if (q < 0.0025f) { put(FurnKind::Lander, x0 + 0.1f, z0 + 0.1f, x0 + CELL - 0.1f, z0 + CELL - 0.1f, 0, 3.0f, true, {0, 1}); return n; }
        if (q < 0.004f) { put(FurnKind::Antenna, cxm - 0.4f, czm - 0.4f, cxm + 0.4f, czm + 0.4f, 0, 3.5f, true, {0, 1}); return n; }
        if (q < 0.005f) { put(FurnKind::Flag, cxm - 0.05f, czm - 0.05f, cxm + 0.05f, czm + 0.05f, 0, 2.2f, true, {1, 0}); return n; }
        if (q < 0.12f) { // a rock, small to big (the big ones are in your way)
            float r = 0.15f + std::pow(hash01(seed_, cx, cz, S_PROP + 73), 3.0f) * 1.1f;
            put(FurnKind::Boulder, cxm - r, czm - r, cxm + r, czm + r, 0, r * 1.3f, r > 0.45f, {0, 1});
        }
        return n;
    }
    if (id_ == LevelId::TheEnd) { // THE END: tipped shelves, reading tables, carts of nothing
        if (near_spawn(cx, cz, 1) || solid(cx, cz)) return n;
        float q = hash01(seed_, cx, cz, S_PROP + 80);
        float cxm = x0 + CELL * 0.5f, czm = z0 + CELL * 0.5f;
        if (q < 0.035f) { // a free-standing shelf unit, turned at odd angles in the open floor
            bool ax = hash3(seed_, cx, cz, S_PROP + 81) & 1;
            if (ax) put(FurnKind::Bookshelf, cxm - 0.9f, czm - 0.3f, cxm + 0.9f, czm + 0.3f, 0, 1.6f, true, {0, 1});
            else put(FurnKind::Bookshelf, cxm - 0.3f, czm - 0.9f, cxm + 0.3f, czm + 0.9f, 0, 1.6f, true, {1, 0});
        } else if (q < 0.045f) {
            put(FurnKind::Table, cxm - 0.65f, czm - 0.38f, cxm + 0.65f, czm + 0.38f, 0, 0.75f, true, {0, 1});
            put(FurnKind::Chair, cxm - 0.24f, czm + 0.45f, cxm + 0.24f, czm + 0.93f, 0, 0.92f, true, {0, -1});
        } else if (q < 0.052f) {
            put(FurnKind::Boxes, cxm - 0.4f, czm - 0.4f, cxm + 0.4f, czm + 0.4f, 0, 0.8f, true, {0, 1});
        }
        return n;
    }
    if (id_ == LevelId::LRun) { // junk in the way: desks, boxes, chairs, barrels pushed against the walls
        if (cz > -6 || cz <= -RUN_LEN + 2 || pmod(cz, 3) == 0 || cx == 0 || solid(cx, cz)) return n;
        float q = hash01(seed_, cx, cz, S_PROP + 60);
        if (q > 0.45f) return n;
        static const FurnKind JUNK[] = {FurnKind::Boxes, FurnKind::Barrel, FurnKind::Table, FurnKind::Chair, FurnKind::Cabinet, FurnKind::Cone};
        FurnKind k = JUNK[hash3(seed_, cx, cz, S_PROP + 61) % 6u];
        float wx = cx < 0 ? x0 + 0.1f : x0 + CELL - 0.85f; // against the side wall
        float zz = z0 + 0.4f + hash01(seed_, cx, cz, S_PROP + 62) * 1.2f;
        float h = k == FurnKind::Cabinet ? 1.32f : (k == FurnKind::Table ? 0.75f : (k == FurnKind::Cone ? 0.72f : 0.9f));
        put(k, wx, zz, wx + 0.75f, zz + 0.75f, 0, h, true, {cx < 0 ? 1.0f : -1.0f, 0});
        return n;
    }
    if (id_ == LevelId::L94) {
        int rx = floor_div(cx + TOWN_REGION / 2, TOWN_REGION), rz = floor_div(cz + TOWN_REGION / 2, TOWN_REGION);
        if (cx == rx * TOWN_REGION + TOWN_GRID && cz == rz * TOWN_REGION && town(cx, cz)) { // the square: almond water fountain
            put(FurnKind::Fountain, x0 + 0.05f, z0 + 0.05f, x0 + CELL - 0.05f, z0 + CELL - 0.05f, 0, 1.9f, true, {0, 1});
            return n;
        }
        if (!town(cx, cz) && !hill(cx, cz) && !near_spawn(cx, cz, 4) && hash01(seed_, cx, cz, S_PROP + 20) < 0.006f) {
            // a room that forgot the rest of its house: one wall, and the furniture beside it
            const float hw = WALL_T * 0.5f;
            put(FurnKind::WallPiece, x0, z0 + 0.3f - hw, x0 + CELL, z0 + 0.3f + hw, 0, 2.6f, true, {0, 1});
            put(FurnKind::Painting, x0 + 0.8f, z0 + 0.3f + hw + 0.005f, x0 + 1.6f, z0 + 0.3f + hw + 0.03f, 1.3f, 1.9f, false, {0, 1});
            put(FurnKind::Couch, x0 + 0.3f, z0 + 0.45f, x0 + 2.2f, z0 + 1.25f, 0, 0.85f, true, {0, 1});
            put(FurnKind::Lamp, x0 + 2.1f, z0 + 1.6f, x0 + 2.34f, z0 + 1.84f, 0, 1.38f, false, {0, 1});
            return n;
        }
    }
    // what turns up where
    struct Pick { FurnKind k; float w; };
    const Pick* list = nullptr;
    int count = 0;
    float rate = 0;
    int fm = floor_mat(cx, cz);
    static const Pick OFFICE[] = {{FurnKind::Chair, 3}, {FurnKind::Table, 1}, {FurnKind::Cabinet, 2}, {FurnKind::Boxes, 1}, {FurnKind::Plant, 1}, {FurnKind::Bin, 1}};
    static const Pick LOBBY[] = {{FurnKind::Chair, 2}, {FurnKind::Table, 1}, {FurnKind::Cabinet, 1}, {FurnKind::Boxes, 1}, {FurnKind::Plant, 0.5f}, {FurnKind::Bin, 1}};
    static const Pick WAREHOUSE[] = {{FurnKind::Boxes, 3}, {FurnKind::Barrel, 2}, {FurnKind::Pallet, 2}, {FurnKind::Cone, 1}, {FurnKind::Shelf, 1}};
    static const Pick TUNNEL[] = {{FurnKind::Barrel, 2}, {FurnKind::Locker, 1}, {FurnKind::Boxes, 1}};
    static const Pick BALLROOM[] = {{FurnKind::RoundTable, 3}, {FurnKind::Chair, 2}};
    static const Pick CORRIDOR[] = {{FurnKind::Plant, 1}, {FurnKind::Bin, 0.5f}};
    static const Pick CAVE[] = {{FurnKind::Rubble, 2}, {FurnKind::Barrel, 0.3f}};
    static const Pick STREET[] = {{FurnKind::Car, 3}, {FurnKind::Bin, 1}};
    static const Pick GRASS[] = {{FurnKind::Tree, 3}, {FurnKind::Mailbox, 1}};
    static const Pick ATTIC[] = {{FurnKind::Boxes, 3}, {FurnKind::Chair, 1}, {FurnKind::Mattress, 1}, {FurnKind::Lantern, 0.3f}};
    static const Pick PARKING[] = {{FurnKind::Cone, 3}, {FurnKind::Barrel, 1}, {FurnKind::Bin, 1}, {FurnKind::Pallet, 1}};
    static const Pick MALL[] = {{FurnKind::Bench, 2}, {FurnKind::Plant, 3}, {FurnKind::Bin, 1}};
    static const Pick SHOP[] = {{FurnKind::Counter, 1}, {FurnKind::Boxes, 2}, {FurnKind::Shelf, 1}};
    static const Pick AIRPORT[] = {{FurnKind::Bench, 1}, {FurnKind::Bin, 1}, {FurnKind::Plant, 1}};
    static const Pick RUINS[] = {{FurnKind::Rubble, 3}, {FurnKind::Barrel, 0.5f}};
    static const Pick PARTY[] = {{FurnKind::Table, 1}, {FurnKind::Chair, 2}, {FurnKind::Boxes, 1}};
    static const Pick GREY[] = {{FurnKind::Chair, 1}, {FurnKind::Cabinet, 0.5f}};
    static const Pick HOUSE[] = {{FurnKind::Chair, 2}, {FurnKind::Table, 1}, {FurnKind::Boxes, 1}};
    static const Pick POOL[] = {{FurnKind::Bench, 1}};
#define USE(L, r) do { list = L; count = (int)(sizeof(L) / sizeof(L[0])); rate = r; } while (0)
    switch (id_) {
        case LevelId::L0: USE(LOBBY, 0.08f); break;
        case LevelId::L1: USE(WAREHOUSE, 0.12f); break;
        case LevelId::L2: USE(TUNNEL, 0.07f); break;
        case LevelId::L3: USE(TUNNEL, 0.08f); break;
        case LevelId::L10: { static const Pick FIELD[] = {{FurnKind::Scarecrow, 1}}; if (fm == 0) USE(FIELD, 0.006f); else USE(WAREHOUSE, 0.02f); break; }
        case LevelId::L13: if (fm == 0) USE(CORRIDOR, 0.05f); break;
        case LevelId::L4: USE(OFFICE, 0.14f); break;
        case LevelId::L5: case LevelId::TSG1:
            if (fm == 1) USE(BALLROOM, 0.35f); else USE(CORRIDOR, 0.05f);
            break;
        case LevelId::L6: USE(LOBBY, 0.07f); break;
        case LevelId::L7: if (std::abs(cx) <= 4 && std::abs(cz) <= 4) USE(HOUSE, 0.18f); break;
        case LevelId::L8: USE(CAVE, 0.06f); break;
        case LevelId::L9: if (fm == 1) USE(STREET, 0.05f); else if (fm == 0) USE(GRASS, 0.10f); break;
        case LevelId::L11: if (fm == 2) USE(STREET, 0.05f); else USE(GRASS, 0.03f); break;
        case LevelId::L19: USE(ATTIC, 0.12f); break;
        case LevelId::L21: {
            int rbx, rbz;
            bool rn;
            if (door_room(cx, cz, rbx, rbz, rn)) USE(OFFICE, 0.55f); else USE(CORRIDOR, 0.05f);
            break;
        }
        case LevelId::L22: USE(PARKING, 0.08f); break;
        case LevelId::L33: if (fm == 1) USE(SHOP, 0.45f); else USE(MALL, 0.12f); break;
        case LevelId::L36: USE(AIRPORT, 0.06f); break;
        case LevelId::L37: USE(POOL, 0.03f); break;
        case LevelId::L51: USE(RUINS, 0.10f); break;
        case LevelId::LFun: USE(PARTY, 0.10f); break;
        case LevelId::TSGFar: case LevelId::TSGHope: USE(GREY, 0.04f); break;
        case LevelId::L70: {
            static const Pick YARD[] = {{FurnKind::Snowman, 1}, {FurnKind::Pine, 1.5f}, {FurnKind::Mailbox, 0.6f}};
            if (fm == 1) USE(STREET, 0.06f); else if (fm == 0) USE(YARD, 0.10f);
            break;
        }
        case LevelId::L94: {
            static const Pick TOWNROAD[] = {{FurnKind::Car, 2}, {FurnKind::Siren, 1}, {FurnKind::Bin, 1}};
            static const Pick TOWNYARD[] = {{FurnKind::Bush, 2}, {FurnKind::Mailbox, 1}, {FurnKind::Tree, 1}};
            static const Pick HILLS[] = {{FurnKind::Cottage, 1.2f}, {FurnKind::Fence, 1.4f}, {FurnKind::PowerPole, 0.7f}, {FurnKind::Bush, 0.8f},
                                         {FurnKind::Tree, 0.5f}, {FurnKind::WaterTower, 0.12f}};
            if (!town(cx, cz)) USE(HILLS, 0.07f); else if (fm == 1) USE(TOWNROAD, 0.05f); else if (fm == 0) USE(TOWNYARD, 0.10f);
            break;
        }
        case LevelId::TSG0: { static const Pick JUNGLE[] = {{FurnKind::Bush, 4}, {FurnKind::Tree, 1}, {FurnKind::Rubble, 0.4f}}; USE(JUNGLE, 0.24f); break; }
        case LevelId::TSG2: { static const Pick FOREST[] = {{FurnKind::Bush, 2}, {FurnKind::Tree, 1.2f}}; USE(FOREST, 0.12f); break; }
        case LevelId::TSG3: {
            static const Pick ISLAND[] = {{FurnKind::Boat, 1}, {FurnKind::Bush, 1}, {FurnKind::Barrel, 1}};
            static const Pick SEA[] = {{FurnKind::Clock, 1}};
            if (fm == 0) USE(ISLAND, 0.10f); else USE(SEA, 0.03f); // clocks float everywhere
            break;
        }
        case LevelId::TSG4: {
            static const Pick WALK[] = {{FurnKind::Bin, 1}, {FurnKind::Mailbox, 0.5f}, {FurnKind::Bench, 0.5f}};
            if (fm == 1) USE(STREET, 0.05f); else if (fm == 2) USE(WALK, 0.05f);
            break;
        }
        default: break;
    }
#undef USE
    if (!list || hash01(seed_, cx, cz, S_PROP) >= rate) return 0; // most cells: nothing (and cheap to find out)
    if (near_spawn(cx, cz, 1) || solid(cx, cz) || pole(cx, cz) || (water(cx, cz) && id_ != LevelId::TSG3) || exit_cell(cx, cz)) return 0;
    // (items find their own spot around props, or on top of them: see item_spot)
    // which sides have a wall to stand against, and is this a narrow hall?
    int wall_side = -1, open_ways = 0;
    static const int DX[4] = {-1, 1, 0, 0}, DZ[4] = {0, 0, -1, 1};
    for (int k = 0; k < 4; k++) {
        if (wall_face(cx, cz, k)) { if (wall_side < 0 || hash3(seed_, cx, cz, S_PROP + 2 + k) & 1) wall_side = k; }
        else if (can_step(cx, cz, DX[k], DZ[k])) open_ways++;
    }
    float tw = 0;
    for (int k = 0; k < count; k++) tw += list[k].w;
    float t = hash01(seed_, cx, cz, S_PROP + 1) * tw;
    FurnKind kind = list[0].k;
    for (int k = 0; k < count; k++) { t -= list[k].w; if (t < 0) { kind = list[k].k; break; } }
    // size of each kind: width along the wall, depth out of it, height
    float w = 0.6f, d = 0.6f, h = 0.9f;
    bool needs_wall = false, solid_ = true;
    switch (kind) {
        case FurnKind::Chair: w = 0.48f; d = 0.48f; h = 0.92f; break;
        case FurnKind::Table: w = 1.3f; d = 0.75f; h = 0.75f; break;
        case FurnKind::RoundTable: w = 1.1f; d = 1.1f; h = 0.75f; break;
        case FurnKind::Cabinet: w = 0.5f; d = 0.62f; h = 1.32f; needs_wall = true; break;
        case FurnKind::Boxes: w = 0.75f; d = 0.75f; h = 0.5f + 0.6f * hash01(seed_, cx, cz, S_PROP + 9); break;
        case FurnKind::Barrel: w = 0.62f; d = 0.62f; h = 0.92f; break;
        case FurnKind::FireBarrel: w = 0.62f; d = 0.62f; h = 0.92f; break;
        case FurnKind::Cone: w = 0.4f; d = 0.4f; h = 0.72f; break;
        case FurnKind::Bench: w = 1.7f; d = 0.5f; h = 0.48f; break;
        case FurnKind::Plant: w = 0.55f; d = 0.55f; h = 1.4f; break;
        case FurnKind::Shelf: w = 1.4f; d = 0.45f; h = std::min(1.9f, H - 0.2f); needs_wall = true; break;
        case FurnKind::Bin: w = 0.5f; d = 0.5f; h = 0.8f; break;
        case FurnKind::Locker: w = 0.9f; d = 0.5f; h = std::min(1.9f, H - 0.2f); needs_wall = true; break;
        case FurnKind::Pallet: w = 1.2f; d = 1.0f; h = 0.15f; solid_ = false; break;
        case FurnKind::Rubble: w = 1.4f; d = 1.2f; h = 0.6f; break;
        case FurnKind::Tree: w = 0.5f; d = 0.5f; h = 4.5f; break;
        case FurnKind::Mattress: w = 1.9f; d = 0.9f; h = 0.18f; solid_ = false; break;
        case FurnKind::Lantern: w = 0.25f; d = 0.25f; h = 0.32f; solid_ = false; break;
        case FurnKind::Counter: w = 1.8f; d = 0.6f; h = 1.0f; needs_wall = true; break;
        case FurnKind::Car: w = 1.8f; d = 2.3f; h = 1.4f; break;
        case FurnKind::Mailbox: w = 0.35f; d = 0.35f; h = 1.15f; break;
        case FurnKind::Scarecrow: w = 0.6f; d = 0.3f; h = 2.0f; break;
        case FurnKind::Snowman: w = 0.7f; d = 0.7f; h = 1.6f; break;
        case FurnKind::Bush: w = 1.1f; d = 1.0f; h = 0.95f; solid_ = false; break;  // you push through foliage
        case FurnKind::Siren: w = 0.3f; d = 0.3f; h = 3.4f; break;
        case FurnKind::Boat: w = 1.5f; d = 3.0f; h = 1.0f; break;
        case FurnKind::Clock: w = 0.8f; d = 0.7f; h = 1.2f; solid_ = false; break;  // floating in the sea
        case FurnKind::Pine: w = 0.7f; d = 0.7f; h = 4.0f; break;
        case FurnKind::Cottage: w = 2.2f; d = 2.2f; h = 3.6f; break;     // a little house up on a hill
        case FurnKind::Fence: w = 2.4f; d = 0.12f; h = 0.95f; break;     // a run of white picket fence
        case FurnKind::PowerPole: w = 0.3f; d = 0.3f; h = 7.5f; break;
        case FurnKind::WaterTower: w = 2.2f; d = 2.2f; h = 9.0f; break;
        default: break;
    }
    if (needs_wall && wall_side < 0) return 0;
    if (kind == FurnKind::Tree || kind == FurnKind::Pine || kind == FurnKind::Cottage || kind == FurnKind::WaterTower) {
        // big things need room around them: no poking through a house, a tower or a wall
        for (int b = -1; b <= 1; b++)
            for (int a = -1; a <= 1; a++)
                if (solid(cx + a, cz + b) || indoors(cx + a, cz + b) || exit_cell(cx + a, cz + b) || pole(cx + a, cz + b)) return 0;
        if (wall_side >= 0) return 0;
    }
    if (open_ways < 3 && (wall_side < 0 || d > 0.62f)) return 0; // keep narrow halls clear
    float jit = hash01(seed_, cx, cz, S_PROP + 3) - 0.5f;
    float cxm = x0 + CELL * 0.5f, czm = z0 + CELL * 0.5f;
    Vec2 face{0, 1};
    float px = cxm + jit * 0.6f, pz = czm + (hash01(seed_, cx, cz, S_PROP + 4) - 0.5f) * 0.6f;
    bool along_x = true; // the long side runs along x
    if (wall_side >= 0 && (needs_wall || kind == FurnKind::Bench || kind == FurnKind::Boxes || kind == FurnKind::Plant ||
                           kind == FurnKind::Bin || kind == FurnKind::Chair || kind == FurnKind::Barrel)) {
        const float gap = WALL_T * 0.5f + 0.03f + d * 0.5f;
        switch (wall_side) {
            case 0: px = x0 + gap; pz = czm + jit; face = {1, 0}; along_x = false; break;
            case 1: px = x0 + CELL - gap; pz = czm + jit; face = {-1, 0}; along_x = false; break;
            case 2: pz = z0 + gap; px = cxm + jit; face = {0, 1}; break;
            default: pz = z0 + CELL - gap; px = cxm + jit; face = {0, -1}; break;
        }
    } else if (kind == FurnKind::Car) { // parked along the road
        along_x = hash3(seed_, cx, cz, S_PROP + 5) & 1;
        px = cxm; pz = czm;
        face = along_x ? Vec2{1, 0} : Vec2{0, 1};
    } else if (hash3(seed_, cx, cz, S_PROP + 6) & 1) { // free-standing: some turn sideways
        along_x = false;
        face = {1, 0};
    }
    float hx = (along_x ? w : d) * 0.5f, hz = (along_x ? d : w) * 0.5f;
    if (kind == FurnKind::Car) { hx = along_x ? 1.15f : 0.88f; hz = along_x ? 0.88f : 1.15f; }
    put(kind, px - hx, pz - hz, px + hx, pz + hz, 0, h, solid_, face);
    if (kind == FurnKind::Chair && list == BALLROOM) { // chairs come in pairs around tables
        float ox = face.x != 0 ? 0 : 0.7f, oz = face.x != 0 ? 0.7f : 0;
        put(kind, px - hx + ox, pz - hz + oz, px + hx + ox, pz + hz + oz, 0, h, true, face);
    }
    return n;
}

// ------------------------------------------------------------------ exits + items
bool World::wall_face(int cx, int cz, int side, float* offset) const {
    static const int DX[4] = {-1, 1, 0, 0}, DZ[4] = {0, 0, -1, 1};
    bool thin = side == 0 ? wall_x(cx, cz) : side == 1 ? wall_x(cx + 1, cz) : side == 2 ? wall_z(cx, cz) : wall_z(cx, cz + 1);
    if (thin) {
        if (offset) *offset = WALL_T * 0.5f;
        return info().wall_h >= 2.3f || id_ == LevelId::L4; // office: the door's wall section goes up to the ceiling
    }
    int nx = cx + DX[side], nz = cz + DZ[side];
    if (id_ != LevelId::L1 && solid(nx, nz) && block_h(nx, nz) >= 2.3f) { // not on a stack of crates
        if (offset) *offset = 0;
        return true;
    }
    return false;
}

// One chance per 12x12-cell region (fewer on harder difficulties). The door is set into a
// real wall near a random spot in the region, so it has to be found, not spotted.
float World::exit_chance() const {
    if (id_ == LevelId::TheEnd) return diff().exits * 0.45f; // the last door is hard to find
    return id_ == LevelId::TSGEnd ? 0.8f : diff().exits;   // TH3 3ND is full of doors (all of them lead to THE END)
}

bool World::exit_for_region(int rx, int rz, ExitDoor& out) const {
    int keep_away = diff_ >= Difficulty::Hard ? 2 : 1;
    if (std::abs(rx) <= keep_away && std::abs(rz) <= keep_away) return false; // not right next to spawn
    if (hash01(seed_, rx, rz, S_EXIT) > exit_chance()) return false;
    int ox = 2 + (int)(hash3(seed_, rx, rz, S_EXIT + 1) % 8u);
    int oz = 2 + (int)(hash3(seed_, rx, rz, S_EXIT + 2) % 8u);
    int first_side = (int)(hash3(seed_, rx, rz, S_EXIT + 3) % 4u);
    int bx = rx * EXIT_REGION - EXIT_REGION / 2, bz = rz * EXIT_REGION - EXIT_REGION / 2;
    const ExitStyle style = exit_style();
    if (style != ExitStyle::Door) { // stands free in an open cell: a hole, a clock, an angel, a lone door
        for (int r = 0; r <= 4; r++)
            for (int dz = -r; dz <= r; dz++)
                for (int dx = -r; dx <= r; dx++) {
                    if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                    int cx = bx + ox + dx, cz = bz + oz + dz;
                    if (solid(cx, cz) || pole(cx, cz)) continue;
                    if (style == ExitStyle::Hole && water(cx, cz)) continue; // nobody digs in a lake
                    out.cx = cx;
                    out.cz = cz;
                    out.side = -1;
                    out.normal = {0, 1};
                    out.pos = {(cx + 0.5f) * CELL, (cz + 0.5f) * CELL};
                    return true;
                }
        return false;
    }
    for (int r = 0; r <= 4; r++)
        for (int dz = -r; dz <= r; dz++)
            for (int dx = -r; dx <= r; dx++) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                int cx = bx + ox + dx, cz = bz + oz + dz;
                if (solid(cx, cz) || pole(cx, cz)) continue;
                for (int k = 0; k < 4; k++) {
                    int side = (first_side + k) % 4;
                    float off;
                    if (!wall_face(cx, cz, side, &off)) continue;
                    static const int SX[4] = {-1, 1, 0, 0}, SZ[4] = {0, 0, -1, 1};
                    if (id_ == LevelId::L94 && hill(cx + SX[side], cz + SZ[side])) continue; // doors are in houses, not hills
                    out.cx = cx;
                    out.cz = cz;
                    out.side = side;
                    static const float NX[4] = {1, -1, 0, 0}, NZ[4] = {0, 0, 1, -1};
                    out.normal = {NX[side], NZ[side]};
                    float x = (cx + 0.5f) * CELL, z = (cz + 0.5f) * CELL;
                    if (side == 0) x = cx * CELL + off;
                    if (side == 1) x = (cx + 1) * CELL - off;
                    if (side == 2) z = cz * CELL + off;
                    if (side == 3) z = (cz + 1) * CELL - off;
                    out.pos = {x, z};
                    return true;
                }
            }
    return false;
}

bool World::exit_door(int cx, int cz, ExitDoor* out) const {
    if (id_ == LevelId::L188) { // the hotel's way out: doors in the outside walls of a few of the outer rooms
        int lx = cx + HOTEL_HALF, lz = cz + HOTEL_HALF;
        if (lx < 0 || lx > 23 || lz < 0 || lz > 23) return false;
        int side = lx == 0 ? 0 : (lx == 23 ? 1 : (lz == 0 ? 2 : (lz == 23 ? 3 : -1)));
        if (side < 0 || court_cell(cx, cz) != 2) return false;
        int b = side < 2 ? lz : lx;
        // one exit on each of 3 sides (2 on Hard and up), in a guest room somewhere along that side
        int want = diff_ >= Difficulty::Hard ? 2 : 3;
        int s0 = (int)(hash3(seed_, 0, 188, S_EXIT + 9) % 4u);
        int k = (side - s0 + 4) % 4;
        if (k >= want) return false;
        int start = 2 + (int)(hash3(seed_, k, 188, S_EXIT + 10) % 20u), pick = -1;
        for (int t = 0; t < 20 && pick < 0; t++) {
            int bb = 2 + (start - 2 + t) % 20;
            int ex = side == 0 ? 0 : side == 1 ? 23 : bb, ez = side == 2 ? 0 : side == 3 ? 23 : bb;
            if (court_cell(ex - HOTEL_HALF, ez - HOTEL_HALF) == 2) pick = bb;
        }
        if (pick != b) return false;
        if (out) {
            static const float NX[4] = {1, -1, 0, 0}, NZ[4] = {0, 0, 1, -1};
            out->cx = cx; out->cz = cz; out->side = side;
            out->normal = {NX[side], NZ[side]};
            float off = 0;
            wall_face(cx, cz, side, &off);
            float x = (cx + 0.5f) * CELL, z = (cz + 0.5f) * CELL;
            if (side == 0) x = cx * CELL + off;
            if (side == 1) x = (cx + 1) * CELL - off;
            if (side == 2) z = cz * CELL + off;
            if (side == 3) z = (cz + 1) * CELL - off;
            out->pos = {x, z};
        }
        return true;
    }
    if (id_ == LevelId::LRun) { // one door, at the very end of the corridor
        if (cx != 0 || cz != -RUN_LEN) return false;
        if (out) { out->cx = 0; out->cz = -RUN_LEN; out->side = 2; out->normal = {0, 1}; out->pos = {CELL * 0.5f, -RUN_LEN * CELL}; }
        return true;
    }
    int rx = floor_div(cx + EXIT_REGION / 2, EXIT_REGION), rz = floor_div(cz + EXIT_REGION / 2, EXIT_REGION);
    if (std::abs(rx) <= 1 && std::abs(rz) <= 1) return false;
    if (hash01(seed_, rx, rz, S_EXIT) > exit_chance()) return false; // cheap early out
    ExitDoor d;
    if (!exit_for_region(rx, rz, d) || d.cx != cx || d.cz != cz) return false;
    if (out) *out = d;
    return true;
}

bool World::exit_on_edge(int gx, int gz, bool vertical) const {
    ExitDoor d;
    if (vertical) // line x = gx, cell row gz: the door is in cell (gx, gz) facing west or (gx-1, gz) facing east
        return (exit_door(gx, gz, &d) && d.side == 0) || (exit_door(gx - 1, gz, &d) && d.side == 1);
    return (exit_door(gx, gz, &d) && d.side == 2) || (exit_door(gx, gz - 1, &d) && d.side == 3);
}

ItemType World::item(int cx, int cz) const {
    if (near_spawn(cx, cz, 0) || exit_cell(cx, cz) || pole(cx, cz)) return ItemType::None;
    if (solid(cx, cz) && !(id_ == LevelId::L4 && block_h(cx, cz) < 1.0f)) return ItemType::None; // (office desks hold things)
    if (cx < -32000 || cx > 32000 || cz < -32000 || cz > 32000) return ItemType::None; // id range
    // a flashlight waits right next to spawn where you'll need one
    if (cx == 1 && cz == 1 && (id_ == LevelId::L0 || id_ == LevelId::L6 || id_ == LevelId::L8 || id_ == LevelId::L19 ||
                               id_ == LevelId::L51 || id_ == LevelId::TSGFar || id_ == LevelId::TSGHope || id_ == LevelId::TSG4)) return ItemType::Flashlight;
    int ocx, ocz;
    if (outpost(cx, cz, ocx, ocz)) { // M.E.G. supplies on the crates
        if (cx == ocx + 1 && cz == ocz) return ItemType::AlmondWater;
        if (cx == ocx - 1 && cz == ocz - 1) return hash01(seed_, ocx, ocz, S_CAMP + 43) < 0.5f ? ItemType::Batteries : ItemType::MedKit;
        if (cx == ocx + 1 && cz == ocz + 1 && hash01(seed_, ocx, ocz, S_CAMP + 44) < 0.35f) return ItemType::Shells;
        return ItemType::None;
    }
    if (id_ == LevelId::Hub) return ItemType::None;
    int ccx, ccz;
    if (camp(cx, cz, ccx, ccz)) { // supplies the wanderers left behind
        if (cx == ccx + 1 && cz == ccz + 1) return ItemType::AlmondWater;
        if (cx == ccx - 1 && cz == ccz) {
            static const ItemType STASH[] = {ItemType::MothJelly, ItemType::MedKit, ItemType::RoyalRation, ItemType::GlowStick,
                                             ItemType::Flashlight, ItemType::SuperAlmond, ItemType::FlashBulb, ItemType::Firesalt};
            return STASH[hash3(seed_, ccx, ccz, S_CAMP + 3) % 8u];
        }
        if (cx == ccx && cz == ccz) return ItemType::None; // the bedroll
    }
    // 1.2: things don't just lie in the middle of the floor. They're left on desks, tables,
    // shelves and beds, so rooms and furniture are worth searching and bare halls mostly aren't.
    float rate = info().item_rate * diff().items * 0.8f;
    if (near_spawn(cx, cz, 6)) rate *= 2.0f;
    if (hash01(seed_, cx, cz, S_ITEM) >= rate * (id_ == LevelId::L188 ? 12.0f : 4.0f)) return ItemType::None; // cheap early out (the biggest boost is x4)
    {
        Vec2 ip;
        float iy;
        bool on_furniture = item_spot(cx, cz, ip, iy) && iy > floor_y(ip.x, ip.z) + 0.1f;
        bool l4_desk = id_ == LevelId::L4 && solid(cx, cz);
        if (id_ == LevelId::L188) rate *= 3.0f; // (one hotel, not an endless one: more to find in it)
        if (on_furniture || l4_desk) rate *= guest_room(cx, cz) ? 4.0f : 3.0f;
        else if (guest_room(cx, cz)) rate *= 1.2f;
        else rate *= info().outdoor ? 0.6f : 0.4f;
    }
    if (hash01(seed_, cx, cz, S_ITEM) >= rate) return ItemType::None;
    // weights per item, with a few level flavours
    float w[ITEM_COUNT] = {0, 30, 15, 6, 10, 6, 6, 6, 8, 5, 5,
                           3, 6, 5, 4, 4, 7, 4, 2, 1, 3,   // 0.5 items (compass: rare)
                           6, 2, 5, 4,                     // 0.6 items
                           0, 0.35f, 0,                     // guns: the shotgun is rare, the rest sandbox only
                           1.6f};                           // shells (rarer than you'd like)
    bool dark = info().dark || id_ == LevelId::L2;
    if (dark) w[(int)ItemType::Flashlight] *= 3, w[(int)ItemType::Firesalt] *= 2;
    if (id_ == LevelId::L1) w[(int)ItemType::RoyalRation] *= 2.5f, w[(int)ItemType::AlmondWater] *= 1.5f; // supply crates
    if (id_ == LevelId::L2) w[(int)ItemType::Crowbar] *= 2.5f;
    if (id_ == LevelId::LFun) w[(int)ItemType::MemoryJuice] *= 2.0f;
    float sum = 0;
    for (float x : w) sum += x;
    float t = hash01(seed_, cx, cz, S_ITEMTYPE) * sum;
    for (int i = 1; i < ITEM_COUNT; i++) {
        t -= w[i];
        if (t < 0) return (ItemType)i;
    }
    return ItemType::AlmondWater;
}

// Where a cell's item would lie: on top of a piece of furniture in the cell if there is one
// (a table, a desk, a shelf, a bed...), otherwise on the floor, against a wall if it can, never
// inside anything solid.
bool World::item_spot(int cx, int cz, Vec2& pos, float& y) const {
    const float x0 = cx * CELL, z0 = cz * CELL;
    if (id_ == LevelId::L4 && solid(cx, cz)) { // an office desk
        pos = {x0 + CELL * (0.3f + 0.4f * hash01(seed_, cx, cz, S_ITEMPOS)), z0 + CELL * (0.3f + 0.4f * hash01(seed_, cx, cz, S_ITEMPOS + 1))};
        y = block_h(cx, cz);
        return true;
    }
    Furn fs[MAX_FURN];
    int n = furniture_raw(cx, cz, fs, nullptr); // (doors don't move items around when they swing)
    // furniture whose middle is in this cell and that has something flat to put things on
    int pick[MAX_FURN], np = 0;
    float top[MAX_FURN];
    for (int k = 0; k < n; k++) {
        const Furn& f = fs[k];
        float mx = (f.x0 + f.x1) * 0.5f, mz = (f.z0 + f.z1) * 0.5f;
        if (floor_to_int(mx / CELL) != cx || floor_to_int(mz / CELL) != cz) continue;
        float t = -1;
        switch (f.kind) {
            case FurnKind::Table: case FurnKind::Table2: case FurnKind::RoundTable: case FurnKind::Counter: case FurnKind::Desk:
            case FurnKind::Nightstand: case FurnKind::Dresser: case FurnKind::Cabinet: case FurnKind::Boxes: case FurnKind::Crate:
            case FurnKind::Bench: case FurnKind::Barrel: case FurnKind::Pallet: case FurnKind::Mattress: case FurnKind::Stove: case FurnKind::Cot:
                t = f.y1; break;
            case FurnKind::Shelf: case FurnKind::Rack: case FurnKind::Bookcase: { // one of the shelves
                int s = (int)(hash3(seed_, cx, cz, S_ITEMPOS + 5) % 3u);
                t = 0.13f + s * (f.y1 - 0.15f) / 3.0f;
                break;
            }
            case FurnKind::Bed: t = 0.58f; break;
            case FurnKind::Couch: case FurnKind::Lounger: t = 0.45f; break;
            default: break;
        }
        if (t > 0) { top[np] = t; pick[np++] = k; }
    }
    float base = floor_y((cx + 0.5f) * CELL, (cz + 0.5f) * CELL);
    if (np > 0) {
        int k = (int)(hash3(seed_, cx, cz, S_ITEMPOS + 2) % (uint32_t)np);
        const Furn& f = fs[pick[k]];
        float ju = hash01(seed_, cx, cz, S_ITEMPOS) - 0.5f, jv = hash01(seed_, cx, cz, S_ITEMPOS + 1) - 0.5f;
        float hx = (f.x1 - f.x0) * 0.5f, hz = (f.z1 - f.z0) * 0.5f;
        pos = {(f.x0 + f.x1) * 0.5f + ju * std::max(0.0f, hx - 0.15f), (f.z0 + f.z1) * 0.5f + jv * std::max(0.0f, hz - 0.15f)};
        y = base + top[k];
        return true;
    }
    // on the floor: try a few spots (by a wall first) and take the first one clear of furniture
    static const int DXs[4] = {-1, 1, 0, 0}, DZs[4] = {0, 0, -1, 1};
    int wall = -1;
    for (int k = 0; k < 4 && wall < 0; k++) {
        int s = (int)((hash3(seed_, cx, cz, S_ITEMPOS + 3) + (uint32_t)k) % 4u);
        if (wall_face(cx, cz, s)) wall = s;
    }
    (void)DXs; (void)DZs;
    for (int t = 0; t < 6; t++) {
        float a = hash01(seed_, cx, cz, S_ITEMPOS + 10 + t) - 0.5f, b = hash01(seed_, cx, cz, S_ITEMPOS + 20 + t) - 0.5f;
        Vec2 p{(cx + 0.5f + a * 0.6f) * CELL, (cz + 0.5f + b * 0.6f) * CELL};
        if (wall >= 0 && t < 3) { // tucked against the wall
            const float in = WALL_T * 0.5f + 0.25f;
            if (wall == 0) p.x = x0 + in;
            if (wall == 1) p.x = x0 + CELL - in;
            if (wall == 2) p.z = z0 + in;
            if (wall == 3) p.z = z0 + CELL - in;
        }
        bool clear = true;
        for (int k = 0; k < n && clear; k++)
            if (fs[k].solid && p.x > fs[k].x0 - 0.2f && p.x < fs[k].x1 + 0.2f && p.z > fs[k].z0 - 0.2f && p.z < fs[k].z1 + 0.2f) clear = false;
        if (!clear) continue;
        pos = p;
        y = floor_y(p.x, p.z);
        return false;
    }
    pos = {(cx + 0.5f) * CELL, (cz + 0.5f) * CELL};
    y = base;
    return false;
}

Vec2 World::item_pos(int cx, int cz) const {
    Vec2 p;
    float y;
    item_spot(cx, cz, p, y);
    return p;
}

float World::item_y(int cx, int cz) const {
    Vec2 p;
    float y;
    item_spot(cx, cz, p, y);
    return y;
}


// ------------------------------------------------------------------ 1.3 notes
// Scraps of paper left by people who came through before you. Reading one is up to each player
// (nothing in the sim changes), so a note is just a pure function of the cell, like an item.
static const char* NOTES[] = {
    "DAY 3. THE LIGHTS HUM IN B FLAT. I CHECKED.|IF YOU CAN HEAR IT TOO, YOU'RE NOT CRAZY.|OR WE BOTH ARE.",
    "DON'T DRINK FROM THE PUDDLES.|I KNOW THE ALMOND WATER RAN OUT.|DON'T.",
    "I'VE PASSED THIS CHAIR FOUR TIMES.|IT'S ALWAYS FACING ME.",
    "M.E.G. NOTICE|OUTPOSTS ARE MARKED WITH A BLUE FLAG.|IF YOU SEE ONE, WALK TOWARD IT. DO NOT RUN.",
    "THE HOUNDS CAN'T SEE WELL.|THEY HEAR YOU BREATHE.|CROUCH. BREATHE SLOWLY. LOOK AT THEM.",
    "SMILE BACK AND IT GETS CLOSER.|LOOK AWAY AND IT GETS CLOSER.|JUST KEEP THE LIGHT ON IT.",
    "FOUND A DOOR THAT WOULDN'T OPEN.|CAME BACK WITH A CROWBAR.|SHOULDN'T HAVE.",
    "TO WHOEVER FINDS THIS: THE EXIT SIGNS|ARE REAL. THE ARROWS UNDER THEM ARE NOT.",
    "MY COMPASS POINTS SOMEWHERE NEW|EVERY TIME I STOP WALKING.|MAYBE THAT'S THE POINT.",
    "THE PARTY PEOPLE SAID I COULD STAY.|THEY SAID IT LIKE A THREAT.",
    "THINGS I MISS: WINDOWS. RAIN.|THE SOUND OF A CAR.|MY NAME. I'M NOT SURE OF IT ANYMORE.",
    "IF THE CARPET IS DRY, KEEP GOING.|IF IT'S WET, KEEP GOING FASTER.",
    "M.E.G. MEMO 14|STAFF ARE REMINDED THAT THE HAZMAT SUITS|ARE NOT OPTIONAL. ASK THE ONES WHO TOOK THEM OFF.",
    "THE MAN IN THE SUIT ON 1ONE KNOCKS.|HE ALWAYS KNOCKS.|DON'T ANSWER.",
    "COUNTED 211 CEILING LIGHTS TODAY.|ONE OF THEM COUNTED BACK.",
    "THE POOLS ARE WARM. THEY SHOULDN'T BE.|NOTHING HEATS THEM.",
    "I TRIED TO SLEEP UNDER A DESK.|WOKE UP UNDER A DIFFERENT DESK.",
    "THE CITY IS EMPTY BUT THE COFFEE IS HOT.|WHO KEEPS MAKING THE COFFEE.",
    "HAVE YOU NOTICED THE WALLPAPER REPEATS|EVERY 1.25 METERS? EXCEPT HERE.|LOOK. IT DOESN'T. WHY HERE.",
    "IF YOU HEAR FOOTSTEPS MATCHING YOURS,|STOP. IF THEY DON'T STOP, RUN.",
    "LEFT A CHALK MARK HERE.|THERE ARE TWO NOW. I ONLY MADE ONE.",
    "MOTION IS NICE BY DAY. THE MUSIC|COMES FROM THE SIRENS ON THE POLES.|AT NIGHT THE MUSIC STOPS. THEN THEY COME.",
    "THE MOON HAS NO AIR AND I CAN STILL|HEAR SOMETHING WALKING BEHIND ME.",
    "M.E.G. FIELD GUIDE, PAGE 2|IF A COLLEAGUE STOPS TALKING ON THE RADIO,|DO NOT GO LOOKING FOR THEM ALONE.",
    "THE HOTEL HAS A COURTYARD.|THE WINDOWS ALL FACE IN.|SOMEONE IS ALWAYS STANDING IN ONE.",
    "I FOUND THE END. IT'S A BOOKSHOP.|EVERY SHELF IS EMPTY.|SOMEONE TOOK ALL THE ENDINGS.",
    "THEY SAY THERE'S A DOOR AT THE VERY END|THAT GOES HOME. THEY SAY A LOT OF THINGS.",
    "HOW TO MAKE ALMOND WATER LAST:|YOU CAN'T. DRINK IT WHEN THE LIGHTS GO.",
    "THE FACELINGS JUST WANT SOMETHING.|GIVE THEM SOMETHING.",
    "BIG LUNGS. STEADY NERVES. LIGHT FEET.|THE OBJECTIVES MAKE YOU STRONGER.|SO DO THE THINGS YOU SEE DOING THEM.",
    "WHOEVER KEEPS CLOSING THE DOORS:|PLEASE STOP.",
    "I THOUGHT I HEARD MY FRIEND CALLING.|IT WAS MY OWN VOICE, A LITTLE LATE.",
    "IF YOU'RE READING THIS YOU STILL HAVE|YOUR EYES. GOOD. KEEP THEM ON THE EXIT.",
    "DON'T TRUST A ROOM WITHOUT A HUM.",
    "THE MANGLED IS TALLER THAN THE BUILDINGS.|IT DOESN'T LOOK DOWN.|THAT'S THE ONLY GOOD NEWS.",
    "WROTE THIS IN THE DARK. SORRY ABOUT|THE HANDWRITING. SORRY ABOUT A LOT.",
};
constexpr int NOTE_COUNT = (int)(sizeof(NOTES) / sizeof(NOTES[0]));
int note_count() { return NOTE_COUNT; }
const char* note_text(int n) { return n >= 1 && n <= NOTE_COUNT ? NOTES[n - 1] : ""; }

int World::note(int cx, int cz) const {
    if (id_ == LevelId::Hub || greyscale() || near_spawn(cx, cz, 2)) return 0;
    if (hash01(seed_, cx, cz, S_ITEM + 60) >= (id_ == LevelId::TheEnd ? 0.02f : 0.006f)) return 0;
    if (solid(cx, cz) || exit_cell(cx, cz) || pole(cx, cz) || water(cx, cz) || pool(cx, cz)) return 0;
    if (item(cx, cz) != ItemType::None) return 0;
    return 1 + (int)(hash3(seed_, cx, cz, S_ITEM + 61) % (uint32_t)NOTE_COUNT);
}

Vec2 World::note_pos(int cx, int cz) const {
    float a = hash01(seed_, cx, cz, S_ITEM + 62) - 0.5f, b = hash01(seed_, cx, cz, S_ITEM + 63) - 0.5f;
    Vec2 p{(cx + 0.5f + a * 0.5f) * CELL, (cz + 0.5f + b * 0.5f) * CELL};
    Furn fs[MAX_FURN];
    int n = furniture_raw(cx, cz, fs, nullptr);
    for (int k = 0; k < n; k++) // never inside furniture
        if (fs[k].solid && p.x > fs[k].x0 - 0.15f && p.x < fs[k].x1 + 0.15f && p.z > fs[k].z0 - 0.15f && p.z < fs[k].z1 + 0.15f)
            p = {(cx + 0.5f) * CELL + (a > 0 ? -0.9f : 0.9f), (cz + 0.5f) * CELL + (b > 0 ? -0.9f : 0.9f)};
    return p;
}

// ------------------------------------------------------------------ navigation
bool World::can_step(int cx, int cz, int dx, int dz) const {
    if (solid(cx + dx, cz + dz)) return false;
    if (dx == 1) return !wall_x(cx + 1, cz);
    if (dx == -1) return !wall_x(cx, cz);
    if (dz == 1) return !wall_z(cx, cz + 1);
    if (dz == -1) return !wall_z(cx, cz);
    return true;
}

bool World::segment_blocked(Vec2 a, Vec2 b) const {
    // Amanatides-Woo walk through the grid: thin walls on every line crossed (unless they're
    // low enough to see over) and solid blocks taller than eye level in every cell entered.
    const bool walls_block = info().wall_h >= 1.7f;
    float x = a.x / CELL, z = a.z / CELL, tx = b.x / CELL, tz = b.z / CELL;
    int cx = floor_to_int(x), cz = floor_to_int(z);
    int ex = floor_to_int(tx), ez = floor_to_int(tz);
    float dx = tx - x, dz = tz - z;
    int sx = dx > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    float tdx = dx != 0 ? std::fabs(1.0f / dx) : 1e30f, tdz = dz != 0 ? std::fabs(1.0f / dz) : 1e30f;
    float tmx = dx != 0 ? ((sx > 0 ? (cx + 1 - x) : (x - cx)) * tdx) : 1e30f;
    float tmz = dz != 0 ? ((sz > 0 ? (cz + 1 - z) : (z - cz)) * tdz) : 1e30f;
    for (int guard = 0; guard < 512 && (cx != ex || cz != ez); guard++) {
        if (tmx < tmz) {
            if (tmx > 1.0f) break;
            if (walls_block && wall_x(sx > 0 ? cx + 1 : cx, cz)) return true;
            cx += sx;
            tmx += tdx;
        } else {
            if (tmz > 1.0f) break;
            if (walls_block && wall_z(cx, sz > 0 ? cz + 1 : cz)) return true;
            cz += sz;
            tmz += tdz;
        }
        if (solid(cx, cz) && block_h(cx, cz) > 1.7f) return true;
    }
    if (id_ == LevelId::L94) { // a hill between you and it
        float ya = floor_y(a.x, a.z) + 1.5f, yb = floor_y(b.x, b.z) + 1.5f;
        float len = (b - a).len();
        int steps = std::min(24, (int)(len / 3.0f));
        for (int k = 1; k < steps; k++) {
            float t = (float)k / steps;
            Vec2 p = a + (b - a) * t;
            if (floor_y(p.x, p.z) > lerpf(ya, yb, t)) return true;
        }
    }
    return false;
}

float World::light_level(Vec3 p) const {
    const LevelInfo& li = info();
    float b = (li.ambient[0] + li.ambient[1] + li.ambient[2]) / 3.0f;
    if (li.dark) b *= 0.25f;
    if (has_outposts()) { // the outposts' floodlights
        int ocx, ocz;
        int pcx0 = floor_to_int(p.x / CELL), pcz0 = floor_to_int(p.z / CELL);
        int rx = floor_div(pcx0 + 12, 24), rz = floor_div(pcz0 + 12, 24);
        if (outpost_region(rx, rz, ocx, ocz)) {
            float d = (Vec2{(ocx + 0.5f) * CELL, (ocz + 0.5f) * CELL} - Vec2{p.x, p.z}).len();
            if (d < 9.0f) b += (1.0f - d / 9.0f) * 1.1f;
        }
    }
    if (li.light_power <= 0) return b;
    int pcx = floor_to_int(p.x / CELL), pcz = floor_to_int(p.z / CELL);
    int r = (int)(li.light_radius / CELL) + 1;
    for (int cz = pcz - r; cz <= pcz + r; cz++)
        for (int cx = pcx - r; cx <= pcx + r; cx++) {
            LightState s = light(cx, cz);
            if (s != LightState::On && s != LightState::Flicker) continue;
            Vec2 lp{(cx + 0.5f) * CELL, (cz + 0.5f) * CELL};
            float d = (lp - Vec2{p.x, p.z}).len();
            if (d >= li.light_radius) continue;
            float att = 1.0f - d / li.light_radius;
            if (segment_blocked({p.x, p.z}, lp)) att *= 0.1f;
            b += att * att * li.light_power;
        }
    return b;
}

} // namespace iris
