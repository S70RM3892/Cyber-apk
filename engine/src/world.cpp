#include "apex/world.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace apex {

namespace {

float unit(std::uint64_t h) { return static_cast<float>(h >> 40) * (1.0f / 16777216.0f); }

std::uint64_t building_hash(const city::Building& b) {
    const auto bx = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(b.x)));
    const auto by = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(b.y)));
    return city::hash64(city::hash64(bx) ^ (by * 0x9E3779B97F4A7C15ull));
}

std::int32_t tile_of(float v, float tile) { return static_cast<std::int32_t>(std::floor(v / tile)); }

}  // namespace

void add_building_boxes(const city::Building& b, std::vector<BuildingInstance>& out) {
    const std::uint64_t h = building_hash(b);
    const auto seed = static_cast<std::uint32_t>(h >> 32);
    const auto district = static_cast<std::uint32_t>(b.district);
    auto box = [&](float footprint, float z0, float z1, bool top) {
        out.push_back({b.x, b.y, footprint, z1, seed, district, top ? BuildingInstance::kTopTier : 0u, z0});
    };
    const float r = unit(city::hash64(h ^ 0x7a11));
    const bool tall = b.district == city::District::Corporate || b.district == city::District::Megastructure;
    if (tall && b.height > 45.0f) {
        // Podium, shaft, crown: the classic setback tower silhouette.
        const float podium = 10.0f + 8.0f * r;
        const float shaft_top = b.height * (0.78f + 0.12f * r);
        box(b.footprint, 0.0f, podium, false);
        box(b.footprint * (0.62f + 0.12f * r), podium, shaft_top, false);
        box(b.footprint * (0.40f + 0.10f * r), shaft_top, b.height, true);
    } else if (b.district == city::District::Residential && b.height > 30.0f) {
        const float split = b.height * (0.55f + 0.2f * r);
        box(b.footprint, 0.0f, split, false);
        box(b.footprint * 0.8f, split, b.height, true);
    } else {
        box(b.footprint, 0.0f, b.height, true);
    }
}

void place_signs(const city::Building& b, std::vector<SignInstance>& out) {
    std::uint64_t h = building_hash(b);
    auto next = [&h] {
        h = city::hash64(h);
        return unit(h);
    };

    int count = 0;
    switch (b.district) {
        case city::District::Megastructure: count = 9; break;
        case city::District::Corporate: count = 4; break;
        case city::District::Residential: count = 8; break;
        case city::District::Industrial: count = 2; break;
        case city::District::Count: break;
    }

    const float half = b.footprint * 0.5f;
    for (int i = 0; i < count; ++i) {
        // Face 0..3: +X, +Y, -X, -Y. The sign's yaw points along the outward normal.
        const int face = static_cast<int>(next() * 4.0f) & 3;
        const float yaw = static_cast<float>(face) * (std::numbers::pi_v<float> * 0.5f);
        const float nx = std::cos(yaw), ny = std::sin(yaw);
        const float tx = -ny, ty = nx;  // along the wall
        const float along = (next() - 0.5f) * b.footprint * 0.8f;
        // Street-level density: most signs sit in the first ~30 m, where the player looks.
        const float top = std::min(b.height - 2.0f, 24.0f);
        const float z = 3.0f + next() * std::max(0.0f, top - 3.0f);

        SignInstance s{};
        s.seed = static_cast<std::uint32_t>(h >> 32);
        if (next() < 0.45f) {
            // Vertical blade sticking out of the wall.
            s.style = static_cast<std::uint32_t>(SignStyle::Blade);
            s.width = 1.4f + next() * 1.2f;
            s.height = 4.0f + next() * 7.0f;
            const float out_dist = half + s.width * 0.5f + 0.2f;
            s.x = b.x + nx * out_dist + tx * along;
            s.y = b.y + ny * out_dist + ty * along;
            s.yaw = yaw + std::numbers::pi_v<float> * 0.5f;  // blade plane is perpendicular to the wall
        } else {
            s.style = static_cast<std::uint32_t>(SignStyle::WallPanel);
            s.width = 2.0f + next() * 6.0f;
            s.height = 1.0f + next() * 3.0f;
            s.x = b.x + nx * (half + 0.15f) + tx * along;
            s.y = b.y + ny * (half + 0.15f) + ty * along;
            s.yaw = yaw;
        }
        s.z = std::min(z, b.height - s.height * 0.5f);
        if (s.z - s.height * 0.5f < 2.5f) s.z = 2.5f + s.height * 0.5f;  // keep head clearance
        out.push_back(s);
    }

    // Landmark rooftop billboards on tall corporate/megastructure towers.
    if ((b.district == city::District::Corporate || b.district == city::District::Megastructure) &&
        next() < 0.35f) {
        const int face = static_cast<int>(next() * 4.0f) & 3;
        const float yaw = static_cast<float>(face) * (std::numbers::pi_v<float> * 0.5f);
        SignInstance s{};
        s.style = static_cast<std::uint32_t>(SignStyle::Rooftop);
        s.seed = static_cast<std::uint32_t>(city::hash64(h) >> 32);
        s.width = b.footprint * 0.8f;
        s.height = s.width * 0.35f;
        s.x = b.x + std::cos(yaw) * (b.footprint * 0.5f + 0.2f);
        s.y = b.y + std::sin(yaw) * (b.footprint * 0.5f + 0.2f);
        s.z = b.height - s.height * 0.5f - 1.0f;
        s.yaw = yaw;
        if (s.z - s.height * 0.5f > 10.0f) out.push_back(s);
    }
}

std::shared_ptr<const CitySnapshot> build_snapshot(const city::Params& p, std::int32_t ctx,
                                                   std::int32_t cty, float tile_size,
                                                   std::int32_t radius) {
    auto snap = std::make_shared<CitySnapshot>();
    snap->center_tx = ctx;
    snap->center_ty = cty;
    for (std::int32_t ty = cty - radius; ty <= cty + radius; ++ty)
        for (std::int32_t tx = ctx - radius; tx <= ctx + radius; ++tx)
            for (const city::Building& b : city::generate_tile(p, tx, ty, tile_size)) {
                add_building_boxes(b, snap->buildings);
                place_signs(b, snap->signs);
            }

    RoadField& rf = snap->roads;
    const float extent = static_cast<float>(2 * radius + 1) * tile_size;
    rf.texel = extent / static_cast<float>(RoadField::kSize);
    rf.origin_x = static_cast<float>(ctx - radius) * tile_size;
    rf.origin_y = static_cast<float>(cty - radius) * tile_size;
    rf.data.resize(std::size_t{RoadField::kSize} * RoadField::kSize);
    for (std::uint32_t j = 0; j < RoadField::kSize; ++j)
        for (std::uint32_t i = 0; i < RoadField::kSize; ++i) {
            const float wx = rf.origin_x + (static_cast<float>(i) + 0.5f) * rf.texel;
            const float wy = rf.origin_y + (static_cast<float>(j) + 0.5f) * rf.texel;
            const float d = city::sample(p, wx, wy).border_distance;
            const float v = std::clamp(d / RoadField::kRange, 0.0f, 1.0f);
            rf.data[std::size_t{j} * RoadField::kSize + i] = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
        }
    return snap;
}

World::World(const city::Params& params, float tile_size, std::int32_t radius_tiles)
    : params_(params), tile_size_(tile_size), radius_tiles_(radius_tiles) {}

World::~World() {
    if (has_pending_) pending_.wait();
}

void World::request(std::int32_t tx, std::int32_t ty) {
    pending_tx_ = tx;
    pending_ty_ = ty;
    has_pending_ = true;
    pending_ = std::async(std::launch::async, [p = params_, tx, ty, ts = tile_size_, r = radius_tiles_] {
        return build_snapshot(p, tx, ty, ts, r);
    });
}

bool World::update(Vec3 pos) {
    const std::int32_t tx = tile_of(pos.x, tile_size_);
    const std::int32_t ty = tile_of(pos.y, tile_size_);

    bool swapped = false;
    if (has_pending_ && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        current_ = pending_.get();
        has_pending_ = false;
        swapped = true;
    }
    const bool wanted_current = current_ && current_->center_tx == tx && current_->center_ty == ty;
    const bool wanted_pending = has_pending_ && pending_tx_ == tx && pending_ty_ == ty;
    if (!wanted_current && !wanted_pending && !has_pending_) request(tx, ty);
    return swapped;
}

void World::wait_ready() {
    if (has_pending_) {
        current_ = pending_.get();
        has_pending_ = false;
    }
}

Vec3 World::move_with_collision(Vec3 from, Vec3 to, float radius) const {
    if (!current_) return to;
    Vec3 p = to;
    // Two relaxation passes resolve corners where two buildings' pushes interact.
    for (int pass = 0; pass < 2; ++pass) {
        for (const BuildingInstance& b : current_->buildings) {
            const float half = b.footprint * 0.5f;
            if (std::fabs(p.x - b.x) > half + radius || std::fabs(p.y - b.y) > half + radius) continue;
            if (p.z > b.height || p.z < b.base_z) continue;  // above / below this box
            const float cx = std::clamp(p.x, b.x - half, b.x + half);
            const float cy = std::clamp(p.y, b.y - half, b.y + half);
            float dx = p.x - cx, dy = p.y - cy;
            float d2 = dx * dx + dy * dy;
            if (d2 >= radius * radius) continue;
            if (d2 < 1e-8f) {
                // Centre inside the box: push out through the side we came from.
                dx = from.x - b.x;
                dy = from.y - b.y;
                if (std::fabs(dx) > std::fabs(dy)) {
                    p.x = b.x + (dx > 0 ? half + radius : -half - radius);
                } else {
                    p.y = b.y + (dy > 0 ? half + radius : -half - radius);
                }
                continue;
            }
            const float d = std::sqrt(d2);
            p.x = cx + dx / d * radius;
            p.y = cy + dy / d * radius;
        }
    }
    return p;
}

Vec3 World::find_spawn(Vec3 near) const {
    // Spiral search for the centre line of an arterial road.
    for (int r = 0; r < 400; r += 4)
        for (int a = 0; a < 16; ++a) {
            const float ang = static_cast<float>(a) * (std::numbers::pi_v<float> / 8.0f);
            const float x = near.x + std::cos(ang) * static_cast<float>(r);
            const float y = near.y + std::sin(ang) * static_cast<float>(r);
            if (city::sample(params_, x, y).border_distance < 2.0f) return {x, y, near.z};
        }
    return near;
}

}  // namespace apex
