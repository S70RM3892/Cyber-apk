// Building massing: the coarse volumes (podium / shaft / top tier) shared by the
// collision boxes, sign placement and the detailed mesh generator, so signs sit on
// actual walls and the player walks on actual roofs.
#pragma once

#include <cstdint>

#include "apex/citygen.hpp"

namespace apex {

struct Massing {
    bool tower = false;       // podium + shaft + crown
    bool stepped = false;     // residential: lower block + narrower upper block
    float base_top = 0;       // top of the full-footprint box (podium / lower block / whole building)
    float shaft = 0;          // tower shaft footprint (m)
    float shaft_top = 0;      // tower shaft top (m)
    float top_footprint = 0;  // footprint of the box carrying the roof
    float shaft_cut = 0;      // tower shaft corner chamfer, as a fraction of its half width
    // Width of each flat shaft face (signs and screens must fit on it).
    float shaft_flat() const { return shaft * (1.0f - shaft_cut); }
};

// Uniform [0, 1) from the high bits of a 64-bit hash.
float unit(std::uint64_t h);
// Stable per-building hash (from its lot position).
std::uint64_t building_hash(const city::Building& b);
Massing massing_of(const city::Building& b);

// Low-rise shacks get a gently sloped corrugated roof: height of the roof surface at
// (x, y), equal to b.height at the building centre.
float shanty_roof_z(const city::Building& b, float x, float y);

// Warehouses with a sawtooth north-light roof (no roof clutter on top).
bool has_sawtooth_roof(const city::Building& b);

}  // namespace apex
