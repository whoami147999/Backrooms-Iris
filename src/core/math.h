// Backrooms Iris - small vector/matrix math. Header-only, no dependencies.
#pragma once
#include <cmath>

namespace iris {

constexpr float PI = 3.14159265358979f;

struct Vec2 {
    float x = 0, z = 0;
    Vec2() = default;
    Vec2(float x_, float z_) : x(x_), z(z_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, z + o.z}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, z - o.z}; }
    Vec2 operator*(float s) const { return {x * s, z * s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; z += o.z; return *this; }
    float len() const { return std::sqrt(x * x + z * z); }
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    float len() const { return std::sqrt(x * x + y * y + z * z); }
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Move `v` toward `target` by at most `step`.
inline float approach(float v, float target, float step) {
    if (v < target) return v + step > target ? target : v + step;
    return v - step < target ? target : v - step;
}

// floor division that rounds toward -infinity (needed for negative world coordinates)
inline int floor_div(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
inline int floor_to_int(float v) { return (int)std::floor(v); }

// Column-major 4x4 matrix (OpenGL layout).
struct Mat4 {
    float m[16];

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
        return r;
    }
    static Mat4 perspective(float fovy_rad, float aspect, float znear, float zfar) {
        Mat4 r{};
        float f = 1.0f / std::tan(fovy_rad * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zfar + znear) / (znear - zfar);
        r.m[11] = -1;
        r.m[14] = 2 * zfar * znear / (znear - zfar);
        return r;
    }
    // First-person view: yaw around +Y (0 = looking down -Z), pitch around X.
    static Mat4 fps_view(Vec3 eye, float yaw, float pitch) {
        float cy = std::cos(yaw), sy = std::sin(yaw);
        float cp = std::cos(pitch), sp = std::sin(pitch);
        // camera basis
        Vec3 right{cy, 0, -sy};
        Vec3 up{sy * sp, cp, cy * sp};
        Vec3 back{sy * cp, -sp, cy * cp};
        Mat4 r = identity();
        r.m[0] = right.x; r.m[4] = right.y; r.m[8] = right.z;
        r.m[1] = up.x;    r.m[5] = up.y;    r.m[9] = up.z;
        r.m[2] = back.x;  r.m[6] = back.y;  r.m[10] = back.z;
        r.m[12] = -right.dot(eye);
        r.m[13] = -up.dot(eye);
        r.m[14] = -back.dot(eye);
        return r;
    }
};

// Forward direction on the ground plane for a given yaw.
inline Vec2 yaw_forward(float yaw) { return {-std::sin(yaw), -std::cos(yaw)}; }
inline Vec2 yaw_right(float yaw) { return {std::cos(yaw), -std::sin(yaw)}; }

} // namespace iris
