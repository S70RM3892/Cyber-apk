// Two-channel normal map encoding (spec §3.3).
//
// Only X/Y are stored; Z is reconstructed in the shader. ASTC has no RG format, so
// the asset build uses `astcenc -normal` (rrrg: X in RGB, Y in A) and the shader
// samples `.ga`.
// Mirrors shaders/include/normal_codec.glsl.
#pragma once

#include <algorithm>
#include <cmath>

#include "apex/math.hpp"

namespace apex {

// Encode a unit tangent-space normal (z >= 0) to two UNORM channels.
constexpr void encode_normal_xy(Vec3 n, float& u, float& v) {
    u = n.x * 0.5f + 0.5f;
    v = n.y * 0.5f + 0.5f;
}

// Decode two UNORM channels and rebuild Z = sqrt(1 - x^2 - y^2).
// The max() guards against block-compression error pushing x^2 + y^2 above 1.
inline Vec3 decode_normal_xy(float u, float v) {
    const float x = u * 2.0f - 1.0f;
    const float y = v * 2.0f - 1.0f;
    const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
    return normalize(Vec3{x, y, z});
}

}  // namespace apex
