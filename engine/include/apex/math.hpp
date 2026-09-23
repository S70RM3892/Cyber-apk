// Minimal vector math shared by the host-side tools and CPU reference paths.
#pragma once

#include <cmath>

namespace apex {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    const float len = length(a);
    return len > 0.0f ? a * (1.0f / len) : Vec3{};
}

// Plane in the form dot(n, p) + d = 0, with n pointing to the inside half-space.
struct Plane {
    Vec3 n;
    float d = 0.0f;
};

}  // namespace apex
