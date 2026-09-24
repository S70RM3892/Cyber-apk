#include "rt_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "apex/log.hpp"

namespace apex {

namespace {

constexpr VkBufferUsageFlags kInputUsage =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

// Oriented box as 12 triangles (BoxInstance / PropInstance frame: x along yaw, y left).
void add_box(std::vector<float>& p, std::vector<std::uint32_t>& idx, float cx, float cy, float z0, float yaw,
             float hx, float hy, float height) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    const auto base = static_cast<std::uint32_t>(p.size() / 3);
    for (int k = 0; k < 8; ++k) {
        const float lx = (k & 1) ? hx : -hx, ly = (k & 2) ? hy : -hy, z = (k & 4) ? z0 + height : z0;
        p.insert(p.end(), {cx + c * lx - s * ly, cy + s * lx + c * ly, z});
    }
    // Winding does not matter: every ray query treats geometry as double-sided.
    static constexpr std::uint32_t kFaces[12][3] = {{0, 1, 3}, {0, 3, 2}, {4, 6, 7}, {4, 7, 5}, {0, 4, 5}, {0, 5, 1},
                                                    {2, 3, 7}, {2, 7, 6}, {0, 2, 6}, {0, 6, 4}, {1, 5, 7}, {1, 7, 3}};
    for (const auto& f : kFaces) idx.insert(idx.end(), {base + f[0], base + f[1], base + f[2]});
}

}  // namespace

RtScene::~RtScene() {
    destroy_accel(top_);
    destroy_accel(world_);
    destroy_accel(signs_);
    for (vk::Buffer* b : {&world_pos_, &world_idx_, &sign_pos_, &instances_, &scratch_}) vk::destroy(ctx_, *b);
}

void RtScene::destroy_accel(Accel& a) {
    if (a.as) ctx_.fns().destroy_as(ctx_.device(), a.as, nullptr);
    vk::destroy(ctx_, a.storage);
    a = {};
}

VkDeviceAddress RtScene::address(const vk::Buffer& b) const {
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer = b.buffer;
    return ctx_.fns().buffer_address(ctx_.device(), &i);
}

void RtScene::build_blas(Accel& out, vk::Buffer& pos, vk::Buffer& idx, const std::vector<float>& p,
                         const std::vector<std::uint32_t>* indices, std::uint32_t tri_count) {
    vk::destroy(ctx_, pos);
    vk::destroy(ctx_, idx);
    pos = vk::create_buffer(ctx_, std::max<VkDeviceSize>(p.size() * sizeof(float), 64), kInputUsage, true);
    std::memcpy(pos.mapped, p.data(), p.size() * sizeof(float));
    idx = vk::create_buffer(ctx_, std::max<VkDeviceSize>(indices->size() * 4, 64), kInputUsage, true);
    std::memcpy(idx.mapped, indices->data(), indices->size() * 4);

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;  // no any-hit work (Arm ray query guidance)
    auto& tri = geom.geometry.triangles;
    tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = address(pos);
    tri.vertexStride = 12;
    tri.maxVertex = static_cast<std::uint32_t>(p.size() / 3);
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = address(idx);

    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    ctx_.fns().as_build_sizes(ctx_.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &tri_count,
                              &sizes);

    destroy_accel(out);
    out.storage = vk::create_buffer(ctx_, sizes.accelerationStructureSize,
                                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    false);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = out.storage.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(ctx_.fns().create_as(ctx_.device(), &ci, nullptr, &out.as));

    const VkDeviceSize align = ctx_.scratch_alignment();
    vk::destroy(ctx_, scratch_);
    scratch_ = vk::create_buffer(ctx_, sizes.buildScratchSize + align,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    info.dstAccelerationStructure = out.as;
    info.scratchData.deviceAddress = (address(scratch_) + align - 1) / align * align;
    VkAccelerationStructureBuildRangeInfoKHR range{tri_count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vk::OneShot os(ctx_);
    ctx_.fns().cmd_build_as(os.cmd(), 1, &info, &ranges);
    os.submit_and_wait();
}

void RtScene::build(const CitySnapshot& snap, float radius) {
    const float cx = snap.roads.origin_x + snap.roads.texel * static_cast<float>(RoadField::kSize) * 0.5f;
    const float cy = snap.roads.origin_y + snap.roads.texel * static_cast<float>(RoadField::kSize) * 0.5f;
    const float r2 = radius * radius;
    auto near = [&](float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) < r2; };

    // ---- Opaque world ----
    std::vector<float> p;
    std::vector<std::uint32_t> idx;
    p.reserve(snap.mesh.vertices.size() * 3);
    for (const MeshVertex& v : snap.mesh.vertices) p.insert(p.end(), {v.x, v.y, v.z});
    for (const MeshChunk& c : snap.mesh.chunks) {
        if (!c.index_count) continue;
        // Whole tiles in or out (chunk bounds), so the index ranges stay contiguous.
        const float mx = std::clamp(cx, c.min[0], c.max[0]), my = std::clamp(cy, c.min[1], c.max[1]);
        if (!near(mx, my)) continue;
        idx.insert(idx.end(), snap.mesh.indices.begin() + c.first_index,
                   snap.mesh.indices.begin() + c.first_index + c.index_count);
    }
    for (const BoxInstance& b : snap.mesh.boxes)
        if (near(b.x, b.y)) add_box(p, idx, b.x, b.y, b.z0, b.yaw, b.hx, b.hy, b.height);
    for (const PropInstance& pr : snap.props) {
        const auto kind = static_cast<PropKind>(pr.kind_seed & 0xFFu);
        if (kind == PropKind::Frame || !near(pr.x, pr.y)) continue;  // lattice: light passes through
        add_box(p, idx, pr.x, pr.y, pr.z, pr.yaw, pr.sx, pr.sy, pr.sz);
    }
    // Expressway decks (shaders/include/highway.glsl): family 0 along y at 18 m, family 1
    // along x at 25 m.
    {
        const float lo_x = cx - radius, hi_x = cx + radius, lo_y = cy - radius, hi_y = cy + radius;
        for (float line = std::ceil(lo_x / kHighwayEvery) * kHighwayEvery; line <= hi_x; line += kHighwayEvery)
            add_box(p, idx, line, cy, 18.0f - 1.3f, 1.5707964f, radius, 5.8f, 1.3f);
        for (float line = std::ceil(lo_y / kHighwayEvery) * kHighwayEvery; line <= hi_y; line += kHighwayEvery)
            add_box(p, idx, cx, line, 25.0f - 1.3f, 0.0f, radius, 5.8f, 1.3f);
    }
    const auto world_tris = static_cast<std::uint32_t>(idx.size() / 3);
    build_blas(world_, world_pos_, world_idx_, p, &idx, world_tris);

    // ---- Signs: one quad each (signs.vert layout), primitive / 2 = sign index ----
    std::vector<float> sp;
    std::vector<std::uint32_t> sidx;
    sp.reserve(snap.signs.size() * 12);
    for (const SignInstance& s : snap.signs) {
        const float tx = -std::sin(s.yaw) * s.width * 0.5f, ty = std::cos(s.yaw) * s.width * 0.5f;
        const float hz = s.height * 0.5f;
        const auto base = static_cast<std::uint32_t>(sp.size() / 3);
        sp.insert(sp.end(), {s.x - tx, s.y - ty, s.z - hz, s.x + tx, s.y + ty, s.z - hz, s.x + tx, s.y + ty, s.z + hz,
                             s.x - tx, s.y - ty, s.z + hz});
        sidx.insert(sidx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    if (sidx.empty()) {  // keep a valid (degenerate) structure
        sp.assign(9, 0.0f);
        sidx = {0, 1, 2};
    }
    vk::Buffer sign_idx;
    build_blas(signs_, sign_pos_, sign_idx, sp, &sidx, static_cast<std::uint32_t>(sidx.size() / 3));
    vk::destroy(ctx_, sign_idx);

    // ---- Top level ----
    VkAccelerationStructureInstanceKHR inst[2]{};
    for (int i = 0; i < 2; ++i) {
        inst[i].transform.matrix[0][0] = inst[i].transform.matrix[1][1] = inst[i].transform.matrix[2][2] = 1.0f;
        inst[i].flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR | VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = i == 0 ? world_.as : signs_.as;
        inst[i].accelerationStructureReference = ctx_.fns().as_address(ctx_.device(), &ai);
    }
    inst[0].mask = kRtMaskWorld;
    inst[0].instanceCustomIndex = 0;
    inst[1].mask = kRtMaskSigns;
    inst[1].instanceCustomIndex = 1;
    vk::destroy(ctx_, instances_);
    instances_ = vk::create_buffer(ctx_, sizeof(inst), kInputUsage, true);
    std::memcpy(instances_.mapped, inst, sizeof(inst));

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.data.deviceAddress = address(instances_);
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geom;
    const std::uint32_t count = 2;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    ctx_.fns().as_build_sizes(ctx_.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
    destroy_accel(top_);
    top_.storage = vk::create_buffer(ctx_, sizes.accelerationStructureSize,
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     false);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = top_.storage.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VK_CHECK(ctx_.fns().create_as(ctx_.device(), &ci, nullptr, &top_.as));
    const VkDeviceSize align = ctx_.scratch_alignment();
    vk::destroy(ctx_, scratch_);
    scratch_ = vk::create_buffer(ctx_, sizes.buildScratchSize + align,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    info.dstAccelerationStructure = top_.as;
    info.scratchData.deviceAddress = (address(scratch_) + align - 1) / align * align;
    VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    {
        vk::OneShot os(ctx_);
        ctx_.fns().cmd_build_as(os.cmd(), 1, &info, &ranges);
        os.submit_and_wait();
    }
    vk::destroy(ctx_, scratch_);  // not needed until the next rebuild
    tlas_ = top_.as;
    triangles_ = world_tris + sidx.size() / 3;
    APEX_LOGI("ray query scene: %u world triangles, %zu signs", world_tris, snap.signs.size());
}

}  // namespace apex
