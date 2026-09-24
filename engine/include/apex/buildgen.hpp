// Detailed building geometry. The massing boxes (world.hpp) stay the collision and
// far-LOD representation; this turns each building into real triangles: chamfered
// tower shafts with floor ledges, fins and stepped / tapered crowns, apartment blocks
// with open-air corridors, balconies and stair cores, shacks with sloped corrugated
// roofs and awnings, sawtooth-roofed warehouses with chimneys and pipe runs.
//
// Everything is a pure function of the building (and the signs already hung on it,
// which the geometry keeps clear of), so any tile can be rebuilt independently.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "apex/city_mesh.hpp"
#include "apex/citygen.hpp"
#include "apex/world.hpp"

namespace apex {

// Close: Near plus modelled windows, wall and roof equipment, stairs, lanterns (the
// few hundred metres the player can walk to). Near: everything, plus facade relief
// (piers / spandrel bands framing each window). Full: attachments and clutter.
// Massing: silhouettes only.
enum class MeshDetail { Close, Near, Full, Massing };

// Append the geometry of one building. `building_index` is the index of its first
// massing box in CitySnapshot::buildings (seed / district for the shader). `signs` are
// the signs placed on this building; attachments that would cut through one are dropped.
// Light-emitting parts (shopfronts, lit canopies) append to `lights`.
void build_building_mesh(const city::Building& b, std::uint32_t building_index, std::span<const SignInstance> signs,
                         MeshDetail detail, CityMesh& out, std::vector<PointLight>& lights);

// Overhead power / data cables strung between neighbouring low-rise buildings, sagging
// across streets and alleys (the wire tangle of the target look).
struct CableAnchor {
    float x, y, footprint, height;
    std::uint32_t building_index;
};
void build_cables(std::span<const CableAnchor> anchors, CityMesh& out);

// Enclosed sky bridges between neighbouring towers whose shafts face each other.
struct TowerAnchor {
    float x, y, half;       // shaft centre and half width
    float base_top, shaft_top;
    std::uint32_t building_index;
    bool corporate;
};
void build_skybridges(std::span<const TowerAnchor> towers, CityMesh& out, std::vector<PointLight>& lights);

// Mirrors of the shader palettes in shaders/include/city_common.glsl.
Rgb neon_color(std::uint32_t h);
Rgb neon_warm(std::uint32_t h);

// Same 32-bit hash as shaders/include/city_common.glsl hash_u / hash_f, so geometry can
// line up with shader-side patterns (window grids).
std::uint32_t shader_hash(std::uint32_t v);
float shader_hash_f(std::uint32_t v);

}  // namespace apex
