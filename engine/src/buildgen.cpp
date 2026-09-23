#include "apex/buildgen.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

#include "build_kit.hpp"
#include "apex/massing.hpp"

namespace apex {

std::uint32_t shader_hash(std::uint32_t v) {
    const std::uint32_t state = v * 747796405u + 2891336453u;
    const std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float shader_hash_f(std::uint32_t v) { return static_cast<float>(shader_hash(v) & 0x00FFFFFFu) / 16777216.0f; }

Rgb neon_color(std::uint32_t h) {
    static constexpr Rgb kPalette[16] = {
        {1.00f, 0.07f, 0.10f}, {1.00f, 0.07f, 0.10f}, {1.00f, 0.10f, 0.08f}, {1.00f, 0.16f, 0.12f},
        {1.00f, 0.12f, 0.45f}, {1.00f, 0.10f, 0.35f}, {1.00f, 0.25f, 0.55f}, {0.85f, 0.10f, 1.00f},
        {0.05f, 0.85f, 1.00f}, {0.15f, 0.95f, 0.95f}, {0.10f, 0.65f, 1.00f}, {1.00f, 0.40f, 0.06f},
        {1.00f, 0.55f, 0.12f}, {1.00f, 0.82f, 0.68f}, {0.25f, 0.45f, 1.00f}, {1.00f, 0.85f, 0.15f}};
    return kPalette[h & 15u];
}

Rgb neon_warm(std::uint32_t h) {
    static constexpr Rgb kPalette[4] = {{1.0f, 0.06f, 0.09f}, {1.0f, 0.1f, 0.06f}, {1.0f, 0.12f, 0.4f}, {1.0f, 0.3f, 0.5f}};
    return kPalette[h & 3u];
}

namespace {

using namespace kit;

std::uint32_t shader_seed(const city::Building& b) { return static_cast<std::uint32_t>(building_hash(b) >> 32); }

// Face seed as building_surface.glsl derives it for mesh face k.
std::uint32_t face_seed(const city::Building& b, int k) {
    return shader_hash(shader_seed(b) ^ static_cast<std::uint32_t>(k + 3));
}

// ---- Facade relief -----------------------------------------------------------------
// The window grid of building_surface.glsl facade(), mirrored so geometry can frame it.
struct FacadeGrid {
    int style;        // 0 punched, 1 curtain wall, 2 dense megastructure grid
    float pitch, fh;  // window pitch along the wall, floor height (m)
    float wx0, wy0;   // glass starts this far into each cell (fractions)
};
FacadeGrid facade_grid(const city::Building& b, bool curtain) {
    const auto d = static_cast<unsigned>(b.district);
    FacadeGrid g{};
    g.style = curtain ? 1 : d == 0 ? 2 : d == 1 ? 1 : 0;
    g.fh = d == 2 ? 3.1f : 3.8f;
    g.pitch = g.style == 2 ? 1.6f : 1.8f + 0.8f * shader_hash_f(shader_seed(b) ^ 0xa5u);
    g.wx0 = g.style == 2 ? 0.2f : 0.22f;
    g.wy0 = g.style == 2 ? 0.3f : 0.32f;
    return g;
}

// Box standing on a face, open at the back (against the wall): front, ends, top, bottom.
void relief_box(Builder& g, const Face& f, float a0, float a1, float o0, float o1, float z0, float z1, M mat) {
    // Local +y of a face-aligned box points into the wall: that face is never seen.
    g.box(f.point((a0 + a1) * 0.5f, (o0 + o1) * 0.5f), f.t, (a1 - a0) * 0.5f, (o1 - o0) * 0.5f, z0, z1, mat, mat, mat,
          true);
}

// Horizontal relief run along a face, broken around signs.
void relief_strip(Builder& g, const Face& f, float a0, float a1, float o0, float o1, float z0, float z1, M mat) {
    std::vector<std::pair<float, float>> cuts;
    const Box3 whole = f.bounds(a0, a1, o0, o1 + 0.2f, z0, z1);
    for (const Box3& s : g.signs()) {
        if (!overlaps(s, whole)) continue;
        const float sa0 = (s.x0 - f.centre.x) * f.t.x + (s.y0 - f.centre.y) * f.t.y;
        const float sa1 = (s.x1 - f.centre.x) * f.t.x + (s.y1 - f.centre.y) * f.t.y;
        cuts.push_back({std::min(sa0, sa1) - 0.05f, std::max(sa0, sa1) + 0.05f});
    }
    std::sort(cuts.begin(), cuts.end());
    float a = a0;
    for (auto [c0, c1] : cuts) {
        if (c0 > a + 0.3f) relief_box(g, f, a, std::min(c0, a1), o0, o1, z0, z1, mat);
        a = std::max(a, c1);
    }
    if (a1 > a + 0.3f) relief_box(g, f, a, a1, o0, o1, z0, z1, mat);
}

// Piers between the windows and spandrel bands across each floor line, lined up with
// the shader's window grid so the glass reads as recessed. mode: 0 grid, 1 horizontal
// (deep bands, ribbon windows), 2 vertical (deep piers).
void facade_relief(Builder& g, const city::Building& b, const Face& f, float z0, float z1, bool curtain, int mode) {
    if (z1 - z0 < 3.0f) return;
    const FacadeGrid gr = facade_grid(b, curtain);
    const float band_d = mode == 1 ? 0.65f : mode == 2 ? 0.12f : 0.35f;
    const float pier_d = mode == 2 ? 0.7f : mode == 1 ? 0.12f : 0.28f;
    const M mat = curtain ? M::Metal : M::Concrete;
    const float hl = f.half_len;
    // Spandrel bands: centred on each floor line (curtain walls: the slab edge above it).
    for (float j = std::ceil(z0 / gr.fh); j * gr.fh < z1; j += 1.0f) {
        const float line = j * gr.fh;
        float zb0 = curtain ? line : line - (1.0f - 0.8f) * gr.fh;
        float zb1 = curtain ? line + 0.18f * gr.fh : line + gr.wy0 * gr.fh;
        zb0 = std::max(zb0, z0);
        zb1 = std::min(zb1, z1);
        if (zb1 - zb0 > 0.1f) relief_strip(g, f, -hl, hl, 0.0f, band_d, zb0, zb1, mat);
    }
    // Piers on the cell boundaries (curtain walls: slim fins every 3 m).
    const float step = curtain ? 3.0f : gr.pitch;
    const float w = curtain ? 0.14f : 2.0f * gr.wx0 * gr.pitch;
    const float depth = curtain ? pier_d + 0.15f : pier_d;
    for (float k = std::ceil((-hl + w) / step); k * step <= hl - w; k += 1.0f) {
        const float a = k * step;
        const Box3 col = f.bounds(a - w * 0.5f, a + w * 0.5f, 0.0f, depth + 0.2f, z0, z1);
        for (auto [s0, s1] : g.free_spans(col.x0, col.y0, col.x1, col.y1, z0, z1, 1.0f))
            relief_box(g, f, a - w * 0.5f, a + w * 0.5f, 0.0f, depth, s0, s1, mat);
    }
    // Corner columns close the relief at the face ends.
    const float cw = 0.45f, cd = std::max(band_d, pier_d);
    for (float a : {-hl + cw * 0.5f, hl - cw * 0.5f}) {
        const Box3 col = f.bounds(a - cw * 0.5f, a + cw * 0.5f, 0.0f, cd + 0.2f, z0, z1);
        for (auto [s0, s1] : g.free_spans(col.x0, col.y0, col.x1, col.y1, z0, z1, 1.0f))
            relief_box(g, f, a - cw * 0.5f, a + cw * 0.5f, 0.0f, cd, s0, s1, mat);
    }
}

// Spill from the shopfront band of a facade (building_surface.glsl facade(), shop_band).
void shopfront_lights(Builder& g, const city::Building& b, const Plan& p) {
    for (int k = 0; k < 4; ++k) {
        const Face f = face_of(p, k);
        const Rgb c = mix({1.0f, 0.8f, 0.65f}, neon_color(shader_hash(face_seed(b, k))), 0.6f);
        const int n = std::max(1, static_cast<int>(f.half_len / 12.0f));
        for (int i = 0; i < n; ++i) {
            const float a = (static_cast<float>(i) + 0.5f) / static_cast<float>(n) * 2.0f * f.half_len - f.half_len;
            g.light(at(f.point(a, 2.5f), 3.0f), c, 9.0f);
        }
    }
}

// Soffit light panels (detail.frag kMatLitPanel): tint matches the shader's choice.
Rgb panel_tint(const city::Building& b) {
    const std::uint32_t seed = shader_seed(b);
    const Rgb accent = neon_color(shader_hash(seed ^ 0xbeefu));
    return mix({1.0f, 0.88f, 0.72f}, accent, shader_hash_f(seed ^ 0x1eu) >= 0.6f ? 0.7f : 0.0f);
}

// ---- Towers: podium, shaft, crown -------------------------------------------------
void tower(Builder& g, const city::Building& b, const Massing& m, MeshDetail detail) {
    Rng rng{building_hash(b) ^ 0x70e5};
    const bool corp = b.district == city::District::Corporate;
    const M skin = corp ? M::Glass : M::Facade;
    const bool full = detail != MeshDetail::Massing;
    const Plan podium{b.x, b.y, b.footprint * 0.5f};
    const Plan shaft{b.x, b.y, m.shaft * 0.5f, m.shaft_cut};
    const Plan top{b.x, b.y, m.top_footprint * 0.5f};
    const float h = b.height;
    constexpr float kFloor = 3.8f;  // building_surface.glsl floor height for towers

    // Podium with a heavy cornice; its roof is a terrace around the shaft.
    g.walls(podium.points(), 0.0f, m.base_top - 0.9f, M::FacadeShop);
    g.solid(podium.offset(0.5f).points(), m.base_top - 0.9f, m.base_top, M::Concrete, M::Roof, M::Concrete);
    if (full) g.parapet(podium.offset(0.5f), 0.3f, m.base_top, 1.0f, M::Concrete);

    // Street canopies with lit soffits on some podium faces.
    if (full)
        for (int k = 0; k < 4; ++k) {
            if (!rng.chance(0.55f)) continue;
            const Face f = face_of(podium, k);
            const float depth = rng.range(1.8f, 3.2f);
            face_strip(g, f, -f.half_len + 0.6f, f.half_len - 0.6f, 0.0f, depth, 4.4f, 4.75f, M::Metal, M::Metal,
                       M::LitPanel);
            for (float a = -f.half_len + 6.0f; a < f.half_len - 3.0f; a += 12.0f)
                g.light(at(f.point(a, depth * 0.5f), 3.9f), panel_tint(b), 1.6f * 12.0f * depth * 0.5f);
        }
    if (full) shopfront_lights(g, b, podium);

    // Shaft, articulated into stacked blocks: recessed "waists" (plant floors behind
    // louvres and fins) split it where no sign hangs, so towers don't read as prisms.
    struct Span {
        float z0, z1;
    };
    std::vector<Span> solid_spans, waists;
    {
        Rng wr{building_hash(b) ^ 0xa157};
        const float r = shaft.half + 1.5f;
        auto free = g.free_spans(b.x - r, b.y - r, b.x + r, b.y + r, m.base_top + 10.0f, m.shaft_top - 10.0f, 24.0f);
        for (auto [f0, f1] : free) {
            if (!wr.chance(0.7f)) continue;
            const float len_w = std::min(f1 - f0 - 4.0f, wr.range(10.0f, 26.0f));
            const float w0 = std::floor((f0 + wr.range(2.0f, f1 - f0 - len_w - 2.0f)) / kFloor) * kFloor;
            if (len_w > 7.0f && w0 > f0 - 0.1f) waists.push_back({w0, w0 + std::floor(len_w / kFloor) * kFloor});
        }
        float z0 = m.base_top;
        for (const Span& w : waists) {
            solid_spans.push_back({z0, w.z0});
            z0 = w.z1;
        }
        solid_spans.push_back({z0, m.shaft_top});
    }
    const float inset = 1.2f + 2.0f * unit(city::hash64(building_hash(b) ^ 0x1115));
    const Plan waist_plan = shaft.offset(-inset);
    for (const Span& sp : solid_spans) g.walls(shaft.points(), sp.z0, sp.z1, skin);
    for (const Span& w : waists) {
        g.walls(waist_plan.points(), w.z0, w.z1, M::Louvre);
        g.ring(shaft.points(), waist_plan.points(), w.z0, M::Roof);                   // top of the block below
        auto so = shaft.points(), si = waist_plan.points();
        std::reverse(so.begin(), so.end());
        std::reverse(si.begin(), si.end());
        g.ring(so, si, w.z1, M::Metal);                                                 // soffit of the block above
        if (full) {
            // Structural columns carry the block above across the waist.
            for (int k = 0; k < 4; ++k) {
                const Face f = face_of(shaft, k);
                for (float a = -f.half_len + 1.0f; a <= f.half_len - 1.0f; a += (f.half_len * 2.0f - 2.0f) / 4.0f)
                    relief_box(g, f, a - 0.4f, a + 0.4f, -inset, -0.05f, w.z0, w.z1, M::Concrete);
                // A lit band along the soffit edge.
                relief_box(g, f, -f.half_len + 0.3f, f.half_len - 0.3f, 0.0f, 0.12f, w.z1 - 0.35f, w.z1 - 0.1f,
                           rng.chance(0.5f) ? M::Led : M::LedRed);
            }
        }
    }
    auto in_waist = [&](float z) {
        return std::any_of(waists.begin(), waists.end(), [z](const Span& w) { return z > w.z0 - 1.0f && z < w.z1 + 1.0f; });
    };
    const bool near = detail == MeshDetail::Near;
    const int relief_mode = static_cast<int>(city::hash64(building_hash(b) ^ 0x4e1f) % 3u);
    if (near) {
        for (int k = 0; k < 4; ++k) {
            facade_relief(g, b, face_of(podium, k), 5.0f, m.base_top - 0.9f, corp, relief_mode);
            for (const Span& sp : solid_spans) facade_relief(g, b, face_of(shaft, k), sp.z0, sp.z1, corp, relief_mode);
        }
    }
    if (full) {
        // Floor ledges every few floors.
        const int every = 2 + rng.index(4);
        const float step = kFloor * static_cast<float>(every);
        const M ledge = rng.chance(0.5f) ? M::Concrete : M::Metal;
        for (float z = std::ceil((m.base_top + 2.0f) / step) * step; z < m.shaft_top - 3.0f; z += step)
            if (!in_waist(z)) g.solid(shaft.offset(0.35f).points(), z + 0.05f, z + 0.5f, ledge);
        // Mechanical floors: louvred bands every ~90 m.
        for (float z = m.base_top + 60.0f + rng.range(0.0f, 30.0f); z < m.shaft_top - 20.0f; z += rng.range(80.0f, 110.0f)) {
            const float zf = std::floor(z / kFloor) * kFloor;
            if (!in_waist(zf) && !in_waist(zf + kFloor * 2.0f))
                g.solid(shaft.offset(0.15f).points(), zf, zf + kFloor * 2.0f, M::Louvre, M::Metal, M::Metal);
        }

        const float style = rng.next();
        const float half = shaft.half;
        if (style < 0.45f && !near) {
            // Vertical fins on the flat faces (the relief replaces them up close).
            const float spacing = rng.range(2.6f, 4.5f), depth = rng.range(0.4f, 0.8f);
            for (int k = 0; k < 4; ++k) {
                const Face f = face_of(shaft, k);
                const int count = static_cast<int>((2.0f * f.half_len - 1.0f) / spacing);
                for (int i = 0; i <= count; ++i) {
                    const float a = -0.5f * spacing * static_cast<float>(count) + spacing * static_cast<float>(i);
                    for (const Span& sp : solid_spans) {
                        const Box3 col = f.bounds(a - 0.15f, a + 0.15f, 0.0f, depth, sp.z0, sp.z1);
                        for (auto [z0, z1] : g.free_spans(col.x0, col.y0, col.x1, col.y1, sp.z0, sp.z1, 2.0f))
                            relief_box(g, f, a - 0.15f, a + 0.15f, 0.0f, depth, z0, z1, M::Metal);
                    }
                }
            }
        } else if (style < 0.75f) {
            if (m.shaft_cut < 0.01f) {
                // Square shaft: heavy corner piers.
                for (int k = 0; k < 4; ++k) {
                    const float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? 1.0f : -1.0f;
                    const V2 c{b.x + sx * (half - 0.4f), b.y + sy * (half - 0.4f)};
                    for (const Span& sp : solid_spans)
                        g.box(c, {1, 0}, 1.2f, 1.2f, sp.z0, sp.z1 + (sp.z1 >= m.shaft_top ? 1.5f : 0.0f), M::Concrete,
                              M::Concrete, M::Concrete);
                }
            } else {
                // Chamfered shaft: LED lines down the diagonal faces, upper part only.
                const auto pts = shaft.points();
                const float zl = m.base_top + (m.shaft_top - m.base_top) * 0.45f;
                for (std::size_t k = 1; k < pts.size(); k += 2) {
                    const V2 a = pts[k], c = pts[(k + 1) % pts.size()];
                    const V2 mid = (a + c) * 0.5f;
                    const V2 along = (c - a) * (1.0f / len(c - a));
                    const V2 out{along.y, -along.x};
                    for (const Span& sp : solid_spans)
                        if (sp.z1 > zl)
                            g.box(mid + out * 0.1f, along, 0.25f, 0.12f, std::max(sp.z0, zl), sp.z1, M::Led, M::Led, M::Led);
                }
            }
        }
        // Vertical LED spine on some corporate towers.
        if (corp && rng.chance(0.3f))
            for (int k = 0; k < 4; k += 2) {
                const Face f = face_of(shaft, (k + rng.index(2)) & 3);
                for (const Span& sp : solid_spans) {
                    const float z0 = std::max(sp.z0, m.base_top + 6.0f);
                    const Box3 col = f.bounds(-0.4f, 0.4f, 0.0f, 0.3f, z0, sp.z1);
                    for (auto [s0, s1] : g.free_spans(col.x0, col.y0, col.x1, col.y1, z0, sp.z1, 3.0f))
                        face_box(g, f, -0.4f, 0.4f, 0.0f, 0.25f, s0, s1, M::Led, M::Led, M::Led);
                }
            }
        // Cantilevered pods: rooms hung off the shaft, lit along their underside.
        Rng pr{building_hash(b) ^ 0x90d5};
        for (int k = 0; k < 4; ++k) {
            if (!pr.chance(0.45f)) continue;
            const Face f = face_of(shaft, k);
            const float w = pr.range(6.0f, std::min(14.0f, f.half_len * 1.5f)), out = pr.range(3.0f, 7.0f);
            const float hgt = std::floor(pr.range(2.0f, 4.0f)) * kFloor;
            const float a = pr.range(-f.half_len + w * 0.5f + 0.5f, f.half_len - w * 0.5f - 0.5f);
            const float z0 = std::floor(pr.range(m.base_top + 12.0f, m.shaft_top - hgt - 6.0f) / kFloor) * kFloor;
            if (z0 < m.base_top + 8.0f || in_waist(z0) || in_waist(z0 + hgt)) continue;
            if (!g.clear(f.bounds(a - w * 0.5f, a + w * 0.5f, 0.0f, out + 0.5f, z0 - 1.0f, z0 + hgt + 1.0f))) continue;
            face_box(g, f, a - w * 0.5f, a + w * 0.5f, 0.0f, out, z0, z0 + hgt, skin, M::Roof, M::Metal);
            relief_box(g, f, a - w * 0.5f, a + w * 0.5f, out, out + 0.12f, z0 - 0.05f, z0 + 0.25f, M::Led);
            g.light(at(f.point(a, out * 0.5f), z0 - 1.0f), panel_tint(b), 20.0f);
        }
    }

    // Crown.
    const float tier = h - m.shaft_top;
    const float crown = rng.next();
    float z = m.shaft_top;
    if (crown < 0.4f && m.shaft_cut < 0.01f) {
        // Tapered: the shaft leans in to the top block.
        const float th = std::min(0.35f * tier, tier - 8.0f);
        if (th > 2.0f) {
            g.walls(shaft.points(), z, top.points(), z + th, skin);
            z += th;
        }
    } else if (crown < 0.75f) {
        // Stepped: two intermediate setbacks, each with a lit lip.
        const float th = std::min(0.4f * tier, tier - 8.0f);
        if (th > 3.0f) {
            for (int s = 1; s <= 2; ++s) {
                const float f = static_cast<float>(s) / 3.0f;
                Plan p = shaft.with_half(shaft.half + (top.half - shaft.half) * f);
                p.cut = shaft.cut * (1.0f - f);
                g.cap(p.offset(0.3f).points(), z, true, M::Roof);
                if (full) g.solid(p.offset(0.3f).points(), z - 0.4f, z, M::Led, M::Metal, M::Metal);
                g.walls(p.points(), z, z + th / 2.0f, skin);
                z += th / 2.0f;
            }
        }
    }
    g.cap(shaft.points(), m.shaft_top, true, M::Roof);  // hidden under a taper, a terrace otherwise
    if (z > m.shaft_top) g.cap(top.offset(0.3f).points(), z, true, M::Roof);
    g.walls(top.points(), z, h, skin);
    if (near)
        for (int k = 0; k < 4; ++k) facade_relief(g, b, face_of(top, k), z, h - 2.5f, corp, relief_mode);
    g.cap(top.points(), h, true, M::Roof);
    if (!full) return;
    g.parapet(top, 0.3f, h, 1.1f, M::Concrete);
    if (rng.chance(0.55f)) {
        const Plan band = top.offset(0.1f);
        const Box3 bb{b.x - band.half, b.y - band.half, h - 2.4f, b.x + band.half, b.y + band.half, h - 1.4f};
        if (g.clear(bb)) g.solid(band.points(), h - 2.4f, h - 1.6f, rng.chance(0.5f) ? M::LedRed : M::Led);
    }
    if (h > 120.0f && rng.chance(0.4f)) {
        // Open steel crown frame above the roof edge.
        const float fh = rng.range(6.0f, 14.0f), e = top.half - 0.15f;
        const V2 corner[4] = {{b.x + e, b.y - e}, {b.x + e, b.y + e}, {b.x - e, b.y + e}, {b.x - e, b.y - e}};
        for (int k = 0; k < 4; ++k) {
            const V2 a = corner[k], c = corner[(k + 1) % 4];
            g.beam(at(a, h + 1.1f), at(a, h + 1.1f + fh), 0.45f, M::Metal);
            g.beam(at(a, h + 1.1f + fh), at(c, h + 1.1f + fh), 0.35f, M::Metal);
            g.beam(at(a, h + 1.1f), at(c, h + 1.1f + fh), 0.25f, M::Metal);
            g.beam(at(c, h + 1.1f), at(a, h + 1.1f + fh), 0.25f, M::Metal);
        }
        g.solid(top.offset(0.05f).points(), h + 1.1f + fh - 0.1f, h + 1.1f + fh + 0.25f, M::LedRed, M::Metal, M::Metal);
    }
}

// ---- Mid-rise blocks: apartments and warehouses ----------------------------------
void sawtooth_roof(Builder& g, const Plan& p, float h) {
    const float span = p.half * 2.0f;
    const int teeth = std::max(2, static_cast<int>(span / 7.0f));
    const float w = span / static_cast<float>(teeth), rise = 2.4f;
    const float x0 = p.cx - p.half, y0 = p.cy - p.half, y1 = p.cy + p.half;
    for (int i = 0; i < teeth; ++i) {
        const float xa = x0 + w * static_cast<float>(i), xb = xa + w;
        const Vec3 a0{xa, y0, h}, a1{xa, y1, h}, b0{xb, y0, h + rise}, b1{xb, y1, h + rise};
        const Vec3 bb0{xb, y0, h}, bb1{xb, y1, h};
        // Sloped sheet rising towards +X, then a vertical glazed face looking along +X.
        g.quad(a0, a1, b1, b0, {xa, y0}, {xa, y1}, {xb, y1}, {xb, y0}, M::Corrugated);
        g.wall_quad(bb0, bb1, b1, b0, -p.half, p.half, M::SawGlass);
        g.tri(a0, b0, bb0, M::Metal);   // end gables
        g.tri(a1, bb1, b1, M::Metal);
    }
}

void block(Builder& g, const city::Building& b, const Massing& m, MeshDetail detail) {
    Rng rng{building_hash(b) ^ 0xb10c};
    const bool full = detail != MeshDetail::Massing;
    const bool resi = b.district == city::District::Residential;
    const float h = b.height;
    const Plan base{b.x, b.y, b.footprint * 0.5f};
    const Plan upper{b.x, b.y, m.top_footprint * 0.5f};
    const bool saw = has_sawtooth_roof(b);

    // Tiers: (plan, z0, z1).
    struct Tier {
        Plan p;
        float z0, z1;
    };
    std::vector<Tier> tiers{{base, 0.0f, m.stepped ? m.base_top : h}};
    if (m.stepped) tiers.push_back({upper, m.base_top, h});

    g.walls(base.points(), 0.0f, tiers[0].z1, M::FacadeShop);
    if (m.stepped) {
        g.cap(base.points(), m.base_top, true, M::Roof);
        if (full) g.parapet(base, 0.25f, m.base_top, 1.0f, M::Concrete);
        g.walls(upper.points(), m.base_top, h, M::Facade);
    }
    const Plan& roof = m.stepped ? upper : base;
    if (saw) {
        sawtooth_roof(g, roof, h);
        g.cap(roof.points(), h, true, M::Roof);
    } else {
        g.cap(roof.points(), h, true, M::Roof);
        if (full) g.parapet(roof, 0.25f, h, 1.0f, M::Concrete);
    }
    if (!full) return;
    shopfront_lights(g, b, base);

    // Heavy slab edge at every few floors reads as construction joints.
    const float fh = resi ? 3.1f : 3.8f;
    const std::uint32_t seed = shader_seed(b);
    const float pitch = 1.8f + 0.8f * shader_hash_f(seed ^ 0xa5u);  // building_surface.glsl window pitch

    const bool near = detail == MeshDetail::Near;
    const int relief_mode = static_cast<int>(city::hash64(building_hash(b) ^ 0x4e1f) % 3u);
    if (resi) {
        const int corridor_face = rng.chance(0.6f) ? rng.index(4) : -1;
        const int core_face = (corridor_face + 1 + rng.index(3)) & 3;
        const float balcony_density = rng.range(0.3f, 0.8f);
        for (int k = 0; k < 4; ++k) {
            const bool balconies = k != corridor_face && rng.chance(0.5f);
            for (const Tier& t : tiers) {
                const Face f = face_of(t.p, k);
                const float first = std::ceil(std::max(t.z0 + 1.0f, 4.6f) / fh) * fh;
                if (near && k != corridor_face)
                    facade_relief(g, b, f, std::max(t.z0, 4.8f), t.z1 - 0.8f, false, relief_mode);
                if (k == corridor_face) {
                    // Open-air access corridor on every floor: slab with lit soffit + solid balustrade.
                    for (float z = first; z + 1.2f < t.z1; z += fh) {
                        face_strip(g, f, -f.half_len + 0.3f, f.half_len - 0.3f, 0.0f, 1.5f, z - 0.25f, z + 0.05f,
                                   M::Concrete, M::Concrete, M::LitPanel);
                        face_strip(g, f, -f.half_len + 0.3f, f.half_len - 0.3f, 1.38f, 1.5f, z + 0.05f, z + 1.05f,
                                   M::Concrete, M::Concrete, M::Concrete);
                    }
                    // Stair tower at one end of the corridor.
                    const float a = (rng.chance(0.5f) ? 1.0f : -1.0f) * (f.half_len - 2.4f);
                    const Box3 st = f.bounds(a - 2.2f, a + 2.2f, 1.5f, 4.5f, 0.0f, t.z1 + 2.5f);
                    if (t.z0 < 0.5f && g.clear(st))
                        face_box(g, f, a - 2.2f, a + 2.2f, 1.5f, 4.5f, 0.0f, t.z1 + 2.5f, M::Concrete, M::Roof,
                                 M::Concrete);
                } else if (balconies) {
                    const float bay = pitch * 2.0f;
                    const int i0 = static_cast<int>(std::ceil((-f.half_len + 0.4f) / bay));
                    const int i1 = static_cast<int>(std::floor((f.half_len - 0.4f) / bay));
                    for (float z = first; z + 1.2f < t.z1; z += fh)
                        for (int i = i0; i < i1; ++i) {
                            if (!rng.chance(balcony_density)) continue;
                            const float a0 = static_cast<float>(i) * bay + 0.12f, a1 = a0 + bay - 0.24f;
                            if (!g.clear(f.bounds(a0, a1, 0.0f, 1.2f, z - 0.2f, z + 1.1f))) continue;
                            face_box(g, f, a0, a1, 0.0f, 1.2f, z - 0.2f, z, M::Concrete, M::Concrete, M::Concrete);
                            face_box(g, f, a0, a1, 1.12f, 1.2f, z, z + 1.0f, M::Metal, M::Metal, M::Metal);
                            face_box(g, f, a0, a0 + 0.06f, 0.0f, 1.12f, z, z + 1.0f, M::Metal, M::Metal, M::Metal);
                            face_box(g, f, a1 - 0.06f, a1, 0.0f, 1.12f, z, z + 1.0f, M::Metal, M::Metal, M::Metal);
                        }
                } else {
                    // Wall-hung AC units under windows, and a couple of drain pipes.
                    const int i0 = static_cast<int>(std::ceil(-f.half_len / pitch));
                    const int i1 = static_cast<int>(std::floor(f.half_len / pitch)) - 1;
                    for (float z = first; z + 1.0f < t.z1; z += fh)
                        for (int i = i0; i <= i1; ++i) {
                            const float a = (static_cast<float>(i) + 0.5f) * pitch;
                            if (rng.chance(0.32f)) {
                                const Box3 bb = f.bounds(a - 0.45f, a + 0.45f, 0.0f, 0.6f, z + 0.1f, z + 0.75f);
                                if (g.clear(bb))
                                    face_box(g, f, a - 0.42f, a + 0.42f, 0.0f, 0.55f, z + 0.12f, z + 0.72f, M::Concrete,
                                             M::Concrete, M::Metal);
                            }
                            // Window cages (security grilles) on the lower floors, Hong Kong style.
                            if (z < 26.0f && rng.chance(0.28f)) {
                                const float hw = pitch * (0.5f - 0.22f) + 0.05f;
                                const float c0 = z + 0.32f * fh - 0.05f, c1 = z + 0.8f * fh + 0.05f;
                                if (g.clear(f.bounds(a - hw, a + hw, 0.0f, 0.55f, c0, c1)))
                                    g.box(f.point(a, 0.25f), f.t, hw, 0.25f, c0, c1, M::Louvre, M::Metal, M::Metal, true);
                            }
                        }
                    for (int p = 0; p < 2; ++p) {
                        const float a = rng.range(-f.half_len + 0.5f, f.half_len - 0.5f);
                        const Box3 col = f.bounds(a - 0.1f, a + 0.1f, 0.0f, 0.25f, t.z0, t.z1);
                        for (auto [z0, z1] : g.free_spans(col.x0, col.y0, col.x1, col.y1, std::max(t.z0, 3.0f), t.z1, 1.5f))
                            face_box(g, f, a - 0.09f, a + 0.09f, 0.05f, 0.23f, z0, z1, M::Metal, M::Metal, M::Metal);
                    }
                }
                // Elevator / stair core rising above the roof.
                if (k == core_face && t.z0 < 0.5f && rng.chance(0.4f)) {
                    const float a = rng.range(-f.half_len + 3.0f, f.half_len - 3.0f);
                    const float top = h + 3.0f;
                    if (g.clear(f.bounds(a - 2.3f, a + 2.3f, 0.0f, 3.0f, 0.0f, top)))
                        face_box(g, f, a - 2.2f, a + 2.2f, -0.2f, 2.8f, 0.0f, top, M::Concrete, M::Roof, M::Concrete);
                }
            }
        }
    } else {
        // Warehouses: pipe runs, a loading canopy, maybe a chimney.
        for (int k = 0; k < 4; ++k) {
            const Face f = face_of(base, k);
            if (near)
                for (const Tier& t : tiers)
                    facade_relief(g, b, face_of(t.p, k), std::max(t.z0, 4.8f), t.z1 - 0.8f, false, relief_mode);
            if (rng.chance(0.4f)) {
                const int pipes = 1 + rng.index(3);
                const float z = rng.range(4.8f, std::max(5.0f, h - 3.0f));
                for (int p = 0; p < pipes; ++p) {
                    const float r = rng.range(0.18f, 0.35f), out = 0.45f + 0.7f * static_cast<float>(p);
                    const float zz = z + 0.2f * static_cast<float>(p);
                    const V2 a = f.point(-f.half_len + 0.5f, out), c = f.point(f.half_len - 0.5f, out);
                    const Box3 bb = f.bounds(-f.half_len, f.half_len, 0.0f, out + r, zz - r, zz + r);
                    if (g.clear(bb)) g.beam(at(a, zz), at(c, zz), r * 2.0f, M::Metal, 6);
                }
            } else if (rng.chance(0.4f)) {
                const float w = rng.range(4.0f, std::max(4.5f, f.half_len));
                const float a = rng.range(-f.half_len + w * 0.5f, f.half_len - w * 0.5f);
                face_strip(g, f, a - w * 0.5f, a + w * 0.5f, 0.0f, 3.5f, 5.0f, 5.35f, M::Metal, M::Metal, M::LitPanel);
                g.light(at(f.point(a, 1.8f), 4.5f), panel_tint(b), 1.6f * w * 1.75f);
            }
        }
        if (rng.chance(0.35f)) {
            const float sx = rng.chance(0.5f) ? 1.0f : -1.0f, sy = rng.chance(0.5f) ? 1.0f : -1.0f;
            const float r = rng.range(1.1f, 1.8f);
            const Plan chimney{b.x + sx * (base.half - r - 0.6f), b.y + sy * (base.half - r - 0.6f), r, 0.0f, 12};
            const float top = h + rng.range(12.0f, 32.0f);
            g.walls(chimney.points(), h, top, M::Concrete);
            g.cap(chimney.points(), top, true, M::Metal);
            g.solid(chimney.offset(0.08f).points(), top - 1.6f, top - 1.1f, M::LedRed);
            g.solid(chimney.offset(0.08f).points(), (h + top) * 0.5f, (h + top) * 0.5f + 0.4f, M::LedRed);
            g.light(at({chimney.cx, chimney.cy}, top - 1.3f), {1.0f, 0.05f, 0.06f}, 25.0f);
        }
    }
}

// ---- Shacks ---------------------------------------------------------------------
void shanty(Builder& g, const city::Building& b, MeshDetail detail) {
    Rng rng{building_hash(b) ^ 0x5a47};
    const Plan p{b.x, b.y, b.footprint * 0.5f};
    auto roof = [&b](V2 q) { return shanty_roof_z(b, q.x, q.y); };
    g.walls(p.points(), 0.0f, roof, M::ShantyWall);
    // Sheet roof with an overhang: top, underside, thin edge.
    const auto eave = p.offset(0.5f).points();
    g.cap(eave, [&](V2 q) { return roof(q) + 0.08f; }, true, M::Corrugated);
    g.cap(eave, [&](V2 q) { return roof(q) - 0.02f; }, false, M::Metal);
    g.walls(eave, [&](V2 q) { return roof(q) - 0.02f; }, [&](V2 q) { return roof(q) + 0.08f; }, M::Metal);
    if (detail == MeshDetail::Massing) return;

    for (int k = 0; k < 4; ++k) {
        const Face f = face_of(p, k);
        // Open stalls (building_surface.glsl shanty_wall) spill light onto the alley.
        const Rgb stall = mix({1.0f, 0.7f, 0.45f}, neon_color(shader_hash(face_seed(b, k) ^ 0x777u)), 0.5f);
        g.light(at(f.point(0.0f, 1.3f), 2.0f), stall, 3.0f + 0.4f * f.half_len);
        if (rng.chance(0.65f)) {
            // Awning over the stall: sloped sheet on two brackets.
            const float w = rng.range(0.4f, 0.95f) * 2.0f * f.half_len;
            const float a = rng.range(-f.half_len + w * 0.5f, f.half_len - w * 0.5f);
            const float out = rng.range(1.0f, 1.8f), zt = rng.range(2.9f, 3.3f), zb = zt - 0.45f;
            if (g.clear(f.bounds(a - w * 0.5f, a + w * 0.5f, 0.0f, out, zb - 0.3f, zt))) {
                const V2 p0 = f.point(a - w * 0.5f, 0.02f), p1 = f.point(a + w * 0.5f, 0.02f);
                const V2 q0 = f.point(a - w * 0.5f, out), q1 = f.point(a + w * 0.5f, out);
                // Top (seen from above) and underside.
                g.quad(at(q0, zb), at(q1, zb), at(p1, zt), at(p0, zt), {0, 0}, {w, 0}, {w, out}, {0, out}, M::Awning);
                g.quad(at(p0, zt - 0.03f), at(p1, zt - 0.03f), at(q1, zb - 0.03f), at(q0, zb - 0.03f), {0, out},
                       {w, out}, {w, 0}, {0, 0}, M::Awning);
                // Valance.
                g.wall_quad(at(q0, zb - 0.3f), at(q1, zb - 0.3f), at(q1, zb), at(q0, zb), a - w * 0.5f, a + w * 0.5f,
                            M::Awning);
                g.beam(at(f.point(a - w * 0.5f + 0.2f, 0.0f), zb - 0.6f), at(f.point(a - w * 0.5f + 0.2f, out - 0.1f), zb),
                       0.06f, M::Metal);
                g.beam(at(f.point(a + w * 0.5f - 0.2f, 0.0f), zb - 0.6f), at(f.point(a + w * 0.5f - 0.2f, out - 0.1f), zb),
                       0.06f, M::Metal);
            }
        }
    }
    // Cantilevered room on the upper floor.
    const float h = b.height;
    if (h > 7.0f && rng.chance(0.5f)) {
        const int k = rng.index(4);
        const Face f = face_of(p, k);
        const float w = rng.range(2.8f, std::min(5.0f, 2.0f * f.half_len - 0.5f));
        const float a = rng.range(-f.half_len + w * 0.5f, f.half_len - w * 0.5f);
        const float z0 = 3.7f, z1 = std::min(h - 0.6f, z0 + 2.7f), out = rng.range(1.0f, 1.7f);
        if (z1 - z0 > 2.0f && g.clear(f.bounds(a - w * 0.5f, a + w * 0.5f, 0.0f, out + 0.3f, z0 - 1.2f, z1 + 0.2f))) {
            face_box(g, f, a - w * 0.5f, a + w * 0.5f, 0.0f, out, z0, z1, M::ShantyWall, M::Corrugated, M::Metal);
            for (int s = -1; s <= 1; s += 2) {
                const float as = a + static_cast<float>(s) * (w * 0.5f - 0.2f);
                g.beam(at(f.point(as, 0.0f), z0 - 1.1f), at(f.point(as, out - 0.1f), z0), 0.1f, M::Metal);
            }
        }
    }
}

}  // namespace

// ---- Street clutter along the sidewalk --------------------------------------------
// Vending machines (each a small light source), bin-bag piles and utility boxes against
// the ground floor: the near layer of every street view.
void street_clutter(Builder& g, const city::Building& b, MeshDetail detail) {
    if (detail == MeshDetail::Massing) return;
    Rng rng{building_hash(b) ^ 0xc177u};
    const Plan p{b.x, b.y, b.footprint * 0.5f};
    for (int k = 0; k < 4; ++k) {
        const Face f = face_of(p, k);
        // Vending machines, often in a row of two or three.
        if (rng.chance(b.shanty ? 0.35f : 0.55f)) {
            const int n = 1 + rng.index(3);
            const float w = 0.95f;
            float a = rng.range(-f.half_len + 1.0f, f.half_len - 1.0f - w * static_cast<float>(n));
            for (int i = 0; i < n; ++i, a += w + 0.05f) {
                if (!g.clear(f.bounds(a, a + w, 0.0f, 0.8f, 0.0f, 1.95f))) continue;
                face_box(g, f, a, a + w, 0.0f, 0.78f, 0.0f, 1.9f, M::Metal, M::Metal, M::Metal);
                // Lit front, a hair in front of the box.
                const V2 p0 = f.point(a + 0.04f, 0.79f), p1 = f.point(a + w - 0.04f, 0.79f);
                g.wall_quad(at(p0, 0.05f), at(p1, 0.05f), at(p1, 1.85f), at(p0, 1.85f), a, a + w, M::Vending);
                g.light(at(f.point(a + w * 0.5f, 1.4f), 1.2f), {0.8f, 0.9f, 1.0f}, 5.0f);
            }
        }
        // Bin bags heaped against the wall.
        if (rng.chance(0.5f)) {
            const float a = rng.range(-f.half_len + 0.8f, f.half_len - 0.8f);
            const int bags = 3 + rng.index(5);
            for (int i = 0; i < bags; ++i) {
                const float ba = a + rng.range(-0.9f, 0.9f), bo = rng.range(0.25f, 1.0f);
                const float r = rng.range(0.22f, 0.34f), hz = r * rng.range(1.4f, 1.9f);
                const float z0 = i >= bags - 2 ? rng.range(0.2f, 0.4f) : 0.0f;  // a couple on top
                if (!g.clear(f.bounds(ba - r, ba + r, bo - r, bo + r, z0, z0 + hz))) continue;
                g.box(f.point(ba, bo), f.t * std::cos(rng.range(0.0f, 1.5f)) + f.n * std::sin(rng.range(0.0f, 1.5f)),
                      r, r * 0.85f, z0, z0 + hz, M::Plastic, M::Plastic, M::Plastic);
            }
        }
        // Utility / meter boxes and a conduit up the wall.
        if (rng.chance(0.45f)) {
            const float a = rng.range(-f.half_len + 0.6f, f.half_len - 0.6f);
            const float z = rng.range(1.0f, 1.6f);
            if (g.clear(f.bounds(a - 0.4f, a + 0.4f, 0.0f, 0.3f, z, std::min(b.height, 8.0f))))  {
                face_box(g, f, a - 0.35f, a + 0.35f, 0.0f, 0.25f, z, z + 0.8f, M::Metal, M::Metal, M::Metal);
                face_box(g, f, a - 0.05f, a + 0.05f, 0.02f, 0.12f, z + 0.8f, std::min(b.height, 8.0f), M::Metal, M::Metal,
                         M::Metal);
            }
        }
    }
}

void build_building_mesh(const city::Building& b, std::uint32_t building_index, std::span<const SignInstance> signs,
                         MeshDetail detail, CityMesh& out, std::vector<PointLight>& lights) {
    Builder g(out, building_index, signs, lights);
    const Massing m = massing_of(b);
    if (b.shanty) shanty(g, b, detail);
    else if (m.tower) tower(g, b, m, detail);
    else block(g, b, m, detail);
    street_clutter(g, b, detail);

    // Steel frames carrying the billboards that stand above the roof.
    for (const SignInstance& s : signs) {
        if (s.kind() != SignStyle::Screen || s.z - s.height * 0.5f < b.height + kBillboardLift - 0.1f) continue;
        const V2 n{std::cos(s.yaw), std::sin(s.yaw)}, t{-n.y, n.x};
        const V2 c{s.x, s.y};
        const float hw = s.width * 0.5f, z0 = b.height, zb = s.z - s.height * 0.5f, zt = s.z + s.height * 0.5f;
        // Dark backing box so the ad isn't seen mirrored from behind.
        g.box(c - n * 0.25f, t, hw + 0.15f, 0.2f, zb - 0.3f, zt + 0.3f, M::Metal, M::Metal, M::Metal);
        const float post[2] = {-hw + 0.4f, hw - 0.4f};
        for (float a : post) {
            const V2 p = c + t * a - n * 0.8f;
            g.beam(at(p, z0), at(p, zt), 0.3f, M::Metal);
            g.beam(at(p, z0), at(c + t * a - n * 0.3f, zb), 0.15f, M::Metal);  // strut to the frame
        }
        const V2 l = c + t * post[0] - n * 0.8f, r = c + t * post[1] - n * 0.8f;
        g.beam(at(l, z0 + 0.3f), at(r, zb - 0.3f), 0.15f, M::Metal);
        g.beam(at(r, z0 + 0.3f), at(l, zb - 0.3f), 0.15f, M::Metal);
        g.beam(at(l, zb - 0.3f), at(r, zb - 0.3f), 0.2f, M::Metal);
        // Service catwalk with a lamp row under the ad.
        g.box(c + n * 0.5f, t, hw, 0.5f, zb - 0.45f, zb - 0.35f, M::Metal, M::Metal, M::Metal);
        g.box(c + n * 0.9f, t, hw, 0.05f, zb - 0.35f, zb + 0.05f, M::LedRed, M::LedRed, M::LedRed);
    }
}

void build_cables(std::span<const CableAnchor> anchors, CityMesh& out) {
    // Bucket anchors on a 30 m grid so each building only looks at its neighbours.
    constexpr float kCell = 30.0f, kMaxSpan = 48.0f;
    auto key = [](int x, int y) { return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y); };
    std::vector<std::pair<std::uint64_t, std::uint32_t>> grid;
    for (std::uint32_t i = 0; i < anchors.size(); ++i)
        grid.push_back({key(static_cast<int>(std::floor(anchors[i].x / kCell)), static_cast<int>(std::floor(anchors[i].y / kCell))), i});
    std::sort(grid.begin(), grid.end());
    std::vector<PointLight> no_lights;
    for (std::uint32_t i = 0; i < anchors.size(); ++i) {
        const CableAnchor& a = anchors[i];
        const int cx = static_cast<int>(std::floor(a.x / kCell)), cy = static_cast<int>(std::floor(a.y / kCell));
        Builder g(out, a.building_index, {}, no_lights);
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                auto it = std::lower_bound(grid.begin(), grid.end(), std::pair{key(cx + dx, cy + dy), 0u});
                for (; it != grid.end() && it->first == key(cx + dx, cy + dy); ++it) {
                    const std::uint32_t j = it->second;
                    if (j <= i) continue;  // each pair once
                    const CableAnchor& b = anchors[j];
                    const V2 d{b.x - a.x, b.y - a.y};
                    const float dist = len(d);
                    if (dist < 8.0f || dist > kMaxSpan) continue;
                    Rng rng{city::hash64(city::hash64(static_cast<std::uint64_t>(std::lround(a.x * 7 + a.y * 13))) ^
                                         static_cast<std::uint64_t>(std::lround(b.x * 11 + b.y * 5)))};
                    if (!rng.chance(0.45f)) continue;
                    const V2 dir = d * (1.0f / dist), side{-dir.y, dir.x};
                    // Leave from the facing walls, below the lower roof.
                    const float top = std::min({a.height, b.height, 16.0f});
                    const int wires = 1 + rng.index(4);
                    const float z0 = rng.range(std::max(3.5f, top - 4.0f), top - 0.3f);
                    const float off = rng.range(-0.3f, 0.3f) * std::min(a.footprint, b.footprint);
                    for (int w = 0; w < wires; ++w) {
                        const float lat = off + static_cast<float>(w) * rng.range(0.2f, 0.6f);
                        const V2 pa = V2{a.x, a.y} + dir * (a.footprint * 0.5f) + side * lat;
                        const V2 pb = V2{b.x, b.y} - dir * (b.footprint * 0.5f) + side * (lat + rng.range(-1.0f, 1.0f));
                        const float za = z0 + rng.range(-0.4f, 0.4f), zb = z0 + rng.range(-0.8f, 0.8f);
                        const float sag = len(pb - pa) * rng.range(0.03f, 0.08f);
                        constexpr int kSegs = 8;
                        Vec3 prev = at(pa, za);
                        for (int k = 1; k <= kSegs; ++k) {
                            const float t = static_cast<float>(k) / kSegs;
                            const V2 p = pa + (pb - pa) * t;
                            const Vec3 cur = at(p, za + (zb - za) * t - 4.0f * sag * t * (1.0f - t));
                            g.beam(prev, cur, 0.05f, M::Metal, 3);
                            prev = cur;
                        }
                    }
                }
            }
    }
}

void build_skybridges(std::span<const TowerAnchor> towers, CityMesh& out, std::vector<PointLight>& lights) {
    for (std::size_t i = 0; i < towers.size(); ++i)
        for (std::size_t j = i + 1; j < towers.size(); ++j) {
            const TowerAnchor& a = towers[i];
            const TowerAnchor& b = towers[j];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const bool along_x = std::fabs(dx) > std::fabs(dy);
            const float lateral = along_x ? std::fabs(dy) : std::fabs(dx);
            const float dist = along_x ? std::fabs(dx) : std::fabs(dy);
            const float gap = dist - a.half - b.half;
            if (lateral > std::min(a.half, b.half) - 4.0f || gap < 6.0f || gap > 45.0f) continue;
            const float lo = std::max(a.base_top, b.base_top) + 15.0f, hi = std::min(a.shaft_top, b.shaft_top) - 12.0f;
            if (hi - lo < 6.0f) continue;
            Rng rng{city::hash64(static_cast<std::uint64_t>(std::lround(a.x * 3 + b.x * 7 + a.y * 11 + b.y * 13)))};
            if (!rng.chance(0.55f)) continue;
            Builder g(out, a.building_index, {}, lights);
            const int decks = 1 + (rng.chance(0.3f) ? 1 : 0);
            for (int d = 0; d < decks; ++d) {
                const float z0 = std::floor(rng.range(lo, hi - 5.0f) / 3.8f) * 3.8f;
                const float w = rng.range(4.0f, 7.0f), hgt = rng.range(4.0f, 7.6f);
                const float off = rng.range(-1.0f, 1.0f) * (std::min(a.half, b.half) - w - 2.0f - lateral);
                const V2 ax = along_x ? V2{dx > 0 ? 1.0f : -1.0f, 0.0f} : V2{0.0f, dy > 0 ? 1.0f : -1.0f};
                const V2 side{-ax.y, ax.x};
                const V2 start = V2{a.x, a.y} + ax * a.half, end = V2{b.x, b.y} - ax * b.half;
                const V2 mid = (start + end) * 0.5f + side * (off + (along_x ? (b.y - a.y) : (b.x - a.x)) * 0.5f * 0.0f);
                g.box(mid + side * 0.0f, ax, gap * 0.5f + 0.3f, w * 0.5f, z0, z0 + hgt,
                      a.corporate ? M::Glass : M::Facade, M::Roof, M::LitPanel);
                // Lit rails along both sides of the deck.
                for (float sd : {-1.0f, 1.0f})
                    g.box(mid + side * (sd * (w * 0.5f + 0.05f)), ax, gap * 0.5f, 0.06f, z0 - 0.1f, z0 + 0.15f, M::Led,
                          M::Led, M::Led);
                g.light(at(mid, z0 - 1.5f), {0.7f, 0.85f, 1.0f}, 30.0f);
            }
        }
}

}  // namespace apex
