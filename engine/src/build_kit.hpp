// Shared geometry kit for procedural building meshes (buildgen.cpp) and the
// hand-authored spawn set (hero.cpp): plans, the triangle / box-instance builder, faces.
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <span>
#include <vector>

#include "apex/city_mesh.hpp"
#include "apex/mat.hpp"
#include "apex/massing.hpp"
#include "apex/world.hpp"

namespace apex::kit {

using M = SurfaceMaterial;

inline Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
constexpr float kPi = std::numbers::pi_v<float>;

struct V2 {
    float x = 0, y = 0;
};
inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
inline V2 operator*(V2 a, float s) { return {a.x * s, a.y * s}; }
inline float len(V2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline Vec3 at(V2 p, float z) { return {p.x, p.y, z}; }

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
        // Keep chamfered plans chamfered (same vertex count) when offsetting inwards.
        if (sides == 0 && cut > 0.0f)
            p.cut = std::max((cut * half + d * (2.0f - std::numbers::sqrt2_v<float>)) / p.half, 0.02f);
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
inline bool overlaps(const Box3& a, const Box3& b) {
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
        const float size = std::clamp(std::sqrt(power) * 0.06f, 0.15f, 1.0f);  // soft-shadow source radius
        lights_.push_back({p.x, p.y, p.z, radius, c.r * power, c.g * power, c.b * power, size});
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
        if (outer.size() != inner.size()) return;  // plans must match vertex for vertex
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
    // Boxes are instanced (BoxInstance), not triangulated.
    void box(V2 c, V2 ax, float hx, float hy, float z0, float z1, M side, M top, M bottom, bool open_back = false) {
        if (hx <= 1e-3f || hy <= 1e-3f || z1 - z0 <= 1e-3f) return;
        m_.boxes.push_back({c.x, c.y, z0, std::atan2(ax.y, ax.x), hx, hy, z1 - z0, 0.0f, id_,
                            static_cast<std::uint32_t>(side) | (static_cast<std::uint32_t>(top) << 8) |
                                (static_cast<std::uint32_t>(bottom) << 16),
                            open_back ? BoxInstance::kOpenBack : 0u, 0u});
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
inline Face face_of(const Plan& p, int k) {
    const float yaw = static_cast<float>(k) * kPi * 0.5f;
    const V2 n{std::round(std::cos(yaw)), std::round(std::sin(yaw))};
    return {V2{p.cx, p.cy} + n * p.half, n, {-n.y, n.x}, p.flat_width() * 0.5f};
}

// Box attached to a face, spanning [a0, a1] along it and [o0, o1] out from it.
inline void face_box(Builder& g, const Face& f, float a0, float a1, float o0, float o1, float z0, float z1, M side, M top,
              M bottom) {
    const V2 c = f.point((a0 + a1) * 0.5f, (o0 + o1) * 0.5f);
    g.box(c, f.t, (a1 - a0) * 0.5f, (o1 - o0) * 0.5f, z0, z1, side, top, bottom);
}

// A run along a face, split around any sign in its way.
inline void face_strip(Builder& g, const Face& f, float a0, float a1, float o0, float o1, float z0, float z1, M side, M top,
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

}  // namespace apex::kit
