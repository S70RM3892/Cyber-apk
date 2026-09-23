#include "apex/world.hpp"

#include "apex/buildgen.hpp"
#include "apex/massing.hpp"
#include "apex/sign_text_data.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace apex {

namespace {
Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
}  // namespace

float unit(std::uint64_t h) { return static_cast<float>(h >> 40) * (1.0f / 16777216.0f); }

std::uint64_t building_hash(const city::Building& b) {
    const auto bx = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(b.x)));
    const auto by = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(b.y)));
    return city::hash64(city::hash64(bx) ^ (by * 0x9E3779B97F4A7C15ull));
}

namespace {
std::int32_t tile_of(float v, float tile) { return static_cast<std::int32_t>(std::floor(v / tile)); }
}  // namespace

Massing massing_of(const city::Building& b) {
    Massing m;
    const float r = unit(city::hash64(building_hash(b) ^ 0x7a11));
    const bool tall = b.district == city::District::Corporate || b.district == city::District::Megastructure;
    m.base_top = b.height;
    m.top_footprint = b.footprint;
    if (b.shanty) return m;
    if (tall && b.height > 45.0f) {
        m.tower = true;
        m.base_top = 10.0f + 8.0f * r;
        m.shaft = b.footprint * (0.62f + 0.12f * r);
        m.shaft_top = b.height * (0.78f + 0.12f * r);
        m.top_footprint = b.footprint * (0.40f + 0.10f * r);
        // Shaft plan: square, chamfered or octagonal.
        const float rc = unit(city::hash64(building_hash(b) ^ 0xc07));
        m.shaft_cut = rc < 0.35f ? 0.0f : rc < 0.75f ? 0.15f + 0.3f * (rc - 0.35f) : 0.586f;
    } else if (b.district == city::District::Residential && b.height > 30.0f) {
        m.stepped = true;
        m.base_top = b.height * (0.55f + 0.2f * r);
        m.top_footprint = b.footprint * 0.8f;
    }
    return m;
}

float shanty_roof_z(const city::Building& b, float x, float y) {
    // Mono-pitch: ~6 degrees down towards one of the four sides.
    const auto dir = static_cast<int>(city::hash64(building_hash(b) ^ 0x51a7) & 3u);
    const float d = dir == 0 ? x - b.x : dir == 1 ? b.x - x : dir == 2 ? y - b.y : b.y - y;
    return b.height - 0.1f * d;
}

bool has_sawtooth_roof(const city::Building& b) {
    return b.district == city::District::Industrial && !b.shanty && b.height < 22.0f && b.footprint > 18.0f &&
           unit(city::hash64(building_hash(b) ^ 0x5a37)) < 0.6f;
}

void add_building_boxes(const city::Building& b, std::vector<BuildingInstance>& out) {
    const std::uint64_t h = building_hash(b);
    const auto seed = static_cast<std::uint32_t>(h >> 32);
    const auto district = static_cast<std::uint32_t>(b.district);
    const std::uint32_t kind = b.shanty ? BuildingInstance::kShanty : 0u;
    auto box = [&](float footprint, float z0, float z1, bool top) {
        out.push_back({b.x, b.y, footprint, z1, seed, district, (top ? BuildingInstance::kTopTier : 0u) | kind, z0});
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
    if (b.shanty) count = 2;  // small shops: a sign or two each

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
        next() < (b.shanty ? 0.3f : 0.45f)) {
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
    // Tall vertical neon banners on tower shafts (Japanese, top to bottom), mounted at a
    // corner and sticking out from the wall like a giant blade sign.
    if (mass.tower && b.height > 70.0f && next() < 0.55f) {
        std::uint32_t text = pick_text(true);
        if (!signtext::kStrings[text].japanese) text = text % signtext::kJapaneseCount;
        const float n = static_cast<float>(length_of(text));
        const int face = static_cast<int>(next() * 4.0f) & 3;
        const float yaw = static_cast<float>(face) * (std::numbers::pi_v<float> * 0.5f);
        const float nx = std::cos(yaw), ny = std::sin(yaw), tx = -ny, ty = nx;
        SignInstance s{};
        s.style = pack(SignStyle::Blade, text);
        s.seed = static_cast<std::uint32_t>(city::hash64(h ^ 0xba7) >> 32);
        s.width = 2.5f + next() * 2.0f;
        s.height = s.width * (n * 0.92f + 0.35f);
        // Clear of the shaft's floor ledges (0.35 m), at the end of a flat face.
        const float out_dist = mass.shaft * 0.5f + s.width * 0.5f + 0.5f;
        const float corner = (next() < 0.5f ? -1.0f : 1.0f) * (mass.shaft_flat() * 0.5f - 1.0f);
        s.x = b.x + nx * out_dist + tx * corner;
        s.y = b.y + ny * out_dist + ty * corner;
        s.z = mass.base_top + 4.0f + s.height * 0.5f + next() * 25.0f;
        s.yaw = yaw + std::numbers::pi_v<float> * 0.5f;
        if (s.z + s.height * 0.5f < mass.shaft_top - 2.0f) out.push_back(s);
    }

    if (mass.tower && b.height > 60.0f) {
        const int screens = next() < 0.35f ? 1 : (next() < 0.7f ? 2 : 3);
        for (int k = 0; k < screens; ++k) {
            const int face = static_cast<int>(next() * 4.0f) & 3;
            const float yaw = static_cast<float>(face) * (std::numbers::pi_v<float> * 0.5f);
            const float nx = std::cos(yaw), ny = std::sin(yaw);
            SignInstance s{};
            s.style = pack(SignStyle::Screen, signtext::kJapaneseCount + static_cast<std::uint32_t>(next() * static_cast<float>(signtext::kLatinCount - 1)));
            s.seed = static_cast<std::uint32_t>(city::hash64(h ^ (0x5c + static_cast<std::uint64_t>(k))) >> 32);
            // Screens hang on the shaft tier's wall, above the podium.
            const float flat = mass.shaft_flat();
            const float max_w = flat * 0.85f;
            if (next() < 0.6f) {  // portrait
                s.width = std::min(max_w, 7.0f + next() * 6.0f);
                s.height = s.width * (2.2f + next() * 1.2f);
            } else {              // landscape
                s.width = std::min(max_w, 14.0f + next() * 12.0f);
                s.height = s.width * 0.56f;
            }
            const float face_offset = mass.shaft * 0.5f + 0.6f;  // in front of ledges and fins
            s.x = b.x + nx * face_offset + (-ny) * (next() - 0.5f) * (flat - s.width) * 0.8f;
            s.y = b.y + ny * face_offset + nx * (next() - 0.5f) * (flat - s.width) * 0.8f;
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

void place_props(const city::Building& b, std::vector<PropInstance>& props, std::vector<LightSprite>& lights) {
    std::uint64_t h = city::hash64(building_hash(b) ^ 0x9209);
    auto next = [&h] {
        h = city::hash64(h);
        return unit(h);
    };
    const Massing m = massing_of(b);
    const float roof = m.top_footprint;
    const float half = roof * 0.5f;
    const float top = b.height;
    auto seed = [&](PropKind k) { return static_cast<std::uint32_t>(k) | (static_cast<std::uint32_t>(h >> 40) << 8); };
    auto spot = [&](float margin) {
        return std::pair{b.x + (next() - 0.5f) * 2.0f * std::max(0.0f, half - margin),
                         b.y + (next() - 0.5f) * 2.0f * std::max(0.0f, half - margin)};
    };
    const float quarter = std::numbers::pi_v<float> * 0.5f;

    // AC units: shacks get one or two, bigger roofs a small farm.
    // Sawtooth warehouse roofs carry nothing (the teeth would swallow it).
    const bool saw = has_sawtooth_roof(b);
    const int ac = saw ? 0 : b.shanty ? static_cast<int>(next() * 2.5f) : 2 + static_cast<int>(next() * 4.0f);
    for (int i = 0; i < ac && roof > 4.0f; ++i) {
        const auto [x, y] = spot(1.5f);
        // Shack roofs slope: sit on the low side of the unit's footprint.
        const float z = b.shanty ? shanty_roof_z(b, x, y) - 0.1f : top;
        props.push_back({x, y, z, static_cast<float>(static_cast<int>(next() * 4.0f)) * quarter,
                         0.6f + next() * 0.5f, 0.45f + next() * 0.3f, 0.9f + next() * 0.6f, seed(PropKind::AcUnit)});
    }
    // Water tank on mid-rise roofs.
    if (!b.shanty && !saw && roof > 10.0f && next() < 0.6f) {
        const auto [x, y] = spot(3.0f);
        const float r = 1.2f + next() * 1.2f;
        props.push_back({x, y, top, 0.0f, r, r, 2.5f + next() * 2.5f, seed(PropKind::WaterTank)});
    }
    // Antenna masts with red aviation lights; taller on towers.
    const int masts = m.tower ? 1 + static_cast<int>(next() * 2.0f) : (next() < (b.shanty ? 0.25f : 0.55f) ? 1 : 0);
    for (int i = 0; i < masts; ++i) {
        const auto [x, y] = spot(1.0f);
        const float hgt = m.tower ? 10.0f + next() * 25.0f : (b.shanty ? 3.0f : 4.0f) + next() * 6.0f;
        props.push_back({x, y, top, 0.0f, 0.12f, 0.12f, hgt, seed(PropKind::Mast)});
        const float blink = next() < 0.5f ? 0.0f : 0.5f + next();
        lights.push_back({x, y, top + hgt, 0.3f, 6.0f, 0.18f, 0.12f, blink});
        if (hgt > 12.0f) lights.push_back({x, y, top + hgt * 0.55f, 0.25f, 5.0f, 0.15f, 0.1f, 0.0f});
    }
    // Lattice frames (billboard supports / cooling towers) on some mid-rise roofs, lined
    // with small red warning lights like the steel structures of the target look.
    if (!b.shanty && !saw && !m.tower && roof > 14.0f && next() < 0.35f) {
        const auto [x, y] = spot(5.0f);
        const float w = 3.0f + next() * 4.0f, hgt = 4.0f + next() * 6.0f;
        const float yaw = static_cast<float>(static_cast<int>(next() * 4.0f)) * quarter;
        props.push_back({x, y, top, yaw, w, 0.8f, hgt, seed(PropKind::Frame)});
        const float cx = std::cos(yaw), cy = std::sin(yaw);
        for (int k = -1; k <= 1; k += 2)
            lights.push_back({x + cx * w * static_cast<float>(k), y + cy * w * static_cast<float>(k), top + hgt, 0.25f,
                              8.0f, 0.2f, 0.15f, 0.0f});
    }
    // Aviation lights on the roof corners of every tall building.
    if (b.height > 45.0f)
        for (int k = 0; k < 4; ++k) {
            const float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? 1.0f : -1.0f;
            lights.push_back({b.x + sx * (half - 0.4f), b.y + sy * (half - 0.4f), top + 0.4f, 0.3f, 6.0f, 0.15f, 0.1f,
                              b.height > 90.0f ? 0.5f : 0.0f});
        }
}

PointLight sign_light(const SignInstance& s) {
    const std::uint32_t h = s.seed;
    Rgb c = neon_color(shader_hash(h));
    float emit = 0.8f;         // average radiance over the sign's area
    float out = 0.8f, dz = 0.0f;  // light position: in front of the face, and vertical shift
    const float area = s.width * s.height;
    switch (s.kind()) {
        case SignStyle::NeonText:  // letters only, lighting the roof they stand on
            if (shader_hash_f(h ^ 0x7eu) < 0.8f) c = neon_warm(h);
            emit = 1.1f;
            out = 0.0f;
            dz = -s.height * 0.45f;
            break;
        case SignStyle::Blade:  // two-sided, sticks out of the wall
            if (shader_hash_f(h ^ 0x7eu) < 0.55f) c = neon_warm(h);
            emit = 0.9f;
            out = 0.0f;
            break;
        case SignStyle::WallPanel:
            if (shader_hash_f(h ^ 0x44u) < 0.5f) c = mix(c, {1.0f, 1.0f, 1.0f}, 0.25f);
            emit = 0.7f;
            break;
        case SignStyle::Screen:  // the creative cycles: a neutral, slightly cool average
            c = mix(neon_color(shader_hash(h ^ 0x5cu)), {0.7f, 0.75f, 1.0f}, 0.6f);
            emit = 0.9f;
            out = 0.25f * std::sqrt(area);
            break;
        case SignStyle::Rooftop:
            c = mix(c, neon_color(shader_hash(h ^ 0x51u)), 0.5f);
            emit = 0.6f;
            out = 0.2f * std::sqrt(area);
            break;
    }
    const float power = std::min(emit * area, 900.0f);
    const float nx = std::cos(s.yaw), ny = std::sin(s.yaw);
    const float radius = std::clamp(std::sqrt(power) * 4.5f, 4.0f, 48.0f);
    return {s.x + nx * out, s.y + ny * out, s.z + dz, radius, c.r * power, c.g * power, c.b * power, 0.0f};
}

std::vector<std::uint32_t> build_light_grid(const std::vector<PointLight>& lights, float ox, float oy, float extent) {
    constexpr std::uint32_t n = kLightGridSize;
    const float cell = extent / static_cast<float>(n);
    std::vector<std::vector<std::uint32_t>> bins(kLightGridCells);
    for (std::uint32_t i = 0; i < lights.size(); ++i) {
        const PointLight& l = lights[i];
        const auto lo = [&](float v, float o) { return std::clamp(static_cast<int>(std::floor((v - o) / cell)), 0, int(n) - 1); };
        const int x0 = lo(l.x - l.radius, ox), x1 = lo(l.x + l.radius, ox);
        const int y0 = lo(l.y - l.radius, oy), y1 = lo(l.y + l.radius, oy);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                // Circle vs cell rectangle (in plan; lights reach any height within radius).
                const float cx0 = ox + static_cast<float>(x) * cell, cy0 = oy + static_cast<float>(y) * cell;
                const float dx = l.x - std::clamp(l.x, cx0, cx0 + cell), dy = l.y - std::clamp(l.y, cy0, cy0 + cell);
                if (dx * dx + dy * dy <= l.radius * l.radius)
                    bins[static_cast<std::size_t>(y) * n + static_cast<std::size_t>(x)].push_back(i);
            }
    }
    std::vector<std::uint32_t> grid(std::size_t{kLightGridCells} * 2);
    for (std::uint32_t c = 0; c < kLightGridCells; ++c) {
        auto& bin = bins[c];
        if (bin.size() > kMaxLightsPerCell) {
            // Keep the lights that matter most at the cell centre.
            const float px = ox + (static_cast<float>(c % n) + 0.5f) * cell;
            const float py = oy + (static_cast<float>(c / n) + 0.5f) * cell;
            auto weight = [&](std::uint32_t i) {
                const PointLight& l = lights[i];
                const float d2 = (l.x - px) * (l.x - px) + (l.y - py) * (l.y - py);
                return (l.r + l.g + l.b) / (d2 + 25.0f);
            };
            std::partial_sort(bin.begin(), bin.begin() + kMaxLightsPerCell, bin.end(),
                              [&](std::uint32_t a, std::uint32_t b) { return weight(a) > weight(b); });
            bin.resize(kMaxLightsPerCell);
        }
        grid[2 * c] = static_cast<std::uint32_t>(grid.size());
        grid[2 * c + 1] = static_cast<std::uint32_t>(bin.size());
        grid.insert(grid.end(), bin.begin(), bin.end());
    }
    return grid;
}

std::shared_ptr<const CitySnapshot> build_snapshot(const city::Params& p, std::int32_t ctx,
                                                   std::int32_t cty, float tile_size,
                                                   std::int32_t radius) {
    auto snap = std::make_shared<CitySnapshot>();
    snap->center_tx = ctx;
    snap->center_ty = cty;
    // Full-detail meshes near the streaming centre, massing-only meshes beyond.
    const float cx = (static_cast<float>(ctx) + 0.5f) * tile_size, cy = (static_cast<float>(cty) + 0.5f) * tile_size;
    const float detail_radius = tile_size * 2.6f;
    CityMesh& mesh = snap->mesh;
    for (std::int32_t ty = cty - radius; ty <= cty + radius; ++ty)
        for (std::int32_t tx = ctx - radius; tx <= ctx + radius; ++tx) {
            MeshChunk chunk;
            chunk.first_index = static_cast<std::uint32_t>(mesh.indices.size());
            chunk.min[0] = chunk.min[1] = chunk.min[2] = 1e30f;
            chunk.max[0] = chunk.max[1] = chunk.max[2] = -1e30f;
            for (const city::Building& b : city::generate_tile(p, tx, ty, tile_size)) {
                const auto first_box = static_cast<std::uint32_t>(snap->buildings.size());
                add_building_boxes(b, snap->buildings);
                for (std::size_t i = first_box; i < snap->buildings.size(); ++i)
                    snap->buildings[i].flags |= BuildingInstance::kMeshed;
                const std::size_t first_sign = snap->signs.size();
                place_signs(b, snap->signs);
                place_props(b, snap->props, snap->lights);
                const float d = std::hypot(b.x - cx, b.y - cy);
                build_building_mesh(b, first_box,
                                    std::span<const SignInstance>(snap->signs).subspan(first_sign),
                                    d < detail_radius ? MeshDetail::Full : MeshDetail::Massing, mesh,
                                    snap->point_lights);
                const float r = b.footprint * 0.5f + 5.0f;  // attachments stick out a little
                chunk.min[0] = std::min(chunk.min[0], b.x - r);
                chunk.min[1] = std::min(chunk.min[1], b.y - r);
                chunk.max[0] = std::max(chunk.max[0], b.x + r);
                chunk.max[1] = std::max(chunk.max[1], b.y + r);
                chunk.max[2] = std::max(chunk.max[2], b.height + 20.0f);
            }
            chunk.min[2] = 0.0f;
            chunk.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - chunk.first_index;
            if (chunk.index_count) mesh.chunks.push_back(chunk);
        }

    for (const SignInstance& s : snap->signs) snap->point_lights.push_back(sign_light(s));

    RoadField& rf = snap->roads;
    const float extent = static_cast<float>(2 * radius + 1) * tile_size;
    snap->light_grid = build_light_grid(snap->point_lights, static_cast<float>(ctx - radius) * tile_size,
                                        static_cast<float>(cty - radius) * tile_size, extent);
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

namespace {

// Push a circle out of an axis-aligned square (centre c, half extent h).
void push_out_of_square(Vec3& p, float cx, float cy, float h, float radius) {
    const float qx = std::clamp(p.x, cx - h, cx + h), qy = std::clamp(p.y, cy - h, cy + h);
    const float dx = p.x - qx, dy = p.y - qy;
    const float d2 = dx * dx + dy * dy;
    if (d2 >= radius * radius) return;
    if (d2 < 1e-8f) {  // centre inside: leave along the shallowest axis
        const float ox = (p.x >= cx ? h + radius : -h - radius), oy = (p.y >= cy ? h + radius : -h - radius);
        if (std::fabs(cx + ox - p.x) < std::fabs(cy + oy - p.y)) p.x = cx + ox; else p.y = cy + oy;
        return;
    }
    const float d = std::sqrt(d2);
    p.x = qx + dx / d * radius;
    p.y = qy + dy / d * radius;
}

}  // namespace

Vec3 World::move_with_collision(Vec3 from, Vec3 to, float radius) const {
    if (!current_) return to;
    Vec3 p = to;
    // Expressway piers in the medians of every kHighwayEvery-th street.
    for (int axis = 0; axis < 2; ++axis) {
        const float cross = axis == 0 ? p.x : p.y, along = axis == 0 ? p.y : p.x;
        const float line = std::round(cross / kHighwayEvery) * kHighwayEvery;
        if (std::fabs(cross - line) > kHighwayPierHalf + radius) continue;
        const float pier = std::round(along / kHighwaySegment) * kHighwaySegment;
        if (axis == 0) push_out_of_square(p, line, pier, kHighwayPierHalf, radius);
        else push_out_of_square(p, pier, line, kHighwayPierHalf, radius);
    }
    // Two relaxation passes resolve corners where two buildings' pushes interact.
    for (int pass = 0; pass < 2; ++pass) {
        for (const BuildingInstance& b : current_->buildings) {
            const float half = b.footprint * 0.5f;
            if (std::fabs(p.x - b.x) > half + radius || std::fabs(p.y - b.y) > half + radius) continue;
            if (p.z >= b.height - 0.05f || p.z + 1.8f < b.base_z) continue;  // standing on it / passing under
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

float World::ground_height(float x, float y, float feet, float step_up) const {
    float ground = 0.0f;
    if (!current_) return ground;
    for (const BuildingInstance& b : current_->buildings) {
        const float half = b.footprint * 0.5f;
        if (std::fabs(x - b.x) > half || std::fabs(y - b.y) > half) continue;
        if (b.height <= feet + step_up) ground = std::max(ground, b.height);
    }
    return ground;
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
