#include "apex/materials.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#include "stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace apex {

std::size_t MaterialTextures::layer_bytes() const {
    std::size_t total = 0;
    for (std::uint32_t m = 0, s = size; m < mips; ++m, s = std::max(1u, s / 2)) total += std::size_t{s} * s * 4;
    return total;
}

namespace {

// Append a full mip chain of an RGBA8 image (box filter; sRGB data is averaged in linear).
void append_chain(std::vector<std::uint8_t>& out, const std::uint8_t* rgba, std::uint32_t size, std::uint32_t mips,
                  bool srgb) {
    auto to_lin = [](std::uint8_t v) {
        const float c = static_cast<float>(v) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    auto to_srgb = [](float l) {
        l = std::clamp(l, 0.0f, 1.0f);
        const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
        return static_cast<std::uint8_t>(std::lround(c * 255.0f));
    };
    std::vector<float> cur(std::size_t{size} * size * 4);
    for (std::size_t i = 0; i < cur.size(); ++i)
        cur[i] = (srgb && (i & 3) != 3) ? to_lin(rgba[i]) : static_cast<float>(rgba[i]) / 255.0f;
    std::uint32_t s = size;
    for (std::uint32_t m = 0; m < mips; ++m) {
        for (std::size_t i = 0; i < cur.size(); ++i) {
            const float v = cur[i];
            out.push_back((srgb && (i & 3) != 3) ? to_srgb(v) : static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
        }
        if (m + 1 == mips) break;
        const std::uint32_t n = std::max(1u, s / 2);
        std::vector<float> next(std::size_t{n} * n * 4);
        for (std::uint32_t y = 0; y < n; ++y)
            for (std::uint32_t x = 0; x < n; ++x)
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](std::uint32_t xx, std::uint32_t yy) {
                        return cur[(std::size_t{std::min(yy, s - 1)} * s + std::min(xx, s - 1)) * 4 + static_cast<std::size_t>(c)];
                    };
                    next[(std::size_t{y} * n + x) * 4 + static_cast<std::size_t>(c)] =
                        0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
                }
        cur.swap(next);
        s = n;
    }
}

}  // namespace

std::optional<MaterialTextures> load_material_textures(const AssetReader& read) {
    MaterialTextures t;
    for (std::uint32_t layer = 0; layer < MaterialTextures::kLayers; ++layer) {
        for (int kind = 0; kind < 2; ++kind) {
            const std::string name = "materials/" + std::to_string(layer) + (kind == 0 ? "_albedo.jpg" : "_nrm.jpg");
            const auto file = read(name);
            if (!file) return std::nullopt;
            int w = 0, h = 0, comp = 0;
            stbi_uc* px = stbi_load_from_memory(file->data(), static_cast<int>(file->size()), &w, &h, &comp, 4);
            if (!px) return std::nullopt;
            if (w != h || w <= 0 || (t.size && static_cast<std::uint32_t>(w) != t.size)) {
                stbi_image_free(px);
                return std::nullopt;
            }
            if (!t.size) {
                t.size = static_cast<std::uint32_t>(w);
                for (std::uint32_t s = t.size; s >= 1; s /= 2) ++t.mips;
            }
            // Averaged normals shorten at coarse mips; the shader renormalises, which also
            // flattens distant detail (a cheap stand-in for Toksvig filtering).
            append_chain(kind == 0 ? t.albedo : t.nrm, px, t.size, t.mips, kind == 0);
            stbi_image_free(px);
        }
    }
    return t;
}

}  // namespace apex
