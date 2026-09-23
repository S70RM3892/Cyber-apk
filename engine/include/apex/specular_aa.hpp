// Normal-variance -> roughness bake for mip chains (spec §3.4).
//
// The spec asks for "Toksvig / LEAN". Toksvig's original formula targets a
// Blinn-Phong exponent; for a GGX pipeline we use the von Mises-Fisher fit
// described by Karis ("Normal map filtering using vMF", 2018):
//
//   r      = |average of unit normals in the footprint|
//   1/k    = (1 - r^2) / (r * (3 - r^2))
//   alpha' = sqrt(alpha^2 + 2/k)            (alpha = GGX alpha = roughness^2)
//
// This needs the *unnormalized* average normal, which a 2-channel runtime
// format cannot carry, so the variance is baked into the roughness mips offline.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "apex/math.hpp"

namespace apex {

// GGX alpha widened by the normal spread encoded in avg_normal's length.
float vmf_widened_alpha(float alpha, Vec3 avg_normal);

struct RoughnessMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> roughness;  // perceptual roughness (sqrt of GGX alpha), row-major
};

// Build the full roughness mip chain (level 0 .. 1x1).
//
// normals:   level-0 unit normals, row-major, width*height entries.
// roughness: level-0 perceptual roughness, same layout.
// width/height must be powers of two (all spec §3.1 tiers are).
std::vector<RoughnessMip> bake_roughness_mips(std::span<const Vec3> normals,
                                              std::span<const float> roughness,
                                              std::uint32_t width, std::uint32_t height);

}  // namespace apex
