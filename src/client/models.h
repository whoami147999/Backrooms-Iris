// Backrooms Iris - curved low-poly models built every frame from primitives: capsules (limbs),
// ellipsoids (heads, torsos), boxes and tubes (guns, tools). Smooth per-vertex shading with a key
// light, so they read as solid shapes instead of stacked boxes. Used for entities, other players
// and the first-person view models. Segment counts drop on the Vita.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/math.h"
#include "chunk_mesh.h" // Vertex

namespace iris {

struct Rgb { float r, g, b; };

class ModelBuilder {
public:
    ModelBuilder(std::vector<Vertex>& out, int seg) : out_(out), seg_(seg) {}
    // where the model stands (world): position on the floor and facing; or camera space for view models
    void place(Vec2 pos, float yaw, float ground = 0) { pos_ = pos; yaw_ = yaw; ground_ = ground; camera_ = false; }
    void place_camera() { camera_ = true; }
    void set_light(float level, Vec3 key_dir) { light_ = level; key_ = key_dir * (1.0f / key_dir.len()); }
    // an extra rotation applied to everything that follows (local, before placement): pitch about x, then roll about z, then yaw about y
    void push_pose(Vec3 pivot, float pitch, float yaw = 0, float roll = 0);
    void pop_pose();

    void ellipsoid(Vec3 c, Vec3 r, Rgb col, float detail = 1.0f);
    void capsule(Vec3 a, Vec3 b, float ra, float rb, Rgb col, bool caps = true);
    void tube(Vec3 a, Vec3 b, float r, Rgb col, bool caps = true, float detail = 1.0f);
    void box(Vec3 c, Vec3 half, Rgb col);
    void cone(Vec3 base, Vec3 tip, float r, Rgb col);

private:
    struct Pose { Vec3 pivot; float cp, sp, cy, sy, cr, sr; };
    std::vector<Vertex>& out_;
    int seg_;
    Vec2 pos_;
    float yaw_ = 0, ground_ = 0;
    bool camera_ = false;
    float light_ = 1;
    Vec3 key_{0.3f, 0.85f, 0.4f};
    std::vector<Pose> poses_;
    Vec3 xf_point(Vec3 p) const;
    Vec3 xf_dir(Vec3 n) const;
    void tri(Vec3 a, Vec3 na, Vec3 b, Vec3 nb, Vec3 c, Vec3 nc, Rgb col);
    void quad(Vec3 a, Vec3 na, Vec3 b, Vec3 nb, Vec3 c, Vec3 nc, Vec3 d, Vec3 nd, Rgb col);
    uint8_t shade(Vec3 n_world, float base) const;
};

// A two-legged figure: everything that walks upright uses this, with its own proportions.
struct Humanoid {
    float leg = 0.85f;          // hip to ankle
    float torso = 0.55f;        // pelvis to the base of the neck
    float neck = 0.08f;
    float hip_w = 0.11f, shoulder_w = 0.2f;
    float upper_arm = 0.3f, forearm = 0.28f;
    float limb_r = 0.06f, arm_r = 0.045f;
    Vec3 chest{0.19f, 0.2f, 0.12f}, belly{0.16f, 0.17f, 0.11f};
    Vec3 head{0.11f, 0.13f, 0.12f};
    Rgb skin{0.85f, 0.68f, 0.56f}, top{0.3f, 0.3f, 0.35f}, legs{0.2f, 0.22f, 0.3f}, feet{0.12f, 0.1f, 0.09f}, hands{0.85f, 0.68f, 0.56f};
    Rgb hair{0.2f, 0.14f, 0.1f};
    bool has_hair = true;
    // pose / animation
    float phase = 0;            // walk cycle (radians)
    float stride = 0;           // 0 standing .. 1 full stride
    float lean = 0;             // forward lean of the torso (radians)
    float crouch = 0;           // 0..1
    float arm_forward = 0;      // both arms raised forward (radians)
    float arm_swing = 0.6f;
    float head_tilt = 0;
    // outputs: joint positions (local) for extras
    Vec3 head_c, chest_c, hand[2], foot[2], shoulder[2], hip_c;
    void build(ModelBuilder& m);
};

} // namespace iris
