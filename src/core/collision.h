// Backrooms Iris - player vs world collision (2D: the level has flat floors/ceilings).
#pragma once
#include "level0.h"
#include "math.h"

namespace iris {

// Pushes a circle (the player) out of every wall, post and pillar it overlaps.
void collide_circle(const Level0& level, Vec2& pos, float radius);

} // namespace iris
