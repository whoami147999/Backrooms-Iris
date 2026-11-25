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
    std::vector<Vec3> exits;          // exit door positions (centre of the door, floor level)
    std::vector<uint32_t> items;      // item ids lying in this chunk
};

// Builds chunk (kx, kz): cells [kx*CHUNK_CELLS, (kx+1)*CHUNK_CELLS) on each axis.
void build_chunk_mesh(const World& world, int kx, int kz, ChunkMesh& out);

// Cave ceilings are lumpy: height of the ceiling at a world position (level 8).
float ceiling_y(const World& world, float x, float z);

} // namespace iris
