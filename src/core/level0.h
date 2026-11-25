// Backrooms Iris - Level 0 ("the lobby"): an endless grid of yellow rooms.
//
// The level is not stored anywhere. Every query is a pure function of the seed and grid
// coordinates, so chunks generate in any order, there are no seams, and the server can
// answer "is there a wall here?" without ever building a mesh.
//
// Grid: cell (cx, cz) covers x in [cx*CELL, (cx+1)*CELL], z in [cz*CELL, (cz+1)*CELL].
// Walls live on cell edges, posts/pillars on grid points.
#pragma once
#include <cstdint>

#include "config.h"
#include "math.h"

namespace iris {

enum class LightState : uint8_t { None, On, Dead, Flicker };

enum class District : uint8_t { Rooms, Hall, Maze, Corridors, COUNT };

class Level0 {
public:
    explicit Level0(uint32_t seed = 1) : seed_(seed) {}
    uint32_t seed() const { return seed_; }

    // Wall on the vertical line x = gx*CELL, spanning z in [cz, cz+1] cells.
    bool wall_x(int gx, int cz) const;
    // Wall on the horizontal line z = gz*CELL, spanning x in [cx, cx+1] cells.
    bool wall_z(int cx, int gz) const;
    // A corner post at grid point (gx, gz): present whenever any wall touches the point.
    bool post(int gx, int gz) const;
    // A free-standing pillar at grid point (gx, gz).
    bool pillar(int gx, int gz) const;
    // Ceiling light panel in cell (cx, cz).
    LightState light(int cx, int cz) const;
    District district(int cx, int cz) const;

    // Spawn position (feet) - always in an open, lit room.
    Vec3 spawn_point() const { return {CELL * 0.5f, 0.0f, CELL * 0.5f}; }

    // True if a wall crosses the 2D segment a->b (used for light occlusion / line of sight).
    bool segment_blocked(Vec2 a, Vec2 b) const;

private:
    uint32_t seed_;
    bool in_spawn_zone(int cx, int cz) const { return cx >= -1 && cx <= 1 && cz >= -1 && cz <= 1; }
    bool edge_wall(int line, int along, bool vertical, District d) const;
};

} // namespace iris
