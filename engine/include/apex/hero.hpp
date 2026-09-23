// The spawn set: the first view of the game is laid out by hand to one composition
// (a rooftop over a shack market: red rooftop neon, a steel gantry with warning lights,
// a fashion billboard, an overpass, downtown towers with giant screens behind), instead
// of whatever the procedural city happens to put there.
//
// Local frame: x forward along the view heading, y to the left, z up, origin under the
// camera. The procedural lots in the view are replaced by authored lots (which still go
// through the normal building generator, so they get the same detail) plus bespoke
// geometry for the foreground pieces.
#pragma once

#include <vector>

#include "apex/citygen.hpp"
#include "apex/math.hpp"
#include "apex/world.hpp"

namespace apex::hero {

// Just north of an expressway line (y = 0), so the next one is ~460 m ahead, beyond the set.
inline constexpr float kOriginX = 240.0f, kOriginY = 20.0f;  // world position of the viewpoint
inline constexpr float kHeading = 1.5707964f;                   // world yaw of local +x (north)
inline constexpr float kRoofZ = 12.0f;                          // roof the player starts on
inline constexpr float kPitch = 0.06f;                          // start looking slightly up

// Local (forward, left, z) to world.
Vec3 to_world(float fx, float ly, float z);

// An authored lot, run through the procedural building generator.
struct Lot {
    city::Building building;
    bool procedural_signs = true;    // also hang the generator's signs
    std::vector<SignInstance> signs; // authored signs on this building
};
const std::vector<Lot>& lots();

// True for procedural lots the set replaces.
bool suppresses(const city::Building& b);

// Bespoke foreground geometry, its collision boxes, signs and lights (one mesh chunk).
void build(CitySnapshot& snap);

}  // namespace apex::hero
