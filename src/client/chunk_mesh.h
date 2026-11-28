// Backrooms Iris - turns a chunk of a level into triangles (CPU side, no GL here).
#pragma once
#include <cstdint>
#include <vector>

#include "../core/world.h"
#include "textures.h"

namespace iris {

#pragma pack(push, 1)
struct Vertex {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
};
#pragma pack(pop)
static_assert(sizeof(Vertex) == 24, "Vertex must stay 24 bytes");

struct ChunkMesh {
    std::vector<Vertex> verts;
    std::vector<uint16_t> idx;
    uint32_t first[MAT_COUNT] = {};   // index range of each material inside `idx`
    uint32_t count[MAT_COUNT] = {};
    std::vector<ExitDoor> exits;      // exit doors in this chunk (for the signs)
    std::vector<uint32_t> items;      // item ids lying in this chunk
    std::vector<Vec3> balloons;       // Level Fun decorations
    struct LightMark { Vec3 pos; float sx, sz; LightState state; };
    std::vector<LightMark> lights;    // light fixtures (halos, light shafts, flicker)
    struct Water { float x0, z0, x1, z1, y; };
    std::vector<Water> water;         // 1.2: pool surfaces (drawn see-through by the renderer)
    struct Shadow { float x, z, hx, hz, y; };
    std::vector<Shadow> shadows;      // 1.2: soft shadows under furniture (enhanced graphics)
    std::vector<Furn> doors;          // 1.3: hinged doors (drawn live, at whatever angle they're at)
    std::vector<Furn> containers;     // 1.3: drawers / cabinets / lockers (drawn open once searched)
    std::vector<uint32_t> notes;      // 1.3: notes lying in this chunk (cell ids)
};

// Builds chunk (kx, kz): cells [kx*CHUNK_CELLS, (kx+1)*CHUNK_CELLS) on each axis.
void build_chunk_mesh(const World& world, int kx, int kz, ChunkMesh& out, int detail = 1);

// Cave ceilings are lumpy: height of the ceiling at a world position (level 8).
float ceiling_y(const World& world, float x, float z);

} // namespace iris
