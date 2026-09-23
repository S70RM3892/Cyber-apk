#include "apex/specular_aa.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace apex {

float vmf_widened_alpha(float alpha, Vec3 avg_normal) {
    const float r2 = std::min(dot(avg_normal, avg_normal), 1.0f);
    if (r2 >= 1.0f - 1e-7f) return alpha;  // no spread: nothing to add
    if (r2 <= 1e-12f) return 1.0f;         // normals cancel out: fully rough
    const float r = std::sqrt(r2);
    const float inv_kappa = (1.0f - r2) / (r * (3.0f - r2));
    return std::min(1.0f, std::sqrt(alpha * alpha + 2.0f * inv_kappa));
}

namespace {

struct Level {
    std::uint32_t w, h;
    std::vector<Vec3> avg_normal;  // unnormalized mean of the footprint's unit normals
    std::vector<float> mean_alpha2;  // mean GGX alpha^2 over the footprint
};

// Every texel of `next` covers the same number of level-0 texels, so the mean of
// the child means is the mean over the whole footprint.
Level downsample(const Level& src) {
    Level dst{std::max(1u, src.w / 2), std::max(1u, src.h / 2), {}, {}};
    dst.avg_normal.resize(std::size_t{dst.w} * dst.h);
    dst.mean_alpha2.resize(dst.avg_normal.size());
    const std::uint32_t sx = src.w > 1 ? 2 : 1;
    const std::uint32_t sy = src.h > 1 ? 2 : 1;
    const float inv = 1.0f / static_cast<float>(sx * sy);
    for (std::uint32_t y = 0; y < dst.h; ++y) {
        for (std::uint32_t x = 0; x < dst.w; ++x) {
            Vec3 n{};
            float a2 = 0.0f;
            for (std::uint32_t j = 0; j < sy; ++j) {
                for (std::uint32_t i = 0; i < sx; ++i) {
                    const std::size_t s = std::size_t{y * sy + j} * src.w + (x * sx + i);
                    n = n + src.avg_normal[s];
                    a2 += src.mean_alpha2[s];
                }
            }
            const std::size_t d = std::size_t{y} * dst.w + x;
            dst.avg_normal[d] = n * inv;
            dst.mean_alpha2[d] = a2 * inv;
        }
    }
    return dst;
}

RoughnessMip resolve(const Level& lvl) {
    RoughnessMip mip{lvl.w, lvl.h, std::vector<float>(lvl.avg_normal.size())};
    for (std::size_t i = 0; i < mip.roughness.size(); ++i) {
        const float alpha = vmf_widened_alpha(std::sqrt(lvl.mean_alpha2[i]), lvl.avg_normal[i]);
        mip.roughness[i] = std::sqrt(alpha);
    }
    return mip;
}

}  // namespace

std::vector<RoughnessMip> bake_roughness_mips(std::span<const Vec3> normals,
                                              std::span<const float> roughness,
                                              std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0 || !std::has_single_bit(width) || !std::has_single_bit(height))
        throw std::invalid_argument("bake_roughness_mips: dimensions must be powers of two");
    const std::size_t count = std::size_t{width} * height;
    if (normals.size() != count || roughness.size() != count)
        throw std::invalid_argument("bake_roughness_mips: input size mismatch");

    Level lvl{width, height, {normals.begin(), normals.end()}, std::vector<float>(count)};
    for (std::size_t i = 0; i < count; ++i) {
        const float alpha = roughness[i] * roughness[i];
        lvl.mean_alpha2[i] = alpha * alpha;
    }

    std::vector<RoughnessMip> mips;
    mips.push_back(resolve(lvl));
    while (lvl.w > 1 || lvl.h > 1) {
        lvl = downsample(lvl);
        mips.push_back(resolve(lvl));
    }
    return mips;
}

}  // namespace apex
