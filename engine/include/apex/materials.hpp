// Photographic material textures (CC0, see assets/apk/materials/CREDITS.txt): decoded
// from the JPEG layers the APK ships and expanded into full mip chains for the renderer's
// two texture arrays (albedo + roughness-free colour, normal XY + roughness).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex {

struct MaterialTextures {
    static constexpr std::uint32_t kLayers = 16;  // shaders/include/materials.glsl
    // Albedo and normal arrays have their own resolution (colour carries the detail).
    std::uint32_t size = 0, mips = 0;          // albedo mip 0 edge (texels) and chain length
    std::uint32_t nrm_size = 0, nrm_mips = 0;  // normal + roughness
    // Per layer, mips from 0: size^2, (size/2)^2 ... RGBA8. Albedo is sRGB colour; nrm
    // holds normal X, Y (unorm, 0.5 = 0) and roughness in B.
    std::vector<std::uint8_t> albedo, nrm;
    std::size_t layer_bytes() const;      // bytes of one albedo layer's full mip chain
    std::size_t nrm_layer_bytes() const;  // same for the normal array
};

// Reads a file by name ("materials/0_albedo.jpg"); nullopt if missing.
using AssetReader = std::function<std::optional<std::vector<std::uint8_t>>(const std::string&)>;

// Decode all layers. nullopt if any layer is missing or malformed.
std::optional<MaterialTextures> load_material_textures(const AssetReader& read);

}  // namespace apex
