// Backrooms Iris - circle vs world collision (2D: floors are flat).
#pragma once
#include "math.h"
#include "world.h"

namespace iris {

// Pushes a circle out of every wall, post, pillar and solid block it overlaps.
void collide_circle(const World& world, Vec2& pos, float radius);

} // namespace iris
