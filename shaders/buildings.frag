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

// Low-rise shack walls: patchwork corrugated siding, small warm windows, roll-down
// shutters or open stalls at street level.
void shanty_wall(float u, float v, uint seed, uint face_seed, float height, vec3 ambient, float t,
                 out vec3 color, out vec3 emissive)
{
    // Patchwork panels ~2 m wide, each its own paint / rust.
    float panel_id = floor(u / 2.1);
    uint ph = hash_u2(uvec2(uint(int(panel_id) + 4096), face_seed));
    const vec3 paints[6] = vec3[6](vec3(0.09, 0.05, 0.035), vec3(0.04, 0.07, 0.07), vec3(0.06, 0.06, 0.06),
                                   vec3(0.11, 0.03, 0.03), vec3(0.05, 0.05, 0.08), vec3(0.08, 0.07, 0.05));
    vec3 albedo = paints[ph % 6u] * (0.75 + 0.5 * hash_f(ph ^ 5u));
    float ribs = 0.8 + 0.2 * sin(u * 52.0);                       // vertical corrugation
    float grime = 0.7 + 0.3 * value_noise(vec2(u * 0.7, v * 1.3) + float(seed & 255u));
    albedo *= ribs * grime;
    emissive = vec3(0.0);

    // Street level: open stall (bright, colourful) or roll-down shutter.
    float fw_v = fwidth(v), fw_u = fwidth(u);
    if (v < 3.2) {
        uint sh = hash_u2(uvec2(uint(int(floor(u / 3.0)) + 777), face_seed));
        float stall = step(0.45, hash_f(sh));
        float opening = clamp((min(fract(u / 3.0) - 0.06, 0.94 - fract(u / 3.0))) / max(fw_u / 3.0, 1e-4) + 0.5, 0.0, 1.0) *
                        clamp((min(v - 0.2, 2.7 - v)) / max(fw_v, 1e-4) + 0.5, 0.0, 1.0);
        vec3 inside = mix(vec3(1.0, 0.7, 0.45), neon_color(sh), 0.5) * (0.25 + 0.15 * sin(v * 9.0 + float(sh)));
        vec3 shutter = vec3(0.07, 0.075, 0.08) * (0.8 + 0.2 * step(0.5, fract(v * 6.0)));
        albedo = mix(albedo, stall > 0.5 ? vec3(0.0) : shutter, opening);
        emissive += stall * opening * inside;
        // Awning strip above the stall glows faintly.
        float awning = clamp((0.12 - abs(v - 2.9)) / max(fw_v, 1e-4), 0.0, 1.0) * stall;
        emissive += awning * neon_color(sh ^ 3u) * 0.9;
    } else if (v < height - 0.4) {
        // Small windows on the upper floors.
        vec2 g = vec2(u / 2.1, (v - 3.2) / 2.8);
        vec2 f = fract(g);
        uint wh = hash_u3(uvec3(ucell(g), face_seed));
        float win = clamp((min(f.x - 0.3, 0.7 - f.x)) / max(fwidth(g.x), 1e-4) + 0.5, 0.0, 1.0) *
                    clamp((min(f.y - 0.35, 0.8 - f.y)) / max(fwidth(g.y), 1e-4) + 0.5, 0.0, 1.0);
        float lit = step(hash_f(wh), 0.45);
        emissive += win * lit * vec3(1.0, 0.6, 0.3) * (0.12 + 0.15 * hash_f(wh ^ 9u));
        albedo = mix(albedo, vec3(0.01, 0.015, 0.02), win * (1.0 - lit));
    }
    color = albedo * ambient * 6.0;
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

    vec3 base_albedo = mix(vec3(0.026, 0.034, 0.038), vec3(0.050, 0.052, 0.050), hash_f(seed ^ 0x1234u));
    vec3 color;
    vec3 emissive = vec3(0.0);

    // Ambient: dark sky from above + coloured street glow from below.
    float street_glow = exp(-max(p.z, 0.0) / 14.0);
    vec3 glow_tint = mix(neon_color(hash_u(seed)), neon_color(hash_u(seed + 7u)), 0.5);
    vec3 ambient = vec3(0.008, 0.013, 0.015) + (glow_tint * 0.5 + vec3(0.25, 0.03, 0.04)) * street_glow * 0.06;

    bool shanty = (b.seed_district_flags_base.z & kShanty) != 0u;
    vec4 material = vec4(0.0, 1.0, 0.5, 0.5);  // reflectivity, roughness, normal perturbation

    if (n.z > 0.5) {
        if (shanty) {
            // Wet corrugated metal: ridges along one axis, rusty patches, puddled troughs.
            bool along_x = (seed & 1u) == 0u;
            float c = along_x ? p.y : p.x;
            float ridge = sin(c * 40.0);
            float fw_c = fwidth(c * 40.0);
            float ridge_aa = ridge * (1.0 - smoothstep(0.5, 2.0, fw_c));  // flatten when sub-pixel
            float rust = smoothstep(0.35, 0.75, fbm(p.xy * 0.35 + float(seed & 1023u)));
            vec3 metal = mix(vec3(0.05, 0.055, 0.06), vec3(0.09, 0.045, 0.03), rust);
            color = metal * (0.8 + 0.2 * ridge_aa) * ambient * 5.0;
            vec2 perturb = along_x ? vec2(0.0, cos(c * 40.0) * 0.35) : vec2(cos(c * 40.0) * 0.35, 0.0);
            perturb *= 1.0 - smoothstep(0.5, 2.0, fw_c);
            material = vec4(mix(0.55, 0.2, rust) * frame.fog.w, mix(0.12, 0.4, rust), perturb * 0.5 + 0.5);
        } else {
            // Flat roofs: dark wet tar with scattered equipment lights.
            vec2 cell = floor(p.xy / 3.0);
            float lamp = step(0.985, hash_f3(uvec3(ucell(cell), seed)));
            vec2 f = fract(p.xy / 3.0) - 0.5;
            emissive += lamp * smoothstep(0.25, 0.0, length(f)) * vec3(1.0, 0.25, 0.1) * 5.0;
            color = base_albedo * ambient * 4.0;
            float puddle = smoothstep(0.5, 0.65, fbm(p.xy * 0.2 + float(seed & 511u)));
            material = vec4(mix(0.25, 0.7, puddle) * frame.fog.w, mix(0.3, 0.05, puddle), 0.5, 0.5);
        }
    } else if (shanty) {
        float u = dot(p.xy - b.pos_size.xy, vec2(-n.y, n.x));
        float face_id = dot(n.xy, vec2(1.0, 2.0));
        uint face_seed = hash_u(seed ^ uint(int(face_id) + 3));
        shanty_wall(u, p.z, seed, face_seed, b.pos_size.w, ambient, t, color, emissive);
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
        float pitch = style == 2u ? 1.6 : mix(1.8, 2.6, hash_f(seed ^ 0xa5u));
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
        vec3 extra_albedo = vec3(0.0);
        if (style == 1u) {
            // Curtain wall: continuous glass, whole office floors lit or dark.
            float slab = 1.0 - aa_box(f.y, 0.0, 0.18, fw.y);
            float mullion = aa_box(fract(u / 1.5), 0.04, 0.96, fwidth(u / 1.5));
            glass = slab * mullion;
            float floor_lit = step(hash_f(floor_hash), 0.25);
            float bay_lit = step(hash_f(cell_hash), 0.75);
            vec3 office = mix(vec3(0.75, 0.88, 1.0), vec3(1.0, 0.85, 0.65), step(0.7, hash_f(floor_hash ^ 3u)));
            // Interior read: bright ceiling fixtures near the top of the floor, darker
            // desks/partitions below, fixture rows every 3 m.
            float ceiling = mix(0.25, 1.0, smoothstep(0.45, 0.95, f.y));
            float fixtures = mix(0.6, 1.0, aa_box(fract(u / 3.0), 0.2, 0.8, fwidth(u / 3.0)));
            vec3 near_w = glass * floor_lit * bay_lit * office * ceiling * fixtures * 0.3;
            // Far away the office grid becomes a field of pale dots: keep its average up.
            windows = mix(near_w, office * 0.25 * 0.75 * 0.82 * 0.5 * 0.55, far_blend);
        } else {
            float wx0 = style == 2u ? 0.2 : 0.22, wy0 = style == 2u ? 0.3 : 0.32;
            glass = aa_box(f.x, wx0, 1.0 - wx0, fw.x) * aa_box(f.y, wy0, 0.80, fw.y);
            // Air-conditioner units under some windows: small light-grey boxes with a fan.
            if (style == 0u && hash_f(cell_hash ^ 0xacu) < 0.35) {
                float ac = aa_box(f.x, 0.52, 0.86, fw.x) * aa_box(f.y, 0.05, 0.24, fw.y);
                vec2 fan = (f - vec2(0.69, 0.145)) * vec2(pitch, floor_h);
                float blades = 0.75 + 0.25 * smoothstep(0.1, 0.08, length(fan));
                extra_albedo += ac * vec3(0.10, 0.11, 0.11) * blades * (1.0 - far_blend);
            }
            float occupancy = style == 2u ? 0.34 : district == 2u ? 0.26 : 0.08;
            float lit = step(hash_f(cell_hash), occupancy);
            float flicker = hash_f(cell_hash ^ 0x9e37u) < 0.04 ? 0.6 + 0.4 * sin(t * 13.0 + float(cell_hash & 255u)) : 1.0;
            float blind = mix(0.3, 1.0, smoothstep(0.84, 0.4, f.y));
            vec3 wl = window_light(cell_hash) * (0.15 + 0.4 * hash_f(cell_hash ^ 0x77u)) * blind * flicker;
            float coverage = (1.0 - 2.0 * wx0) * (0.80 - wy0);
            windows = mix(glass * lit * wl, vec3(0.75, 0.62, 0.5) * occupancy * coverage * 0.5, far_blend);
        }

        // Street level (ground tier only): shopfront band with bright interiors.
        float shop = base_z < 0.5 ? step(v, 4.6) : 0.0;
        uint shop_hash = hash_u(face_seed ^ uint(int(floor(u / 6.0)) + 1000));
        float shop_open = step(0.3, hash_f(shop_hash));
        float shop_win = aa_box(fract(u / 6.0), 0.06, 0.94, fwidth(u / 6.0)) * aa_box(v, 0.4, 3.6, fwidth(v));
        // Interior: bright top, shelves/counter silhouettes lower down.
        float shelves = 1.0 - 0.7 * aa_box(fract(v / 0.9), 0.0, 0.18, fwidth(v / 0.9)) * step(v, 2.6);
        float interior = mix(0.35, 1.0, smoothstep(0.5, 3.4, v)) * shelves;
        vec3 shop_light = mix(vec3(1.0, 0.8, 0.65), neon_color(shop_hash), 0.75) * 0.16 * shop_open * interior;
        // Neon strip over the shopfront.
        float strip_on = step(0.45, hash_f(shop_hash ^ 0x2du));
        float shop_strip = aa_box(v, 3.95, 4.2, fwidth(v)) * aa_box(fract(u / 6.0), 0.1, 0.9, fwidth(u / 6.0));
        emissive += shop_strip * strip_on * neon_color(shop_hash ^ 0x2du) * 10.0 * (base_z < 0.5 ? 1.0 : 0.0);

        // Dark tinted glass: a faint teal sheen, not a mirror of the red horizon haze.
        vec3 glass_refl = vec3(0.010, 0.020, 0.024) * (0.6 + 0.8 * pow(1.0 - abs(dot(view_dir, n)), 3.0));
        emissive += (1.0 - shop) * (windows + glass * glass_refl * (1.0 - far_blend));
        emissive += shop * shop_win * shop_light;

        // LED edge strips on corporate towers, accent bands on megastructures.
        vec3 accent = neon_color(hash_u(seed ^ 0xbeefu));
        if (district == 1u && hash_f(seed ^ 0x51edu) < 0.3) {
            // A minority of towers carry LED corner strips (on the upper part only), so the
            // skyline reads as solid masses rather than wireframes.
            float strip = aa_box(edge, 0.0, 0.3, fwidth(edge));
            float pulse = 0.75 + 0.25 * sin(t * 1.5 - v * 0.08);
            emissive += strip * accent * 3.0 * pulse * step(b.pos_size.w * 0.6, v);
        } else if (district == 0u && hash_f(seed ^ 0xbadu) < 0.35) {
            float band_v = fract(v / (floor_h * 10.0));
            float band = aa_box(band_v, 0.0, 0.025, fwidth(v / (floor_h * 10.0)));
            emissive += band * accent * 1.8 * step(10.0, v);
        }
        // Lit cornice where a setback steps in.
        float cornice = aa_box(v, b.pos_size.w - 0.5, b.pos_size.w, fwidth(v)) * (top_tier ? 0.3 : 1.0);
        emissive += cornice * accent * 1.2 * step(20.0, v) * step(hash_f(seed ^ 0xc0u), 0.35);

        float panel = 0.85 + 0.3 * hash_f3(uvec3(ucell(cell), seed));
        color = (base_albedo * panel + extra_albedo) * ambient * 5.0 * (1.0 - glass * 0.6);
    }

    out_color = vec4(apply_fog(color + emissive, p), 1.0);
    out_material = material;
}
