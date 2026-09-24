// Close-range building equipment modelled as real geometry: windows with frames and
// panes, wall AC units, rooftop tanks, dishes, vents, pipes, exterior stairs and lantern
// strings. Used by the Close level of detail (buildgen.cpp) and the spawn set (hero.cpp).
#pragma once

#include "build_kit.hpp"

namespace apex::kit {

// Upright cylinder (optionally capped) around c from z0 to z0 + h.
inline void cylinder(Builder& g, V2 c, float z0, float r, float h, int sides, M side, M top, bool cap = true) {
    std::vector<V2> ring;
    for (int i = 0; i < sides; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
        ring.push_back(c + V2{std::cos(a), std::sin(a)} * r);
    }
    g.walls(ring, z0, z0 + h, side);
    if (cap) g.cap(ring, z0 + h, true, top);
}

// Shallow cone roof (tank lids, lantern caps).
inline void cone(Builder& g, V2 c, float z0, float r, float h, int sides, M mat) {
    const Vec3 apex = at(c, z0 + h);
    for (int i = 0; i < sides; ++i) {
        const float a0 = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
        const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / static_cast<float>(sides);
        g.tri(at(c + V2{std::cos(a0), std::sin(a0)} * r, z0), at(c + V2{std::cos(a1), std::sin(a1)} * r, z0), apex, mat);
    }
}

// Rooftop water tank on a steel stand, with a ladder and an outlet pipe.
inline void water_tank(Builder& g, V2 c, float z, float r, float h, Rng& rng) {
    const float stand = 0.7f;
    for (int k = 0; k < 4; ++k) {
        const float a = kPi * 0.25f + kPi * 0.5f * static_cast<float>(k);
        const V2 leg = c + V2{std::cos(a), std::sin(a)} * (r * 0.8f);
        g.beam(at(leg, z), at(leg, z + stand), 0.08f, M::Metal);
    }
    g.box(c, {1.0f, 0.0f}, r * 0.85f, r * 0.85f, z + stand - 0.08f, z + stand, M::Metal, M::Metal, M::Metal);
    cylinder(g, c, z + stand, r, h, 14, M::Tank, M::Tank, false);
    cone(g, c, z + stand + h, r * 1.02f, 0.22f * r, 14, M::Tank);
    // Hoops.
    for (float t : {0.3f, 0.7f}) cylinder(g, c, z + stand + h * t, r + 0.02f, 0.05f, 14, M::Metal, M::Metal, false);
    const float a = rng.range(0.0f, 2.0f * kPi);
    const V2 d{std::cos(a), std::sin(a)}, s{-d.y, d.x};
    for (float o : {-0.18f, 0.18f})
        g.beam(at(c + d * (r + 0.08f) + s * o, z), at(c + d * (r + 0.08f) + s * o, z + stand + h + 0.1f), 0.03f, M::Metal);
    for (float zz = z + 0.3f; zz < z + stand + h; zz += 0.3f)
        g.beam(at(c + d * (r + 0.08f) - s * 0.18f, zz), at(c + d * (r + 0.08f) + s * 0.18f, zz), 0.02f, M::Metal);
    g.beam(at(c - d * r * 0.5f, z + stand), at(c - d * (r + 0.6f), z + 0.05f), 0.06f, M::Metal, 6);
}

// Wall-mounted split AC unit on brackets; `n` is the wall's outward normal.
inline void wall_ac(Builder& g, V2 base, V2 n, float z, float w = 0.8f) {
    const V2 t{-n.y, n.x};
    const float d = 0.32f, h = 0.55f;
    g.box(base + n * (d * 0.5f + 0.05f), t, w * 0.5f, d * 0.5f, z, z + h, M::Appliance, M::Appliance, M::Appliance);
    // Fan grille disc on the front and the brackets under it.
    g.beam(at(base + n * (d + 0.05f) + t * (w * 0.15f), z + h * 0.5f),
           at(base + n * (d + 0.075f) + t * (w * 0.15f), z + h * 0.5f), h * 0.75f, M::Metal, 12);
    for (float o : {-w * 0.35f, w * 0.35f}) {
        g.beam(at(base + t * o, z - 0.35f), at(base + t * o + n * (d + 0.05f), z), 0.04f, M::Metal);
        g.beam(at(base + t * o, z - 0.02f), at(base + t * o + n * (d + 0.05f), z - 0.02f), 0.04f, M::Metal);
    }
    // Condensate pipe down the wall.
    g.beam(at(base + t * (-w * 0.45f) + n * 0.06f, z), at(base + t * (-w * 0.45f) + n * 0.06f, std::max(z - 2.5f, 0.3f)),
           0.03f, M::Metal, 6);
}

// Rooftop condenser: casing with a horizontal fan on top.
inline void roof_ac(Builder& g, V2 c, float z, float yaw) {
    const V2 ax{std::cos(yaw), std::sin(yaw)};
    g.box(c, ax, 0.55f, 0.45f, z, z + 0.9f, M::Appliance, M::Metal, M::Metal);
    g.beam(at(c, z + 0.9f), at(c, z + 0.95f), 0.7f, M::Metal, 12);
    g.beam(at(c + ax * 0.55f, z + 0.2f), at(c + ax * 1.2f, z + 0.2f), 0.08f, M::Metal, 6);
}

// Satellite dish on a short mast, aimed at `yaw` and tilted up.
inline void dish(Builder& g, V2 c, float z, float r, float yaw) {
    const float mast = 0.9f;
    g.beam(at(c, z), at(c, z + mast), 0.06f, M::Metal, 6);
    const Vec3 aim = normalize(Vec3{std::cos(yaw), std::sin(yaw), 0.6f});
    const Vec3 up{0.0f, 0.0f, 1.0f};
    const Vec3 sx = normalize(cross(aim, up)), sy = cross(sx, aim);
    const Vec3 centre = at(c, z + mast) + aim * 0.1f;
    const Vec3 back = centre - aim * (r * 0.35f);
    const int n = 12;
    for (int i = 0; i < n; ++i) {
        const float a0 = 2.0f * kPi * static_cast<float>(i) / n, a1 = 2.0f * kPi * static_cast<float>(i + 1) / n;
        const Vec3 p0 = centre + (sx * std::cos(a0) + sy * std::sin(a0)) * r;
        const Vec3 p1 = centre + (sx * std::cos(a1) + sy * std::sin(a1)) * r;
        g.tri(back, p1, p0, M::Appliance);  // concave front
        g.tri(back - aim * 0.02f, p0, p1, M::Metal);
    }
    g.beam(centre, centre + aim * (r * 0.9f), 0.03f, M::Metal);
    g.box({centre.x + aim.x * r * 0.9f, centre.y + aim.y * r * 0.9f}, {aim.x, aim.y}, 0.06f, 0.05f,
          centre.z + aim.z * r * 0.9f - 0.05f, centre.z + aim.z * r * 0.9f + 0.05f, M::Metal, M::Metal, M::Metal);
}

// Vent stack with a rain cap.
inline void vent(Builder& g, V2 c, float z, float h = 0.8f) {
    cylinder(g, c, z, 0.12f, h, 8, M::Metal, M::Metal);
    cylinder(g, c, z + h + 0.08f, 0.22f, 0.06f, 8, M::Metal, M::Metal);
}

// Window in a wall face: pane, frame, sill, optional security bars and hood.
struct WindowStyle {
    bool bars = false, hood = false;
};
inline void window(Builder& g, const Face& f, float a, float z0, float w, float h, WindowStyle st) {
    const float fr = 0.06f;
    face_box(g, f, a - w * 0.5f, a + w * 0.5f, 0.0f, 0.03f, z0, z0 + h, M::Window, M::Window, M::Window);
    face_box(g, f, a - w * 0.5f - fr, a + w * 0.5f + fr, 0.0f, 0.08f, z0 + h, z0 + h + fr, M::Metal, M::Metal, M::Metal);
    face_box(g, f, a - w * 0.5f - fr, a - w * 0.5f, 0.0f, 0.08f, z0, z0 + h, M::Metal, M::Metal, M::Metal);
    face_box(g, f, a + w * 0.5f, a + w * 0.5f + fr, 0.0f, 0.08f, z0, z0 + h, M::Metal, M::Metal, M::Metal);
    face_box(g, f, a - 0.02f, a + 0.02f, 0.0f, 0.06f, z0, z0 + h, M::Metal, M::Metal, M::Metal);  // mullion
    face_box(g, f, a - w * 0.5f - 0.1f, a + w * 0.5f + 0.1f, 0.0f, 0.16f, z0 - 0.08f, z0, M::Concrete, M::Concrete,
             M::Concrete);
    if (st.bars) {
        for (float x = a - w * 0.5f + 0.12f; x < a + w * 0.5f; x += 0.14f)
            g.beam(at(f.point(x, 0.18f), z0 - 0.05f), at(f.point(x, 0.18f), z0 + h + 0.05f), 0.02f, M::Metal, 4);
        for (float z : {z0 + 0.05f, z0 + h * 0.5f, z0 + h - 0.05f})
            g.beam(at(f.point(a - w * 0.5f, 0.18f), z), at(f.point(a + w * 0.5f, 0.18f), z), 0.025f, M::Metal, 4);
        face_box(g, f, a - w * 0.5f - 0.05f, a + w * 0.5f + 0.05f, 0.0f, 0.2f, z0 + h + 0.05f, z0 + h + 0.09f, M::Metal,
                 M::Metal, M::Metal);
    }
    if (st.hood) {
        const V2 p0 = f.point(a - w * 0.5f - 0.15f, 0.0f), p1 = f.point(a + w * 0.5f + 0.15f, 0.0f);
        const V2 q0 = f.point(a - w * 0.5f - 0.15f, 0.45f), q1 = f.point(a + w * 0.5f + 0.15f, 0.45f);
        const float zt = z0 + h + 0.35f, zb = z0 + h + 0.12f;
        g.quad_out(at(p0, zt), at(p1, zt), at(q1, zb), at(q0, zb), at(f.point(a, -1.0f), zb - 1.0f), M::Corrugated);
        g.quad_out(at(p0, zt - 0.02f), at(p1, zt - 0.02f), at(q1, zb - 0.02f), at(q0, zb - 0.02f),
                   at(f.point(a, 0.2f), zb + 1.0f), M::Metal);
    }
}

// Straight exterior stair up a wall face to a landing at z_top, starting at along a0 and
// climbing towards a0 + dir * run.
inline void wall_stair(Builder& g, const Face& f, float a0, float dir, float z_top) {
    const int steps = std::max(3, static_cast<int>(z_top / 0.19f));
    const float rise = z_top / static_cast<float>(steps), go = 0.26f, width = 0.9f;
    for (int i = 0; i < steps; ++i) {
        const float a = a0 + dir * (static_cast<float>(i) + 0.5f) * go;
        const float z = rise * static_cast<float>(i + 1);
        face_box(g, f, a - go * 0.5f, a + go * 0.5f, 0.05f, width, z - 0.05f, z, M::Metal, M::Metal, M::Metal);
    }
    const float a_end = a0 + dir * go * static_cast<float>(steps);
    // Landing and stringers.
    face_box(g, f, std::min(a_end, a_end + dir * 1.1f), std::max(a_end, a_end + dir * 1.1f), 0.05f, width, z_top - 0.08f,
             z_top, M::Metal, M::Metal, M::Metal);
    for (float o : {0.08f, width - 0.02f}) {
        g.beam(at(f.point(a0, o), 0.08f), at(f.point(a_end, o), z_top), 0.08f, M::Metal);  // clear of the ground
        // Handrail on posts.
        if (o > 0.5f) {
            g.beam(at(f.point(a0, o), 0.95f), at(f.point(a_end, o), z_top + 0.95f), 0.04f, M::Metal, 6);
            for (int k = 0; k <= steps; k += 3) {
                const float a = a0 + dir * go * static_cast<float>(k);
                const float z = rise * static_cast<float>(k);
                g.beam(at(f.point(a, o), z), at(f.point(a, o), z + 0.95f), 0.03f, M::Metal, 4);
            }
            g.beam(at(f.point(a_end, o), z_top + 0.95f), at(f.point(a_end + dir * 1.1f, o), z_top + 0.95f), 0.04f,
                   M::Metal, 6);
        }
    }
    g.beam(at(f.point(a_end + dir * 1.1f, width - 0.02f), 0.0f), at(f.point(a_end + dir * 1.1f, width - 0.02f), z_top),
           0.06f, M::Metal);
}

// A sagging string of paper lanterns between two points, with a light every few.
inline void lantern_string(Builder& g, Vec3 a, Vec3 b, int count, Rgb colour) {
    const float sag = 0.35f;
    Vec3 prev = a;
    for (int i = 1; i <= count + 1; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count + 1);
        const Vec3 p = a + (b - a) * t - Vec3{0.0f, 0.0f, sag * 4.0f * t * (1.0f - t)};
        g.beam(prev, p, 0.015f, M::Metal, 4);
        prev = p;
        if (i <= count) {
            const V2 c{p.x, p.y};
            g.beam(p, p - Vec3{0.0f, 0.0f, 0.12f}, 0.01f, M::Metal, 4);
            cylinder(g, c, p.z - 0.5f, 0.14f, 0.38f, 8, M::Lantern, M::Lantern);
            if (i % 3 == 1) g.light(p - Vec3{0.0f, 0.0f, 0.35f}, colour, 3.0f);
        }
    }
}

// TV antenna: a mast with a few crossbars of elements.
inline void tv_antenna(Builder& g, V2 c, float z, float h, float yaw) {
    g.beam(at(c, z), at(c, z + h), 0.04f, M::Metal, 6);
    const V2 d{std::cos(yaw), std::sin(yaw)}, s{-d.y, d.x};
    for (int k = 0; k < 3; ++k) {
        const float zz = z + h - 0.25f - 0.35f * static_cast<float>(k);
        const float len = 0.9f - 0.2f * static_cast<float>(k);
        g.beam(at(c - d * len * 0.5f, zz), at(c + d * len * 0.5f, zz), 0.02f, M::Metal, 4);
        for (float o = -len * 0.5f; o <= len * 0.5f; o += 0.18f)
            g.beam(at(c + d * o - s * 0.2f, zz), at(c + d * o + s * 0.2f, zz), 0.012f, M::Metal, 4);
    }
}

// Clothesline between two posts with a few hanging garments.
inline void clothesline(Builder& g, V2 a, V2 b, float z, Rng& rng) {
    const float h = 1.7f;
    g.beam(at(a, z), at(a, z + h), 0.04f, M::Metal, 4);
    g.beam(at(b, z), at(b, z + h), 0.04f, M::Metal, 4);
    g.beam(at(a, z + h), at(b, z + h), 0.01f, M::Metal, 4);
    const V2 d = (b - a) * (1.0f / std::max(len(b - a), 1e-3f));
    for (float t = 0.3f; t < len(b - a) - 0.3f; t += rng.range(0.45f, 0.8f)) {
        const float w = rng.range(0.3f, 0.55f), drop = rng.range(0.4f, 0.8f);
        const V2 p0 = a + d * t, p1 = a + d * (t + w);
        g.quad_out(at(p0, z + h), at(p1, z + h), at(p1, z + h - drop), at(p0, z + h - drop),
                   at(p0 + V2{d.y, -d.x}, z + h), M::Awning);
        g.quad_out(at(p0, z + h), at(p1, z + h), at(p1, z + h - drop), at(p0, z + h - drop),
                   at(p0 - V2{d.y, -d.x}, z + h), M::Awning);
    }
}

// Rooftop add-on room: a box shack with a sheet roof, a window and a door.
inline void roof_room(Builder& g, V2 c, float z, float hx, float hy, float yaw, Rng& rng) {
    const V2 ax{std::cos(yaw), std::sin(yaw)};
    const float h = rng.range(2.2f, 2.6f);
    g.box(c, ax, hx, hy, z - 0.3f, z + h, M::Siding, M::Corrugated, M::Metal);
    g.box(c, ax, hx + 0.25f, hy + 0.25f, z + h, z + h + 0.06f, M::Metal, M::Corrugated, M::Metal);
    const V2 n{-ax.y, ax.x};
    const Face f{c + n * hy, n, ax * -1.0f, hx};  // the +y side; tangent counter-clockwise
    window(g, f, -hx * 0.4f, z + 0.9f, std::min(0.9f, hx * 0.8f), 0.9f, {rng.chance(0.4f), false});
    face_box(g, f, hx * 0.3f, hx * 0.3f + 0.8f, 0.0f, 0.04f, z, z + 1.9f, M::Metal, M::Metal, M::Metal);  // door
}

// Stack of crates / sacks under a tarp.
inline void crates(Builder& g, V2 c, float z, float yaw, Rng& rng) {
    const V2 ax{std::cos(yaw), std::sin(yaw)}, ay{-ax.y, ax.x};
    const int n = 2 + rng.index(3);
    for (int i = 0; i < n; ++i) {
        const V2 p = c + ax * rng.range(-0.6f, 0.6f) + ay * rng.range(-0.5f, 0.5f);
        const float s = rng.range(0.25f, 0.4f), zz = z + (i >= 3 ? 0.6f : 0.0f);
        g.box(p, ax, s, s * 0.8f, zz, zz + s * 1.4f, rng.chance(0.5f) ? M::Plastic : M::Metal, M::Plastic, M::Metal);
    }
}

}  // namespace apex::kit
