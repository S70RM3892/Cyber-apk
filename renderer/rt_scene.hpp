// Ray-query acceleration structures for the streamed city (VK_KHR_ray_query).
//
// Two bottom-level structures under one top-level instance each:
//   mask 0x01  opaque world: building meshes, instanced box parts, rooftop props and
//              expressway decks, as triangles (shadow / occlusion / reflection rays)
//   mask 0x02  signs: one quad per sign, custom index 1, so reflection rays can find
//              the neon that lights a puddle from off screen (primitive / 2 = sign)
// Rebuilt from scratch whenever the snapshot changes (tile crossings, a GPU idle
// already happens there).
#pragma once

#include <cstdint>
#include <vector>

#include "apex/world.hpp"
#include "vk_resources.hpp"

namespace apex {

inline constexpr std::uint32_t kRtMaskWorld = 0x01;
inline constexpr std::uint32_t kRtMaskSigns = 0x02;

class RtScene {
public:
    explicit RtScene(vk::Context& ctx) : ctx_(ctx) {}
    ~RtScene();
    RtScene(const RtScene&) = delete;
    RtScene& operator=(const RtScene&) = delete;

    // Build for this snapshot (blocking). Opaque geometry beyond `radius` metres of the
    // streaming centre is left out: shadows and reflections only matter up close.
    void build(const CitySnapshot& snap, float radius = 700.0f);
    VkAccelerationStructureKHR tlas() const { return tlas_; }
    std::uint64_t triangle_count() const { return triangles_; }

private:
    struct Accel {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        vk::Buffer storage;
    };
    vk::Context& ctx_;
    Accel world_, signs_, top_;
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
    vk::Buffer world_pos_, world_idx_, sign_pos_, instances_, scratch_;
    std::uint64_t triangles_ = 0;

    void destroy_accel(Accel& a);
    void build_blas(Accel& out, vk::Buffer& pos, vk::Buffer& idx, const std::vector<float>& p,
                    const std::vector<std::uint32_t>* indices, std::uint32_t tri_count);
    VkDeviceAddress address(const vk::Buffer& b) const;
};

}  // namespace apex
