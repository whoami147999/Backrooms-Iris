#include "models.h"

#include <algorithm>
#include <cmath>

namespace iris {

void ModelBuilder::push_pose(Vec3 pivot, float pitch, float yaw, float roll) {
    poses_.push_back({pivot, std::cos(pitch), std::sin(pitch), std::cos(yaw), std::sin(yaw), std::cos(roll), std::sin(roll)});
}
void ModelBuilder::pop_pose() { if (!poses_.empty()) poses_.pop_back(); }

Vec3 ModelBuilder::xf_dir(Vec3 n) const {
    for (int i = (int)poses_.size() - 1; i >= 0; i--) {
        const Pose& p = poses_[(size_t)i];
        // pitch about x (positive tips +z up... forward toward -y), roll about z, yaw about y
        float y = n.y * p.cp - n.z * p.sp, z = n.y * p.sp + n.z * p.cp;
        n.y = y; n.z = z;
        float x = n.x * p.cr - n.y * p.sr; y = n.x * p.sr + n.y * p.cr;
        n.x = x; n.y = y;
        x = n.x * p.cy + n.z * p.sy; z = -n.x * p.sy + n.z * p.cy;
        n.x = x; n.z = z;
    }
    if (camera_) return n;
    // local x = right, local z = forward; world: right = (cos, -sin), forward = (-sin, -cos)
    float cy = std::cos(yaw_), sy = std::sin(yaw_);
    return {n.x * cy - n.z * sy, n.y, -n.x * sy - n.z * cy};
}

Vec3 ModelBuilder::xf_point(Vec3 p) const {
    for (int i = (int)poses_.size() - 1; i >= 0; i--) {
        const Pose& q = poses_[(size_t)i];
        Vec3 d = p - q.pivot;
        float y = d.y * q.cp - d.z * q.sp, z = d.y * q.sp + d.z * q.cp;
        d.y = y; d.z = z;
        float x = d.x * q.cr - d.y * q.sr; y = d.x * q.sr + d.y * q.cr;
        d.x = x; d.y = y;
        x = d.x * q.cy + d.z * q.sy; z = -d.x * q.sy + d.z * q.cy;
        d.x = x; d.z = z;
        p = q.pivot + d;
    }
    if (camera_) return p;
    float cy = std::cos(yaw_), sy = std::sin(yaw_);
    return {pos_.x + p.x * cy - p.z * sy, p.y + ground_, pos_.z - p.x * sy - p.z * cy};
}

uint8_t ModelBuilder::shade(Vec3 n, float base) const {
    float d = std::max(0.0f, n.dot(key_));
    float sky = 0.5f + 0.5f * n.y;               // a bit of light from above everywhere
    float k = (0.30f + 0.55f * d + 0.20f * sky) * light_;
    return (uint8_t)std::min(255.0f, std::max(0.0f, base * k * 255.0f));
}

void ModelBuilder::tri(Vec3 a, Vec3 na, Vec3 b, Vec3 nb, Vec3 c, Vec3 nc, Rgb col) {
    Vec3 P[3] = {xf_point(a), xf_point(b), xf_point(c)};
    Vec3 N[3] = {xf_dir(na), xf_dir(nb), xf_dir(nc)};
    for (int i = 0; i < 3; i++) {
        float u = (P[i].x + P[i].z) * 2.0f, v = P[i].y * 2.0f; // grit texture, world-aligned
        out_.push_back({P[i].x, P[i].y, P[i].z, u, v, shade(N[i], col.r), shade(N[i], col.g), shade(N[i], col.b), 255});
    }
}

void ModelBuilder::quad(Vec3 a, Vec3 na, Vec3 b, Vec3 nb, Vec3 c, Vec3 nc, Vec3 d, Vec3 nd, Rgb col) {
    tri(a, na, b, nb, c, nc, col);
    tri(a, na, c, nc, d, nd, col);
}

void ModelBuilder::ellipsoid(Vec3 c, Vec3 r, Rgb col, float detail) {
    int seg = std::max(5, (int)(seg_ * detail)), rings = std::max(3, seg / 2);
    for (int j = 0; j < rings; j++) {
        float p0 = -PI / 2 + PI * j / rings, p1 = -PI / 2 + PI * (j + 1) / rings;
        for (int k = 0; k < seg; k++) {
            float a0 = 2 * PI * k / seg, a1 = 2 * PI * (k + 1) / seg;
            auto U = [&](float ph, float a) { return Vec3{std::cos(ph) * std::cos(a), std::sin(ph), std::cos(ph) * std::sin(a)}; };
            auto at = [&](Vec3 u) { return Vec3{c.x + u.x * r.x, c.y + u.y * r.y, c.z + u.z * r.z}; };
            auto nrm = [&](Vec3 u) { Vec3 n{u.x / r.x, u.y / r.y, u.z / r.z}; return n * (1.0f / std::max(1e-5f, n.len())); };
            Vec3 u00 = U(p0, a0), u01 = U(p0, a1), u10 = U(p1, a0), u11 = U(p1, a1);
            quad(at(u01), nrm(u01), at(u00), nrm(u00), at(u10), nrm(u10), at(u11), nrm(u11), col);
        }
    }
}

void ModelBuilder::tube(Vec3 a, Vec3 b, float r, Rgb col, bool caps, float detail) {
    capsule(a, b, r, r, col, false);
    if (caps) { // flat ends
        Vec3 d = b - a;
        float len = d.len();
        if (len < 1e-5f) return;
        d = d * (1.0f / len);
        Vec3 up = std::fabs(d.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        Vec3 e1{d.y * up.z - d.z * up.y, d.z * up.x - d.x * up.z, d.x * up.y - d.y * up.x};
        e1 = e1 * (1.0f / e1.len());
        Vec3 e2{d.y * e1.z - d.z * e1.y, d.z * e1.x - d.x * e1.z, d.x * e1.y - d.y * e1.x};
        int seg = std::max(5, (int)(seg_ * detail));
        for (int k = 0; k < seg; k++) {
            float a0 = 2 * PI * k / seg, a1 = 2 * PI * (k + 1) / seg;
            Vec3 o0 = e1 * std::cos(a0) + e2 * std::sin(a0), o1 = e1 * std::cos(a1) + e2 * std::sin(a1);
            tri(b, d, b + o0 * r, d, b + o1 * r, d, col);
            tri(a, d * -1, a + o1 * r, d * -1, a + o0 * r, d * -1, col);
        }
    }
}

void ModelBuilder::capsule(Vec3 a, Vec3 b, float ra, float rb, Rgb col, bool caps) {
    Vec3 ax = b - a;
    float len = ax.len();
    if (len < 1e-5f) { if (caps) ellipsoid(a, {ra, ra, ra}, col, 0.6f); return; }
    Vec3 d = ax * (1.0f / len);
    Vec3 up = std::fabs(d.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 e1{d.y * up.z - d.z * up.y, d.z * up.x - d.x * up.z, d.x * up.y - d.y * up.x};
    e1 = e1 * (1.0f / e1.len());
    Vec3 e2{d.y * e1.z - d.z * e1.y, d.z * e1.x - d.x * e1.z, d.x * e1.y - d.y * e1.x};
    float slope = (ra - rb) / len;
    for (int k = 0; k < seg_; k++) {
        float a0 = 2 * PI * k / seg_, a1 = 2 * PI * (k + 1) / seg_;
        Vec3 o0 = e1 * std::cos(a0) + e2 * std::sin(a0), o1 = e1 * std::cos(a1) + e2 * std::sin(a1);
        Vec3 n0 = o0 + d * slope, n1 = o1 + d * slope;
        n0 = n0 * (1.0f / n0.len()); n1 = n1 * (1.0f / n1.len());
        quad(a + o0 * ra, n0, a + o1 * ra, n1, b + o1 * rb, n1, b + o0 * rb, n0, col);
    }
    if (caps) { // round joints: a small sphere at each end
        ellipsoid(a, {ra, ra, ra}, col, 0.6f);
        ellipsoid(b, {rb, rb, rb}, col, 0.6f);
    }
}

void ModelBuilder::cone(Vec3 base, Vec3 tip, float r, Rgb col) { capsule(base, tip, r, 0.003f, col, false); }

void ModelBuilder::box(Vec3 c, Vec3 h, Rgb col) {
    const Vec3 N[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : N) {
        // two axes spanning the face
        Vec3 u = std::fabs(n.y) > 0.5f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        Vec3 v{n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x};
        Vec3 fc{c.x + n.x * h.x, c.y + n.y * h.y, c.z + n.z * h.z};
        Vec3 us{u.x * h.x, u.y * h.y, u.z * h.z}, vs{v.x * h.x, v.y * h.y, v.z * h.z};
        quad(fc - us - vs, n, fc + us - vs, n, fc + us + vs, n, fc - us + vs, n, col);
    }
}

// ------------------------------------------------------------------ humanoid
static Vec3 swing_down(float len, float angle) { return {0, -len * std::cos(angle), len * std::sin(angle)}; } // a limb hanging, swung forward by angle

void Humanoid::build(ModelBuilder& m) {
    const float thigh = leg * 0.5f, shin = leg * 0.5f;
    float bend_all = crouch * 0.9f;
    float pelvis_y = leg * (1.0f - crouch * 0.32f) + 0.05f;
    hip_c = {0, pelvis_y, 0};
    // legs
    for (int s = 0; s < 2; s++) {
        float side = s == 0 ? -1.0f : 1.0f;
        float ph = phase + (s == 0 ? 0.0f : PI);
        float th = std::sin(ph) * 0.55f * stride + bend_all * 0.6f;
        float knee_bend = std::max(0.0f, -std::cos(ph)) * 0.9f * stride + bend_all + 0.05f;
        Vec3 hip{side * hip_w, pelvis_y, 0};
        Vec3 knee = hip + swing_down(thigh, th);
        Vec3 ankle = knee + swing_down(shin, th - knee_bend);
        m.capsule(hip, knee, limb_r * 1.15f, limb_r * 0.9f, legs);
        m.capsule(knee, ankle, limb_r * 0.85f, limb_r * 0.6f, legs);
        Vec3 toe = ankle + Vec3{0, -0.03f, 0.13f};
        m.capsule(ankle + Vec3{0, -0.02f, -0.02f}, toe, limb_r * 0.75f, limb_r * 0.6f, feet);
        foot[s] = toe;
    }
    // torso: pelvis, belly, chest, leaning forward
    Vec3 up{0, std::cos(lean), std::sin(lean)};
    Vec3 base{0, pelvis_y, 0};
    m.ellipsoid(base + up * 0.04f, {hip_w + limb_r * 1.1f, 0.1f, belly.z * 0.95f}, legs, 0.8f);
    Vec3 mid = base + up * (torso * 0.38f);
    m.push_pose(mid, lean);
    m.ellipsoid(mid, belly, top);
    m.pop_pose();
    chest_c = base + up * (torso * 0.72f);
    m.push_pose(chest_c, lean);
    m.ellipsoid(chest_c, chest, top);
    m.pop_pose();
    Vec3 neck_base = base + up * torso;
    // arms
    for (int s = 0; s < 2; s++) {
        float side = s == 0 ? -1.0f : 1.0f;
        float ph = phase + (s == 0 ? PI : 0.0f);
        Vec3 sh = neck_base + Vec3{side * shoulder_w, -0.05f, 0};
        shoulder[s] = sh;
        float a = std::sin(ph) * arm_swing * stride + arm_forward + lean * 0.5f;
        if (arm_a[s] < 1e8f) a = arm_a[s];
        Vec3 elbow = sh + swing_down(upper_arm, a) + Vec3{side * 0.02f, 0, 0};
        float eb = 0.25f + std::max(0.0f, std::sin(ph)) * 0.6f * stride + arm_forward * 0.2f;
        if (arm_eb[s] < 1e8f) eb = arm_eb[s];
        Vec3 wrist = elbow + swing_down(forearm, a + eb);
        m.ellipsoid(sh, {arm_r * 1.6f, arm_r * 1.5f, arm_r * 1.6f}, top, 0.6f);
        m.capsule(sh, elbow, arm_r * 1.2f, arm_r, top);
        m.capsule(elbow, wrist, arm_r, arm_r * 0.8f, top);
        m.ellipsoid(wrist + swing_down(0.05f, a + eb), {arm_r * 1.05f, arm_r * 1.4f, arm_r * 0.8f}, hands, 0.6f);
        hand[s] = wrist + swing_down(0.06f, a + eb);
    }
    // neck and head
    Vec3 neck_top = neck_base + up * neck;
    m.capsule(neck_base, neck_top, arm_r * 1.2f, arm_r * 1.1f, skin, false);
    head_c = neck_top + Vec3{0, head.y * 0.85f, head.z * 0.15f};
    m.push_pose(head_c, head_tilt);
    m.ellipsoid(head_c, head, skin);
    if (has_hair) m.ellipsoid(head_c + Vec3{0, head.y * 0.35f, -head.z * 0.15f}, {head.x * 1.06f, head.y * 0.75f, head.z * 1.0f}, hair);
    m.pop_pose();
}

} // namespace iris
