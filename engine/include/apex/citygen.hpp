// Seed-driven procedural city layout (spec §5).
//
// The layout is a stateless function of (seed, world position): district sites
// are jittered-grid Voronoi points, roads are the Voronoi cell borders, and lots
// inside a district are hashed per block. Any tile can therefore be expanded
// independently and in any order, which is what streaming needs — no global
// city graph is stored in the package.
//
// All randomness uses an explicit integer hash, not <random> distributions,
// whose output is implementation-defined and would differ between the host
// asset build (glibc/libstdc++) and the device (bionic/libc++).
#pragma once

#include <cstdint>
#include <vector>

namespace apex::city {

enum class District : std::uint8_t {
    Megastructure,  // arcology / megablock towers
    Corporate,      // high-rise downtown
    Residential,    // dense mid-rise blocks
    Industrial,     // low, wide, pipe-heavy
    Count,
};

struct Params {
    std::uint64_t seed = 0;
    float district_size = 400.0f;  // metres between Voronoi sites (grid pitch)
    float road_half_width = 9.0f;  // half-width of arterial roads on cell borders
    float block_size = 60.0f;      // metres per building lot grid inside a district
    float street_half_width = 7.0f;  // side-street corridor: 4.5 m roadway + sidewalk
};

struct DistrictSample {
    District district;
    std::uint32_t site_id;     // stable id of the nearest Voronoi site
    float border_distance;     // approx. distance to the nearest cell border (metres)
    bool on_road;
};

DistrictSample sample(const Params& p, float x, float y);

// A building placed from the modular kit (spec §5 "モジュールアセンブリ").
// Rendering instantiates `module_id` with GPU instancing; nothing here is unique geometry.
struct Building {
    float x, y;             // centre (metres)
    float footprint;        // square footprint edge (metres)
    float height;           // metres
    std::uint16_t module_id;
    District district;
    bool shanty = false;    // low-rise market / shack cluster building (corrugated roof)
};

// Expand all buildings whose lot centre lies inside the square tile
// [tx*tile_size, (tx+1)*tile_size) x [ty*tile_size, (ty+1)*tile_size).
std::vector<Building> generate_tile(const Params& p, std::int32_t tx, std::int32_t ty,
                                    float tile_size);

// Exposed for tests: 64-bit avalanche hash (SplitMix64 finalizer).
std::uint64_t hash64(std::uint64_t x);

}  // namespace apex::city
