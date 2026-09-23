#include "apex/buildgen.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

#include "apex/mat.hpp"
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

using M = SurfaceMaterial;

Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
constexpr float kPi = std::numbers::pi_v<float>;

struct V2 {
    float x = 0, y = 0;
};
V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
V2 operator*(V2 a, float s) { return {a.x * s, a.y * s}; }
float len(V2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
Vec3 at(V2 p, float z) { return {p.x, p.y, z}; }

// Convex plan centred at (cx, cy), counter-clockwise from above: a square with
// 45-degree corner cuts, or a regular polygon. `half` is the apothem, so offsetting keeps
// every face parallel.
struct Plan {
    float cx = 0, cy = 0, half = 0;
    float cut = 0;  // corner cut as a fraction of half (0 square, 0.586 regular octagon)
    int sides = 0;  // > 0: regular polygon

    Plan offset(float d) const {
        Plan p = *this;
        p.half = half + d;
        if (sides == 0 && cut > 0.0f) p.cut = (cut * half + d * (2.0f - std::numbers::sqrt2_v<float>)) / p.half;
        return p;
    }
    Plan with_half(float h) const {
        Plan p = *this;
        p.half = h;
        return p;
    }
    std::vector<V2> points() const {
        std::vector<V2> pts;
        if (sides > 0) {
            const float r = half / std::cos(kPi / static_cast<float>(sides));
            for (int i = 0; i < sides; ++i) {
                const float a = kPi / static_cast<float>(sides) * static_cast<float>(2 * i - 1);
                pts.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
            }
            return pts;
        }
        const float h = half, c = cut * half;
        if (c < 1e-3f) return {{cx + h, cy - h}, {cx + h, cy + h}, {cx - h, cy + h}, {cx - h, cy - h}};
        const V2 local[8] = {{h, -h + c}, {h, h - c}, {h - c, h}, {-h + c, h},
                             {-h, h - c}, {-h, -h + c}, {-h + c, -h}, {h - c, -h}};
        for (V2 p : local) pts.push_back({cx + p.x, cy + p.y});
        return pts;
    }
    // Faces whose outward normal is axis-aligned: +X, +Y, -X, -Y as face index 0..3.
    // Returns the index into points() of the face's first vertex.
    int axis_face_start(int axis_face) const { return cut * half < 1e-3f ? axis_face : 2 * axis_face; }
    float flat_width() const { return 2.0f * half * (1.0f - cut); }
};

struct Box3 {
    float x0, y0, z0, x1, y1, z1;
};
bool overlaps(const Box3& a, const Box3& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1 && a.z0 < b.z1 && b.z0 < a.z1;
}

// Deterministic stream of uniforms from the building hash.
struct Rng {
    std::uint64_t h;
    float next() {
        h = city::hash64(h);
        return unit(h);
    }
    float range(float a, float b) { return a + (b - a) * next(); }
    int index(int n) { return std::min(n - 1, static_cast<int>(next() * static_cast<float>(n))); }
    bool chance(float p) { return next() < p; }
};

class Builder {
public:
    Builder(CityMesh& m, std::uint32_t building, std::span<const SignInstance> signs, std::vector<PointLight>& lights)
        : m_(m), id_(building), lights_(lights) {
        for (const SignInstance& s : signs) {
            const float hw = s.width * 0.5f, t = 0.3f;
            const float c = std::fabs(std::cos(s.yaw)), sn = std::fabs(std::sin(s.yaw));
            const float ex = sn * hw + c * t, ey = c * hw + sn * t;
            signs_.push_back({s.x - ex, s.y - ey, s.z - s.height * 0.5f - 0.1f, s.x + ex, s.y + ey,
                              s.z + s.height * 0.5f + 0.1f});
        }
    }

    bool clear(const Box3& b) const {
        return std::none_of(signs_.begin(), signs_.end(), [&](const Box3& s) { return overlaps(s, b); });
    }
    const std::vector<Box3>& signs() const { return signs_; }

    // Light of `power` (roughly emitted flux in the shader's units) in colour c.
    void light(Vec3 p, Rgb c, float power) {
        const float radius = std::clamp(std::sqrt(power) * 4.5f, 4.0f, 36.0f);
        lights_.push_back({p.x, p.y, p.z, radius, c.r * power, c.g * power, c.b * power, 0.0f});
    }

    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, V2 ta, V2 tb, V2 tc, V2 td, M mat) {
        // Diagonal cross product: robust for trapezoids and quads with a collapsed edge.
        const Vec3 n = normalize(cross(c - a, d - b));
        const std::uint32_t i0 = vertex(a, n, ta, mat), i1 = vertex(b, n, tb, mat);
        const std::uint32_t i2 = vertex(c, n, tc, mat), i3 = vertex(d, n, td, mat);
        m_.indices.insert(m_.indices.end(), {i0, i1, i2, i0, i2, i3});
    }
    // Wall quad a-b bottom, c-d top (counter-clockwise from outside); v = z.
    void wall_quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, float u0, float u1, M mat) {
        quad(a, b, c, d, {u0, a.z}, {u1, b.z}, {u1, c.z}, {u0, d.z}, mat);
    }
    void tri(Vec3 a, Vec3 b, Vec3 c, M mat) {
        const Vec3 n = normalize(cross(b - a, c - a));
        const std::uint32_t i0 = vertex(a, n, {a.x, a.y}, mat), i1 = vertex(b, n, {b.x, b.y}, mat);
        const std::uint32_t i2 = vertex(c, n, {c.x, c.y}, mat);
        m_.indices.insert(m_.indices.end(), {i0, i1, i2});
    }
    // Quad wound either way; flipped so its normal points away from `inside`.
    void quad_out(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 inside, M mat, float u0 = 0, float u1 = 1) {
        const Vec3 n = cross(c - a, d - b);
        const Vec3 mid = (a + b + c + d) * 0.25f;
        if (dot(n, mid - inside) < 0.0f) {
            std::swap(a, b);
            std::swap(c, d);
        }
        wall_quad(a, b, c, d, u0, u1, mat);
    }

    // Vertical walls around a plan from bottom(x, y) to top(x, y).
    void walls(const std::vector<V2>& poly, const std::function<float(V2)>& bottom,
               const std::function<float(V2)>& top, M mat) {
        const std::size_t n = poly.size();
        for (std::size_t i = 0; i < n; ++i) {
            const V2 a = poly[i], b = poly[(i + 1) % n];
            const float l = len(b - a);
            const float u0 = static_cast<float>(i) * kFaceStride - l * 0.5f;
            wall_quad(at(a, bottom(a)), at(b, bottom(b)), at(b, top(b)), at(a, top(a)), u0, u0 + l, mat);
        }
    }
    void walls(const std::vector<V2>& poly, float z0, const std::function<float(V2)>& top, M mat) {
        walls(poly, [z0](V2) { return z0; }, top, mat);
    }
    void walls(const std::vector<V2>& poly, float z0, float z1, M mat) {
        walls(poly, z0, [z1](V2) { return z1; }, mat);
    }
    // Sloped walls between two plans with the same vertex count (a frustum).
    void walls(const std::vector<V2>& lo, float z0, const std::vector<V2>& hi, float z1, M mat) {
        const std::size_t n = lo.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const float l0 = len(lo[j] - lo[i]), l1 = len(hi[j] - hi[i]);
            const float u = static_cast<float>(i) * kFaceStride;
            quad(at(lo[i], z0), at(lo[j], z0), at(hi[j], z1), at(hi[i], z1), {u - l0 * 0.5f, z0}, {u + l0 * 0.5f, z0},
                 {u + l1 * 0.5f, z1}, {u - l1 * 0.5f, z1}, mat);
        }
    }
    // Convex cap (fan from the centroid); z may vary linearly (sloped roofs).
    void cap(const std::vector<V2>& poly, const std::function<float(V2)>& z, bool up, M mat) {
        V2 c{};
        for (V2 p : poly) c = c + p;
        c = c * (1.0f / static_cast<float>(poly.size()));
        const std::size_t n = poly.size();
        for (std::size_t i = 0; i < n; ++i) {
            V2 a = poly[i], b = poly[(i + 1) % n];
            if (!up) std::swap(a, b);
            tri(at(c, z(c)), at(a, z(a)), at(b, z(b)), mat);
        }
    }
    void cap(const std::vector<V2>& poly, float z, bool up, M mat) {
        cap(poly, [z](V2) { return z; }, up, mat);
    }
    // Horizontal ring between an outer and an inner plan (parapet tops, ledges seen from above).
    void ring(const std::vector<V2>& outer, const std::vector<V2>& inner, float z, M mat) {
        const std::size_t n = outer.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            quad(at(outer[i], z), at(outer[j], z), at(inner[j], z), at(inner[i], z), {outer[i].x, outer[i].y},
                 {outer[j].x, outer[j].y}, {inner[j].x, inner[j].y}, {inner[i].x, inner[i].y}, mat);
        }
    }
    void solid(const std::vector<V2>& poly, float z0, float z1, M side, M top, M bottom, bool with_bottom = true) {
        walls(poly, z0, z1, side);
        cap(poly, z1, true, top);
        if (with_bottom) cap(poly, z0, false, bottom);
    }
    void solid(const std::vector<V2>& poly, float z0, float z1, M mat) { solid(poly, z0, z1, mat, mat, mat); }
    // Low wall around the edge of a roof.
    void parapet(const Plan& p, float thickness, float z, float height, M mat) {
        const auto outer = p.points();
        auto inner = p.offset(-thickness).points();
        walls(outer, z, z + height, mat);
        std::vector<V2> rev(inner.rbegin(), inner.rend());
        walls(rev, z, z + height, mat);
        ring(outer, inner, z + height, mat);
    }
    // Box with its local x axis along `ax` (unit), centred at c.
    std::vector<V2> rect(V2 c, V2 ax, float hx, float hy) const {
        const V2 ay{-ax.y, ax.x};
        return {c + ax * hx - ay * hy, c + ax * hx + ay * hy, c - ax * hx + ay * hy, c - ax * hx - ay * hy};
    }
    void box(V2 c, V2 ax, float hx, float hy, float z0, float z1, M side, M top, M bottom) {
        solid(rect(c, ax, hx, hy), z0, z1, side, top, bottom);
    }
    // Straight member between two points with a square (sides = 4) or round cross-section.
    void beam(Vec3 a, Vec3 b, float w, M mat, int sides = 4) {
        const Vec3 d = normalize(b - a);
        const Vec3 ref = std::fabs(d.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        const Vec3 s = normalize(cross(d, ref)), t = cross(s, d);
        const float r = w * 0.5f / std::cos(kPi / static_cast<float>(sides));
        std::vector<Vec3> ra, rb;
        for (int i = 0; i < sides; ++i) {
            const float ang = kPi / static_cast<float>(sides) * static_cast<float>(2 * i + 1);
            const Vec3 o = s * (r * std::cos(ang)) + t * (r * std::sin(ang));
            ra.push_back(a + o);
            rb.push_back(b + o);
        }
        for (int i = 0; i < sides; ++i) {
            const int j = (i + 1) % sides;
            const Vec3 mid = (a + b) * 0.5f;
            quad_out(ra[static_cast<std::size_t>(i)], ra[static_cast<std::size_t>(j)], rb[static_cast<std::size_t>(j)],
                     rb[static_cast<std::size_t>(i)], mid, mat);
        }
        if (sides == 4) {  // end caps only matter for the chunky members
            quad_out(ra[0], ra[1], ra[2], ra[3], b, mat);
            quad_out(rb[0], rb[1], rb[2], rb[3], a, mat);
        }
    }

    // Remove the z ranges covered by signs from [z0, z1] over the footprint [x0,x1]x[y0,y1].
    std::vector<std::pair<float, float>> free_spans(float x0, float y0, float x1, float y1, float z0, float z1,
                                                    float min_len) const {
        std::vector<std::pair<float, float>> cuts;
        const Box3 column{x0, y0, z0, x1, y1, z1};
        for (const Box3& s : signs_)
            if (overlaps(s, column)) cuts.push_back({s.z0, s.z1});
        std::sort(cuts.begin(), cuts.end());
        std::vector<std::pair<float, float>> out;
        float z = z0;
        for (auto [c0, c1] : cuts) {
            if (c0 > z && c0 - z >= min_len) out.push_back({z, std::min(c0, z1)});
            z = std::max(z, c1);
        }
        if (z1 - z >= min_len) out.push_back({z, z1});
        return out;
    }

private:
    CityMesh& m_;
    std::uint32_t id_;
    std::vector<PointLight>& lights_;
    std::vector<Box3> signs_;

    std::uint32_t vertex(Vec3 p, Vec3 n, V2 uv, M mat) {
        auto q = [](float v) { return static_cast<std::int8_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)); };
        m_.vertices.push_back({p.x, p.y, p.z, q(n.x), q(n.y), q(n.z), 0, uv.x, uv.y,
                               id_ | (static_cast<std::uint32_t>(mat) << 24)});
        return static_cast<std::uint32_t>(m_.vertices.size() - 1);
    }
};

// A face of an axis-aligned square plan: outward normal and the along-wall tangent
// (counter-clockwise), so attachments can be placed in (along, out, z) coordinates.
struct Face {
    V2 centre, n, t;
    float half_len;
    V2 point(float along, float out) const { return centre + t * along + n * out; }
    Box3 bounds(float a0, float a1, float o0, float o1, float z0, float z1) const {
        const V2 p0 = point(a0, o0), p1 = point(a1, o1);
        return {std::min(p0.x, p1.x), std::min(p0.y, p1.y), z0, std::max(p0.x, p1.x), std::max(p0.y, p1.y), z1};
    }
};
Face face_of(const Plan& p, int k) {
    const float yaw = static_cast<float>(k) * kPi * 0.5f;
    const V2 n{std::round(std::cos(yaw)), std::round(std::sin(yaw))};
    return {V2{p.cx, p.cy} + n * p.half, n, {-n.y, n.x}, p.flat_width() * 0.5f};
}

// Box attached to a face, spanning [a0, a1] along it and [o0, o1] out from it.
void face_box(Builder& g, const Face& f, float a0, float a1, float o0, float o1, float z0, float z1, M side, M top,
              M bottom) {
    const V2 c = f.point((a0 + a1) * 0.5f, (o0 + o1) * 0.5f);
    g.box(c, f.t, (a1 - a0) * 0.5f, (o1 - o0) * 0.5f, z0, z1, side, top, bottom);
}

// A run along a face, split around any sign in its way.
void face_strip(Builder& g, const Face& f, float a0, float a1, float o0, float o1, float z0, float z1, M side, M top,
                M bottom, float min_len = 0.8f) {
    std::vector<std::pair<float, float>> cuts;
    const Box3 whole = f.bounds(a0, a1, o0, o1, z0, z1);
    for (const Box3& s : g.signs()) {
        if (!overlaps(s, whole)) continue;
        // Project the sign's box onto the along axis.
        const float sa0 = (s.x0 - f.centre.x) * f.t.x + (s.y0 - f.centre.y) * f.t.y;
        const float sa1 = (s.x1 - f.centre.x) * f.t.x + (s.y1 - f.centre.y) * f.t.y;
        cuts.push_back({std::min(sa0, sa1) - 0.1f, std::max(sa0, sa1) + 0.1f});
    }
    std::sort(cuts.begin(), cuts.end());
    float a = a0;
    auto emit = [&](float s0, float s1) {
        if (s1 - s0 >= min_len) face_box(g, f, s0, s1, o0, o1, z0, z1, side, top, bottom);
    };
    for (auto [c0, c1] : cuts) {
        if (c0 > a) emit(a, std::min(c0, a1));
        a = std::max(a, c1);
    }
    if (a < a1) emit(a, a1);
}

std::uint32_t shader_seed(const city::Building& b) { return static_cast<std::uint32_t>(building_hash(b) >> 32); }

// Face seed as building_surface.glsl derives it for mesh face k.
std::uint32_t face_seed(const city::Building& b, int k) {
    return shader_hash(shader_seed(b) ^ static_cast<std::uint32_t>(k + 3));
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
    const bool full = detail == MeshDetail::Full;
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

    // Shaft.
    g.walls(shaft.points(), m.base_top, m.shaft_top, skin);
    if (full) {
        // Floor ledges every few floors.
        const int every = 2 + rng.index(4);
        const float step = kFloor * static_cast<float>(every);
        const M ledge = rng.chance(0.5f) ? M::Concrete : M::Metal;
        for (float z = std::ceil((m.base_top + 2.0f) / step) * step; z < m.shaft_top - 3.0f; z += step)
            g.solid(shaft.offset(0.35f).points(), z + 0.05f, z + 0.5f, ledge);
        // Mechanical floors: louvred bands every ~90 m.
        for (float z = m.base_top + 60.0f + rng.range(0.0f, 30.0f); z < m.shaft_top - 20.0f; z += rng.range(80.0f, 110.0f)) {
            const float zf = std::floor(z / kFloor) * kFloor;
            g.solid(shaft.offset(0.15f).points(), zf, zf + kFloor * 2.0f, M::Louvre, M::Metal, M::Metal);
        }

        const float style = rng.next();
        const float half = shaft.half;
        if (style < 0.45f) {
            // Vertical fins on the flat faces.
            const float spacing = rng.range(2.6f, 4.5f), depth = rng.range(0.4f, 0.8f);
            for (int k = 0; k < 4; ++k) {
                const Face f = face_of(shaft, k);
                const int count = static_cast<int>((2.0f * f.half_len - 1.0f) / spacing);
                for (int i = 0; i <= count; ++i) {
                    const float a = -0.5f * spacing * static_cast<float>(count) + spacing * static_cast<float>(i);
                    const Box3 col = f.bounds(a - 0.15f, a + 0.15f, 0.0f, depth, m.base_top, m.shaft_top);
                    for (auto [z0, z1] : g.free_spans(col.x0, col.y0, col.x1, col.y1, m.base_top, m.shaft_top, 2.0f))
                        face_box(g, f, a - 0.15f, a + 0.15f, 0.0f, depth, z0, z1, M::Metal, M::Metal, M::Metal);
                }
            }
        } else if (style < 0.75f) {
            if (m.shaft_cut < 0.01f) {
                // Square shaft: heavy corner piers.
                for (int k = 0; k < 4; ++k) {
                    const float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? 1.0f : -1.0f;
                    const V2 c{b.x + sx * (half - 0.4f), b.y + sy * (half - 0.4f)};
                    g.box(c, {1, 0}, 1.2f, 1.2f, m.base_top, m.shaft_top + 1.5f, M::Concrete, M::Concrete, M::Concrete);
                }
            } else {
                // Chamfered shaft: LED lines down the diagonal faces, upper part only.
                const auto pts = shaft.points();
                for (std::size_t k = 1; k < pts.size(); k += 2) {
                    const V2 a = pts[k], c = pts[(k + 1) % pts.size()];
                    const V2 mid = (a + c) * 0.5f;
                    const V2 along = (c - a) * (1.0f / len(c - a));
                    const V2 out{along.y, -along.x};
                    const float z0 = m.base_top + (m.shaft_top - m.base_top) * 0.45f;
                    g.box(mid + out * 0.1f, along, 0.25f, 0.12f, z0, m.shaft_top, M::Led, M::Led, M::Led);
                }
            }
        }
        // Vertical LED spine on some corporate towers.
        if (corp && rng.chance(0.3f))
            for (int k = 0; k < 4; k += 2) {
                const Face f = face_of(shaft, (k + rng.index(2)) & 3);
                const float z0 = m.base_top + 6.0f;
                const Box3 col = f.bounds(-0.4f, 0.4f, 0.0f, 0.3f, z0, m.shaft_top);
                for (auto [s0, s1] : g.free_spans(col.x0, col.y0, col.x1, col.y1, z0, m.shaft_top, 3.0f))
                    face_box(g, f, -0.4f, 0.4f, 0.0f, 0.25f, s0, s1, M::Led, M::Led, M::Led);
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
    const bool full = detail == MeshDetail::Full;
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

    if (resi) {
        const int corridor_face = rng.chance(0.6f) ? rng.index(4) : -1;
        const int core_face = (corridor_face + 1 + rng.index(3)) & 3;
        const float balcony_density = rng.range(0.3f, 0.8f);
        for (int k = 0; k < 4; ++k) {
            const bool balconies = k != corridor_face && rng.chance(0.5f);
            for (const Tier& t : tiers) {
                const Face f = face_of(t.p, k);
                const float first = std::ceil(std::max(t.z0 + 1.0f, 4.6f) / fh) * fh;
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
                            if (!rng.chance(0.18f)) continue;
                            const float a = (static_cast<float>(i) + 0.5f) * pitch;
                            const Box3 bb = f.bounds(a - 0.45f, a + 0.45f, 0.0f, 0.6f, z + 0.1f, z + 0.75f);
                            if (g.clear(bb))
                                face_box(g, f, a - 0.42f, a + 0.42f, 0.0f, 0.55f, z + 0.12f, z + 0.72f, M::Concrete,
                                         M::Concrete, M::Metal);
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
    if (detail != MeshDetail::Full) return;

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

void build_building_mesh(const city::Building& b, std::uint32_t building_index, std::span<const SignInstance> signs,
                         MeshDetail detail, CityMesh& out, std::vector<PointLight>& lights) {
    Builder g(out, building_index, signs, lights);
    const Massing m = massing_of(b);
    if (b.shanty) shanty(g, b, detail);
    else if (m.tower) tower(g, b, m, detail);
    else block(g, b, m, detail);

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

}  // namespace apex
