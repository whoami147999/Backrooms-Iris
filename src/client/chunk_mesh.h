// Backrooms Iris - turns a chunk of Level 0 into triangles (CPU side, no GL here).
#pragma once
#include <cstdint>
#include <vector>

#include "../core/level0.h"

namespace iris {

enum Material : int { MAT_WALL, MAT_FLOOR, MAT_CEIL, MAT_LIGHT, MAT_COUNT };

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
    // index range of each material inside `idx`
    uint32_t first[MAT_COUNT] = {};
    uint32_t count[MAT_COUNT] = {};
};

// Builds chunk (kx, kz): cells [kx*CHUNK_CELLS, (kx+1)*CHUNK_CELLS) on each axis.
void build_chunk_mesh(const Level0& level, int kx, int kz, ChunkMesh& out);

} // namespace iris
