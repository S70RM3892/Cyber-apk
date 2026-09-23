// Column-major 4x4 matrix, memory layout compatible with GLSL mat4.
#pragma once

#include <cmath>

#include "apex/math.hpp"

namespace apex {

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    float& at(int col, int row) { return m[col * 4 + row]; }
    float at(int col, int row) const { return m[col * 4 + row]; }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.at(k, rr) * b.at(c, k);
            r.at(c, rr) = s;
        }
    return r;
}

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// World -> view. View space follows Vulkan's clip orientation: +X right, +Y down,
// +Z forward, so the projection needs no Y flip.
inline Mat4 look_to(Vec3 eye, Vec3 forward, Vec3 world_up) {
    const Vec3 f = normalize(forward);
    const Vec3 r = normalize(cross(f, world_up));  // right
    const Vec3 d = cross(f, r);                    // down
    Mat4 v;
    v.at(0, 0) = r.x; v.at(1, 0) = r.y; v.at(2, 0) = r.z; v.at(3, 0) = -dot(r, eye);
    v.at(0, 1) = d.x; v.at(1, 1) = d.y; v.at(2, 1) = d.z; v.at(3, 1) = -dot(d, eye);
    v.at(0, 2) = f.x; v.at(1, 2) = f.y; v.at(2, 2) = f.z; v.at(3, 2) = -dot(f, eye);
    v.at(0, 3) = 0;   v.at(1, 3) = 0;   v.at(2, 3) = 0;   v.at(3, 3) = 1;
    return v;
}

// Reversed-Z, infinite far plane: depth = znear / z_view (1 at the near plane, -> 0 at infinity).
// Same convention as shaders/cluster_cull.comp.
inline Mat4 perspective_reversed_infinite(float fov_y, float aspect, float znear) {
    const float f = 1.0f / std::tan(fov_y * 0.5f);
    Mat4 p;
    for (float& x : p.m) x = 0.0f;
    p.at(0, 0) = f / aspect;
    p.at(1, 1) = f;
    p.at(3, 2) = znear;  // clip.z = znear
    p.at(2, 3) = 1.0f;   // clip.w = z_view
    return p;
}

}  // namespace apex
