// GPU vertex layout of the detailed building meshes (see buildgen.hpp).
#pragma once

#include <cstdint>
#include <vector>

namespace apex {

// Surface kinds; shaders/include/building_surface.glsl switches on these.
enum class SurfaceMaterial : std::uint8_t {
    Facade = 0,      // procedural windows, style by district
    FacadeShop = 1,  // facade with a shopfront band in its lowest 4.6 m
    Glass = 2,       // curtain wall: dark glass, office floors
    Concrete = 3,
    Metal = 4,       // dark painted steel: fins, frames, rails, pipes
    Roof = 5,        // flat wet tar roof
    Corrugated = 6,  // corrugated sheet roofing
    ShantyWall = 7,  // patchwork siding, stalls and shutters at street level
    Awning = 8,      // coloured fabric / plastic awnings
    Led = 9,         // emissive accent strip in the building's accent colour
    LedRed = 10,     // red warning / crown lighting
    Louvre = 11,     // mechanical-floor grille
    SawGlass = 12,   // sawtooth roof glazing, lit from inside
    LitPanel = 13,   // canopy soffit with light panels
    Vending = 14,    // vending machine front: lit product rows
    Plastic = 15,    // glossy bin bags, crates
};

struct MeshVertex {
    float x, y, z;
    std::int8_t nx, ny, nz, pad;
    float u, v;  // walls: u = face * kFaceStride + metres from the face centre, v = z
    std::uint32_t building_material;  // bits 0-23 building index, 24-31 SurfaceMaterial
};
static_assert(sizeof(MeshVertex) == 28);

// Faces of one wall ring are laid out kFaceStride metres apart in u so the shader can
// recover the face (for per-face variation) and the position along it.
inline constexpr float kFaceStride = 256.0f;

// Indices [first_index, first_index + index_count) of one streaming tile, with bounds
// for frustum culling.
struct MeshChunk {
    std::uint32_t first_index = 0, index_count = 0;
    float min[3]{}, max[3]{};
};

// Local light (neon sign, shopfront, canopy): shaders/include/lighting.glsl.
struct PointLight {
    float x, y, z, radius;  // metres; no influence beyond radius
    float r, g, b;          // intensity (colour * power): irradiance ~ rgb / (d^2 + 1)
    float pad;
};
static_assert(sizeof(PointLight) == 32);

struct CityMesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MeshChunk> chunks;
};

}  // namespace apex
