#include "apex/cluster_cull.hpp"

#include <cmath>

namespace apex {

Frustum make_view_frustum(float fov_y_radians, float aspect, float z_near, float z_far) {
    const float ty = std::tan(fov_y_radians * 0.5f);
    const float tx = ty * aspect;
    Frustum f;
    // Side planes pass through the origin; normals point inward.
    f.planes[0] = {normalize(Vec3{1.0f, 0.0f, -tx}), 0.0f};   // left
    f.planes[1] = {normalize(Vec3{-1.0f, 0.0f, -tx}), 0.0f};  // right
    f.planes[2] = {normalize(Vec3{0.0f, 1.0f, -ty}), 0.0f};   // bottom
    f.planes[3] = {normalize(Vec3{0.0f, -1.0f, -ty}), 0.0f};  // top
    f.planes[4] = {Vec3{0.0f, 0.0f, -1.0f}, -z_near};         // near: -z >= z_near
    f.planes[5] = {Vec3{0.0f, 0.0f, 1.0f}, z_far};            // far:  -z <= z_far
    return f;
}

bool sphere_in_frustum(const Frustum& f, Vec3 center, float radius) {
    for (const Plane& p : f.planes)
        if (dot(p.n, center) + p.d < -radius) return false;
    return true;
}

bool cone_backfacing(const ClusterBounds& c, Vec3 camera_pos) {
    if (c.cone_cutoff >= 1.0f) return false;
    return dot(normalize(c.cone_apex - camera_pos), c.cone_axis) >= c.cone_cutoff;
}

std::vector<std::uint32_t> cull_clusters(std::span<const ClusterBounds> clusters,
                                         const Frustum& frustum, Vec3 camera_pos) {
    std::vector<std::uint32_t> visible;
    visible.reserve(clusters.size());
    for (std::uint32_t i = 0; i < clusters.size(); ++i) {
        const ClusterBounds& c = clusters[i];
        if (!sphere_in_frustum(frustum, c.center, c.radius)) continue;
        if (cone_backfacing(c, camera_pos)) continue;
        visible.push_back(i);
    }
    return visible;
}

}  // namespace apex
