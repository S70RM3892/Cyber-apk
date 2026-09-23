#include "apex/world.hpp"

#include "apex/sign_text_data.hpp"

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

namespace {

// Massing parameters shared by the box stack and the sign placement, so signs sit on
// actual walls.
struct Massing {
    bool tower = false;     // podium + shaft + crown
    bool stepped = false;   // residential: lower block + narrower upper block
    float base_top = 0;     // top of the full-footprint box (podium / lower block / whole building)
    float shaft = 0;        // tower shaft footprint (m)
    float shaft_top = 0;    // tower shaft top (m)
    float top_footprint = 0;  // footprint of the box carrying the roof
};

Massing massing_of(const city::Building& b) {
    Massing m;
    const float r = unit(city::hash64(building_hash(b) ^ 0x7a11));
    const bool tall = b.district == city::District::Corporate || b.district == city::District::Megastructure;
    m.base_top = b.height;
    m.top_footprint = b.footprint;
    if (tall && b.height > 45.0f) {
        m.tower = true;
        m.base_top = 10.0f + 8.0f * r;
        m.shaft = b.footprint * (0.62f + 0.12f * r);
        m.shaft_top = b.height * (0.78f + 0.12f * r);
        m.top_footprint = b.footprint * (0.40f + 0.10f * r);
    } else if (b.district == city::District::Residential && b.height > 30.0f) {
        m.stepped = true;
        m.base_top = b.height * (0.55f + 0.2f * r);
        m.top_footprint = b.footprint * 0.8f;
    }
    return m;
}

}  // namespace

void add_building_boxes(const city::Building& b, std::vector<BuildingInstance>& out) {
    const std::uint64_t h = building_hash(b);
    const auto seed = static_cast<std::uint32_t>(h >> 32);
    const auto district = static_cast<std::uint32_t>(b.district);
    auto box = [&](float footprint, float z0, float z1, bool top) {
        out.push_back({b.x, b.y, footprint, z1, seed, district, top ? BuildingInstance::kTopTier : 0u, z0});
    };
    const Massing m = massing_of(b);
    if (m.tower) {
        // Podium, shaft, crown: the classic setback tower silhouette.
        box(b.footprint, 0.0f, m.base_top, false);
        box(m.shaft, m.base_top, m.shaft_top, false);
        box(m.top_footprint, m.shaft_top, b.height, true);
    } else if (m.stepped) {
        box(b.footprint, 0.0f, m.base_top, false);
        box(m.top_footprint, m.base_top, b.height, true);
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
    auto pack = [](SignStyle st, std::uint32_t text) { return static_cast<std::uint32_t>(st) | (text << 8); };
    // Pick a sign string: mostly Japanese in the older districts, brand names downtown.
    const bool downtown = b.district == city::District::Corporate;
    auto pick_text = [&](bool prefer_japanese) {
        const bool ja = next() < (prefer_japanese ? 0.85f : 0.3f);
        const std::uint32_t n = ja ? signtext::kJapaneseCount : signtext::kLatinCount;
        const auto i = std::min(n - 1, static_cast<std::uint32_t>(next() * static_cast<float>(n)));
        return ja ? i : signtext::kJapaneseCount + i;
    };
    auto length_of = [](std::uint32_t text) { return std::max<std::uint32_t>(1, signtext::kStrings[text].length); };

    int count = 0;
    switch (b.district) {
        case city::District::Megastructure: count = 9; break;
        case city::District::Corporate: count = 4; break;
        case city::District::Residential: count = 8; break;
        case city::District::Industrial: count = 3; break;
        case city::District::Count: break;
    }

    const float half = b.footprint * 0.5f;
    const Massing mass = massing_of(b);
    for (int i = 0; i < count; ++i) {
        // Face 0..3: +X, +Y, -X, -Y. The sign's yaw points along the outward normal.
        const int face = static_cast<int>(next() * 4.0f) & 3;
        const float yaw = static_cast<float>(face) * (std::numbers::pi_v<float> * 0.5f);
        const float nx = std::cos(yaw), ny = std::sin(yaw);
        const float tx = -ny, ty = nx;  // along the wall
        const float along = (next() - 0.5f) * b.footprint * 0.8f;
        // Street-level density: most signs sit in the first ~24 m, where the player looks.
        const float top = std::min(mass.base_top - 1.5f, 24.0f);
        const float z = 3.0f + next() * std::max(0.0f, top - 3.0f);

        SignInstance s{};
        s.seed = static_cast<std::uint32_t>(h >> 32);
        if (next() < 0.5f) {
            // Vertical blade sticking out of the wall; Japanese text runs top to bottom.
            std::uint32_t text = pick_text(true);
            if (!signtext::kStrings[text].japanese) text = text % signtext::kJapaneseCount;
            const float n = static_cast<float>(length_of(text));
            s.style = pack(SignStyle::Blade, text);
            s.width = 1.2f + next() * 0.8f;
            s.height = s.width * (n * 0.92f + 0.35f);
            const float out_dist = half + s.width * 0.5f + 0.2f;
            s.x = b.x + nx * out_dist + tx * along;
            s.y = b.y + ny * out_dist + ty * along;
            s.yaw = yaw + std::numbers::pi_v<float> * 0.5f;  // blade plane is perpendicular to the wall
        } else {
            const std::uint32_t text = pick_text(!downtown);
            const float n = static_cast<float>(length_of(text));
            s.style = pack(SignStyle::WallPanel, text);
            s.height = 0.9f + next() * 1.6f;
            s.width = s.height * (n * 0.8f + 0.4f);
            s.x = b.x + nx * (half + 0.15f) + tx * along;
            s.y = b.y + ny * (half + 0.15f) + ty * along;
            s.yaw = yaw;
        }
        s.z = std::min(z, mass.base_top - s.height * 0.5f);
        if (s.z - s.height * 0.5f < 2.5f) s.z = 2.5f + s.height * 0.5f;  // keep head clearance
        // Too tall for the wall it hangs on (short podium, long string): skip it.
        if (s.z + s.height * 0.5f <= mass.base_top) out.push_back(s);
    }

    // Big neon lettering standing on the roof of low buildings ("24時間営業" style).
    if (b.height < 40.0f && (b.district == city::District::Residential || b.district == city::District::Industrial ||
                             b.district == city::District::Megastructure) &&
        next() < 0.45f) {
        const int face = static_cast<int>(next() * 4.0f) & 3;
        const float yaw = static_cast<float>(face) * (std::numbers::pi_v<float> * 0.5f);
        std::uint32_t text = pick_text(true);
        const float n = static_cast<float>(length_of(text));
        SignInstance s{};
        s.style = pack(SignStyle::NeonText, text);
        s.seed = static_cast<std::uint32_t>(city::hash64(h ^ 0x77) >> 32);
        const float roof = mass.top_footprint;
        s.height = std::min(2.2f + next() * 2.0f, roof * 0.8f / n);
        s.width = s.height * n;
        s.x = b.x + std::cos(yaw) * (roof * 0.5f - 1.0f);
        s.y = b.y + std::sin(yaw) * (roof * 0.5f - 1.0f);
        s.z = b.height + s.height * 0.5f + 0.6f;
        s.yaw = yaw;
        if (s.height > 1.0f) out.push_back(s);
    }

    // Giant video screens on tall towers, facing the street.
    if (mass.tower && b.height > 60.0f) {
        const int screens = next() < 0.5f ? 1 : 2;
        for (int k = 0; k < screens; ++k) {
            const int face = static_cast<int>(next() * 4.0f) & 3;
            const float yaw = static_cast<float>(face) * (std::numbers::pi_v<float> * 0.5f);
            const float nx = std::cos(yaw), ny = std::sin(yaw);
            SignInstance s{};
            s.style = pack(SignStyle::Screen, signtext::kJapaneseCount + static_cast<std::uint32_t>(next() * static_cast<float>(signtext::kLatinCount - 1)));
            s.seed = static_cast<std::uint32_t>(city::hash64(h ^ (0x5c + static_cast<std::uint64_t>(k))) >> 32);
            // Screens hang on the shaft tier's wall, above the podium.
            const float max_w = mass.shaft * 0.85f;
            if (next() < 0.6f) {  // portrait
                s.width = std::min(max_w, 7.0f + next() * 6.0f);
                s.height = s.width * (2.2f + next() * 1.2f);
            } else {              // landscape
                s.width = std::min(max_w, 14.0f + next() * 12.0f);
                s.height = s.width * 0.56f;
            }
            const float face_offset = mass.shaft * 0.5f + 0.25f;
            s.x = b.x + nx * face_offset + (-ny) * (next() - 0.5f) * (mass.shaft - s.width) * 0.8f;
            s.y = b.y + ny * face_offset + nx * (next() - 0.5f) * (mass.shaft - s.width) * 0.8f;
            s.z = mass.base_top + 2.0f + s.height * 0.5f + next() * 30.0f;
            s.yaw = yaw;
            if (s.z + s.height * 0.5f < mass.shaft_top - 2.0f) out.push_back(s);
        }
    }

    // Landmark rooftop billboards on tall corporate/megastructure towers.
    if ((b.district == city::District::Corporate || b.district == city::District::Megastructure) &&
        next() < 0.35f) {
        const int face = static_cast<int>(next() * 4.0f) & 3;
        const float yaw = static_cast<float>(face) * (std::numbers::pi_v<float> * 0.5f);
        SignInstance s{};
        s.style = pack(SignStyle::Rooftop, pick_text(false));
        s.seed = static_cast<std::uint32_t>(city::hash64(h) >> 32);
        const float wall = mass.top_footprint;  // the billboard hangs on the top tier
        s.width = wall * 0.8f;
        s.height = s.width * 0.35f;
        s.x = b.x + std::cos(yaw) * (wall * 0.5f + 0.2f);
        s.y = b.y + std::sin(yaw) * (wall * 0.5f + 0.2f);
        s.z = b.height - s.height * 0.5f - 1.0f;
        s.yaw = yaw;
        if (s.z - s.height * 0.5f > std::max(10.0f, mass.tower ? mass.shaft_top : 0.0f)) out.push_back(s);
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
