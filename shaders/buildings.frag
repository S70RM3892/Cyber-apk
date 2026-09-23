#version 460
#extension GL_GOOGLE_include_directive : require
// Procedural facades. Three styles: punched windows (residential / industrial),
// glass curtain walls with lit office floors (corporate), and dense small-window
// grids (megastructures). Street-level shopfronts, LED edge strips, accent bands and
// aviation lights on top. Everything is filtered with screen-space derivatives so
// window grids resolve to their average colour at distance instead of shimmering —
// the main source of aliasing on a phone screen.
#include "include/city_common.glsl"

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) flat in vec3 in_normal;
layout(location = 2) flat in uint in_instance;

layout(location = 0) out vec4 out_color;     // HDR radiance
layout(location = 1) out vec4 out_material;  // r reflectivity, g roughness, ba normal perturbation

// Anti-aliased box: 1 inside [lo, hi], filtered over the pixel footprint w.
float aa_box(float x, float lo, float hi, float w)
{
    return clamp((min(x - lo, hi - x)) / max(w, 1e-4) + 0.5, 0.0, 1.0);
}

vec3 window_light(uint h)
{
    float r = hash_f(h);
    if (r < 0.50) return vec3(1.0, 0.62, 0.32);        // warm tungsten
    if (r < 0.72) return vec3(0.70, 0.85, 1.00);       // cool fluorescent
    if (r < 0.84) return vec3(0.35, 0.55, 1.00);       // screen glow
    if (r < 0.92) return vec3(1.0, 0.35, 0.55);        // pink LED
    return neon_color(hash_u(h ^ 0x5bd1e995u));        // neon-lit interior
}

void main()
{
    Building b = buildings[in_instance];
    uint seed = b.seed_district_flags_base.x;
    uint district = b.seed_district_flags_base.y;
    bool top_tier = (b.seed_district_flags_base.z & kTopTier) != 0u;
    float base_z = uintBitsToFloat(b.seed_district_flags_base.w);
    vec3 n = in_normal;
    vec3 p = in_world_pos;
    float t = frame.camera_pos.w;
    vec3 view_dir = normalize(p - frame.camera_pos.xyz);

    vec3 base_albedo = mix(vec3(0.030, 0.032, 0.040), vec3(0.065, 0.060, 0.055), hash_f(seed ^ 0x1234u));
    vec3 color;
    vec3 emissive = vec3(0.0);

    // Ambient: dark sky from above + coloured street glow from below.
    float street_glow = exp(-max(p.z, 0.0) / 14.0);
    vec3 glow_tint = mix(neon_color(hash_u(seed)), neon_color(hash_u(seed + 7u)), 0.5);
    vec3 ambient = vec3(0.010, 0.012, 0.025) + glow_tint * street_glow * 0.12;

    if (n.z > 0.5) {
        // Roofs: dark, with scattered equipment lights and a lit parapet edge.
        vec2 cell = floor(p.xy / 3.0);
        float lamp = step(0.985, hash_f3(uvec3(ucell(cell), seed)));
        vec2 f = fract(p.xy / 3.0) - 0.5;
        emissive += lamp * smoothstep(0.25, 0.0, length(f)) * vec3(1.0, 0.25, 0.1) * 5.0;
        color = base_albedo * ambient * 4.0;
    } else {
        // Wall coordinates: u along the wall, v up.
        float u = dot(p.xy - b.pos_size.xy, vec2(-n.y, n.x));
        float v = p.z;
        float face_id = dot(n.xy, vec2(1.0, 2.0));
        uint face_seed = hash_u(seed ^ uint(int(face_id) + 3));
        float half_size = b.pos_size.z * 0.5;
        float edge = half_size - abs(u);

        uint style = district == 1u ? 1u : district == 0u ? 2u : 0u;
        float floor_h = district == 2u ? 3.1 : 3.8;
        float pitch = style == 2u ? 1.6 : mix(2.0, 3.4, hash_f(seed ^ 0xa5u));
        vec2 grid = vec2(u / pitch, v / floor_h);
        vec2 cell = floor(grid);
        vec2 f = fract(grid);
        vec2 fw = fwidth(grid);
        float footprint = max(fw.x, fw.y);
        float far_blend = smoothstep(0.35, 0.8, footprint);

        uint cell_hash = hash_u3(uvec3(ucell(cell), face_seed));
        uint floor_hash = hash_u2(uvec2(uint(int(cell.y) + 4096), face_seed));
        vec3 windows;
        float glass;
        if (style == 1u) {
            // Curtain wall: continuous glass, whole office floors lit or dark.
            float slab = 1.0 - aa_box(f.y, 0.0, 0.18, fw.y);
            float mullion = aa_box(fract(u / 1.5), 0.04, 0.96, fwidth(u / 1.5));
            glass = slab * mullion;
            float floor_lit = step(hash_f(floor_hash), 0.35);
            float bay_lit = step(hash_f(cell_hash), 0.75);
            vec3 office = mix(vec3(0.75, 0.88, 1.0), vec3(1.0, 0.85, 0.65), step(0.7, hash_f(floor_hash ^ 3u)));
            // Interior read: bright ceiling fixtures near the top of the floor, darker
            // desks/partitions below, fixture rows every 3 m.
            float ceiling = mix(0.25, 1.0, smoothstep(0.45, 0.95, f.y));
            float fixtures = mix(0.6, 1.0, aa_box(fract(u / 3.0), 0.2, 0.8, fwidth(u / 3.0)));
            vec3 near_w = glass * floor_lit * bay_lit * office * ceiling * fixtures * 0.45;
            windows = mix(near_w, office * 0.35 * 0.75 * 0.82 * 0.5 * 0.45, far_blend);
        } else {
            float wx0 = style == 2u ? 0.2 : 0.16, wy0 = style == 2u ? 0.3 : 0.24;
            glass = aa_box(f.x, wx0, 1.0 - wx0, fw.x) * aa_box(f.y, wy0, 0.84, fw.y);
            float occupancy = style == 2u ? 0.42 : district == 2u ? 0.34 : 0.10;
            float lit = step(hash_f(cell_hash), occupancy);
            float flicker = hash_f(cell_hash ^ 0x9e37u) < 0.04 ? 0.6 + 0.4 * sin(t * 13.0 + float(cell_hash & 255u)) : 1.0;
            float blind = mix(0.3, 1.0, smoothstep(0.84, 0.4, f.y));
            vec3 wl = window_light(cell_hash) * (0.25 + 0.5 * hash_f(cell_hash ^ 0x77u)) * blind * flicker;
            float coverage = (1.0 - 2.0 * wx0) * (0.84 - wy0);
            windows = mix(glass * lit * wl, vec3(0.8, 0.62, 0.48) * occupancy * coverage * 0.6, far_blend);
        }

        // Street level (ground tier only): shopfront band with bright interiors.
        float shop = base_z < 0.5 ? step(v, 4.6) : 0.0;
        uint shop_hash = hash_u(face_seed ^ uint(int(floor(u / 6.0)) + 1000));
        float shop_open = step(0.3, hash_f(shop_hash));
        float shop_win = aa_box(fract(u / 6.0), 0.06, 0.94, fwidth(u / 6.0)) * aa_box(v, 0.4, 3.6, fwidth(v));
        // Interior: bright top, shelves/counter silhouettes lower down.
        float shelves = 1.0 - 0.7 * aa_box(fract(v / 0.9), 0.0, 0.18, fwidth(v / 0.9)) * step(v, 2.6);
        float interior = mix(0.35, 1.0, smoothstep(0.5, 3.4, v)) * shelves;
        vec3 shop_light = mix(vec3(1.0, 0.85, 0.7), neon_color(shop_hash), 0.6) * 0.5 * shop_open * interior;
        // Neon strip over the shopfront.
        float strip_on = step(0.45, hash_f(shop_hash ^ 0x2du));
        float shop_strip = aa_box(v, 3.95, 4.2, fwidth(v)) * aa_box(fract(u / 6.0), 0.1, 0.9, fwidth(u / 6.0));
        emissive += shop_strip * strip_on * neon_color(shop_hash ^ 0x2du) * 10.0 * (base_z < 0.5 ? 1.0 : 0.0);

        vec3 glass_refl = sky_color(reflect(view_dir, n)) * 0.8;
        emissive += (1.0 - shop) * (windows + glass * glass_refl * (1.0 - far_blend));
        emissive += shop * shop_win * shop_light;

        // LED edge strips on corporate towers, accent bands on megastructures.
        vec3 accent = neon_color(hash_u(seed ^ 0xbeefu));
        if (district == 1u) {
            float strip = aa_box(edge, 0.0, 0.3, fwidth(edge));
            float pulse = 0.75 + 0.25 * sin(t * 1.5 - v * 0.08);
            emissive += strip * accent * 4.0 * pulse * step(base_z + 1.0, v);
        } else if (district == 0u) {
            float band_v = fract(v / (floor_h * 6.0));
            float band = aa_box(band_v, 0.0, 0.03, fwidth(v / (floor_h * 6.0)));
            emissive += band * accent * 3.0 * step(10.0, v);
        }
        // Lit cornice where a setback steps in.
        float cornice = aa_box(v, b.pos_size.w - 0.5, b.pos_size.w, fwidth(v)) * (top_tier ? 0.3 : 1.0);
        emissive += cornice * accent * 2.0 * step(20.0, v);

        // Aviation lights on the roof corners of tall towers.
        if (top_tier && b.pos_size.w > 90.0) {
            float top = aa_box(v, b.pos_size.w - 1.2, b.pos_size.w - 0.4, fwidth(v));
            float corner = aa_box(edge, 0.0, 0.8, fwidth(edge));
            float blink = step(0.5, fract(t * 0.5 + hash_f(seed)));
            emissive += top * corner * blink * vec3(1.0, 0.05, 0.02) * 30.0;
        }

        float panel = 0.85 + 0.3 * hash_f3(uvec3(ucell(cell), seed));
        color = base_albedo * panel * ambient * 5.0 * (1.0 - glass * 0.6);
    }

    out_color = vec4(apply_fog(color + emissive, p), 1.0);
    out_material = vec4(0.0, 1.0, 0.5, 0.5);
}
