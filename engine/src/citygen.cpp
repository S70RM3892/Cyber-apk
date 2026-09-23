#include "apex/citygen.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace apex::city {

std::uint64_t hash64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

namespace {

std::uint64_t hash_cell(std::uint64_t seed, std::int32_t cx, std::int32_t cy, std::uint64_t salt) {
    std::uint64_t h = hash64(seed ^ salt);
    h = hash64(h ^ static_cast<std::uint32_t>(cx));
    return hash64(h ^ (std::uint64_t{static_cast<std::uint32_t>(cy)} << 32));
}

// Uniform float in [0, 1) from the top 24 bits.
float unit(std::uint64_t h) { return static_cast<float>(h >> 40) * (1.0f / 16777216.0f); }

constexpr std::uint64_t kSaltSiteX = 0x51;
constexpr std::uint64_t kSaltSiteY = 0x52;
constexpr std::uint64_t kSaltDistrict = 0x53;
constexpr std::uint64_t kSaltLot = 0x54;

struct Site {
    float x, y;
    std::int32_t cx, cy;
};

Site site_of(const Params& p, std::int32_t cx, std::int32_t cy) {
    const float jx = unit(hash_cell(p.seed, cx, cy, kSaltSiteX));
    const float jy = unit(hash_cell(p.seed, cx, cy, kSaltSiteY));
    return {(static_cast<float>(cx) + jx) * p.district_size,
            (static_cast<float>(cy) + jy) * p.district_size, cx, cy};
}

std::int32_t cell_of(float v, float pitch) {
    return static_cast<std::int32_t>(std::floor(v / pitch));
}

District district_of(const Params& p, const Site& s) {
    const auto h = hash_cell(p.seed, s.cx, s.cy, kSaltDistrict);
    // Weighted: megastructures are rare landmarks.
    const float u = unit(h);
    if (u < 0.08f) return District::Megastructure;
    if (u < 0.35f) return District::Corporate;
    if (u < 0.75f) return District::Residential;
    return District::Industrial;
}

}  // namespace

DistrictSample sample(const Params& p, float x, float y) {
    const std::int32_t pcx = cell_of(x, p.district_size);
    const std::int32_t pcy = cell_of(y, p.district_size);

    // Jitter spans the whole cell, so a 5x5 search is needed to guarantee the
    // true nearest site (a 3x3 search can miss it near cell corners).
    Site nearest{};
    float best = std::numeric_limits<float>::max();
    for (std::int32_t dy = -2; dy <= 2; ++dy) {
        for (std::int32_t dx = -2; dx <= 2; ++dx) {
            const Site s = site_of(p, pcx + dx, pcy + dy);
            const float d2 = (s.x - x) * (s.x - x) + (s.y - y) * (s.y - y);
            if (d2 < best) {
                best = d2;
                nearest = s;
            }
        }
    }

    // Distance to the cell border = min distance to the bisector with each neighbour.
    float border = std::numeric_limits<float>::max();
    for (std::int32_t dy = -2; dy <= 2; ++dy) {
        for (std::int32_t dx = -2; dx <= 2; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const Site s = site_of(p, nearest.cx + dx, nearest.cy + dy);
            const float ex = s.x - nearest.x, ey = s.y - nearest.y;
            const float len = std::sqrt(ex * ex + ey * ey);
            if (len <= 0.0f) continue;
            const float mx = 0.5f * (s.x + nearest.x), my = 0.5f * (s.y + nearest.y);
            border = std::min(border, ((mx - x) * ex + (my - y) * ey) / len);
        }
    }

    const auto id = static_cast<std::uint32_t>(hash_cell(p.seed, nearest.cx, nearest.cy, 0));
    return {district_of(p, nearest), id, border, border < p.road_half_width};
}

std::vector<Building> generate_tile(const Params& p, std::int32_t tx, std::int32_t ty,
                                    float tile_size) {
    // Tile edges are computed the same way for neighbouring tiles so every lot
    // belongs to exactly one tile.
    const float x0 = static_cast<float>(tx) * tile_size;
    const float x1 = static_cast<float>(tx + 1) * tile_size;
    const float y0 = static_cast<float>(ty) * tile_size;
    const float y1 = static_cast<float>(ty + 1) * tile_size;

    const float b = p.block_size;
    const float max_footprint = b - 2.0f * p.street_half_width;
    std::vector<Building> out;
    for (std::int32_t j = cell_of(y0, b) - 1; j <= cell_of(y1, b) + 1; ++j) {
        for (std::int32_t i = cell_of(x0, b) - 1; i <= cell_of(x1, b) + 1; ++i) {
            const float cx = (static_cast<float>(i) + 0.5f) * b;
            const float cy = (static_cast<float>(j) + 0.5f) * b;
            if (cx < x0 || cx >= x1 || cy < y0 || cy >= y1) continue;

            const DistrictSample ds = sample(p, cx, cy);
            const auto h = hash_cell(p.seed, i, j, kSaltLot);
            const float u0 = unit(h), u1 = unit(hash64(h)), u2 = unit(hash64(h ^ 1));

            float footprint = max_footprint * (0.6f + 0.4f * u0);
            // Keep the whole footprint (half-diagonal) clear of the arterial road.
            const float clearance = p.road_half_width + footprint * 0.7072f;
            if (ds.border_distance < clearance) {
                footprint = (ds.border_distance - p.road_half_width) / 0.7072f;
                if (footprint < 0.25f * max_footprint) continue;  // lot swallowed by the road
            }

            float height = 0.0f;
            std::uint16_t module_base = 0;
            switch (ds.district) {
                case District::Megastructure: height = 250.0f + 350.0f * u1; module_base = 0; break;
                case District::Corporate:     height = 80.0f + 170.0f * u1;  module_base = 16; break;
                case District::Residential:   height = 20.0f + 60.0f * u1;   module_base = 32; break;
                case District::Industrial:    height = 8.0f + 22.0f * u1;    module_base = 48; break;
                case District::Count: break;
            }
            const auto module_id = static_cast<std::uint16_t>(module_base + static_cast<std::uint16_t>(u2 * 16.0f));

            // Low-rise sprawl: some lots become clusters of 1-3 storey shacks and market
            // halls split by alleys. They open sight lines over the rooftops to the towers.
            const float u3 = unit(hash64(h ^ 2));
            float shanty_chance = 0.0f;
            switch (ds.district) {
                case District::Residential: shanty_chance = 0.38f; break;
                case District::Industrial: shanty_chance = 0.45f; break;
                case District::Megastructure: shanty_chance = 0.2f; break;
                default: break;
            }
            if (u3 < shanty_chance && footprint > 0.6f * max_footprint) {
                const int k = unit(hash64(h ^ 3)) < 0.55f ? 2 : 3;
                const float cell = footprint / static_cast<float>(k);
                const float alley = 2.4f;
                for (int sy = 0; sy < k; ++sy)
                    for (int sx = 0; sx < k; ++sx) {
                        const std::uint64_t hs = hash64(h ^ (0x100u + static_cast<std::uint64_t>(sy * k + sx)));
                        const float jitter = (unit(hs) - 0.5f) * 0.25f * alley;
                        const float bx = cx + (static_cast<float>(sx) + 0.5f - 0.5f * static_cast<float>(k)) * cell + jitter;
                        const float by = cy + (static_cast<float>(sy) + 0.5f - 0.5f * static_cast<float>(k)) * cell;
                        const float fp = cell - alley - std::fabs(jitter) * 2.0f;
                        const float hgt = 3.8f + 8.0f * unit(hash64(hs));
                        out.push_back({bx, by, fp, hgt, module_id, ds.district, true});
                    }
                continue;
            }
            out.push_back({cx, cy, footprint, height, module_id, ds.district, false});
        }
    }
    return out;
}

}  // namespace apex::city
