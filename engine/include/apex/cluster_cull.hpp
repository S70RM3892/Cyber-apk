// CPU reference for the GPU-driven cluster culling pass (spec §4.1).
//
// shaders/cluster_cull.comp runs the same frustum + backface-cone tests per
// cluster (plus HZB occlusion, which needs the depth pyramid and is GPU-only);
// this file exists so the math can be unit-tested on the host.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "apex/math.hpp"

namespace apex {

// Per-cluster bounds, laid out like meshoptimizer's meshopt_Bounds subset.
struct ClusterBounds {
    Vec3 center;
    float radius = 0.0f;
    Vec3 cone_apex;
    Vec3 cone_axis;
    float cone_cutoff = 1.0f;  // cos(half-angle) of the normal cone; >= 1 disables cone culling
};

struct Frustum {
    std::array<Plane, 6> planes;  // inward-facing
};

// Build a symmetric perspective frustum in view space (camera at origin looking down -Z).
Frustum make_view_frustum(float fov_y_radians, float aspect, float z_near, float z_far);

bool sphere_in_frustum(const Frustum& f, Vec3 center, float radius);

// meshoptimizer's cone test: reject when every triangle is guaranteed back-facing.
bool cone_backfacing(const ClusterBounds& c, Vec3 camera_pos);

// Returns indices of clusters that survive frustum + cone culling.
// Bounds are expected in the same space as the frustum and camera_pos.
std::vector<std::uint32_t> cull_clusters(std::span<const ClusterBounds> clusters,
                                         const Frustum& frustum, Vec3 camera_pos);

}  // namespace apex
