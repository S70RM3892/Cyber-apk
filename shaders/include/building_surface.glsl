// Building surface shading shared by the massing boxes (buildings.frag) and the
// detailed meshes (detail.frag): facades with procedural windows, roofs, shack
// siding, and the attachment materials of engine/include/apex/city_mesh.hpp.
// Everything is filtered with screen-space derivatives so window grids resolve to
// their average colour at distance instead of shimmering.
#ifndef APEX_BUILDING_SURFACE_GLSL
#define APEX_BUILDING_SURFACE_GLSL

#include "city_common.glsl"
#include "lighting.glsl"
#include "materials.glsl"

// SurfaceMaterial (city_mesh.hpp).
const uint kMatFacade = 0u;
const uint kMatFacadeShop = 1u;
const uint kMatGlass = 2u;
const uint kMatConcrete = 3u;
const uint kMatMetal = 4u;
const uint kMatRoof = 5u;
const uint kMatCorrugated = 6u;
const uint kMatShantyWall = 7u;
const uint kMatAwning = 8u;
const uint kMatLed = 9u;
const uint kMatLedRed = 10u;
const uint kMatLouvre = 11u;
const uint kMatSawGlass = 12u;
const uint kMatLitPanel = 13u;
const uint kMatVending = 14u;
const uint kMatPlastic = 15u;
const uint kMatWindow = 16u;
const uint kMatSiding = 17u;
const uint kMatAppliance = 18u;
const uint kMatTank = 19u;
const uint kMatLantern = 20u;

struct Surface {
    vec3 albedo;      // diffuse reflectance (pre-scaled: multiplied with ambient + local light)
    vec3 emissive;
    vec4 material;    // r reflectivity (SSR), g roughness, ba normal perturbation
    float specular;   // weight of the local-light highlights
    float shininess;
    float glass;      // 1 where the surface is window glass (material textures skip it)
};

Surface surface_default()
{
    Surface s;
    s.albedo = vec3(0.0);
    s.emissive = vec3(0.0);
    s.material = vec4(0.0, 1.0, 0.5, 0.5);
    s.specular = 0.02;
    s.shininess = 16.0;
    s.glass = 0.0;
    return s;
}

// Final radiance of a building surface: ambient + neon / shopfront lights, highlights.
vec3 shade_surface(Surface s, vec3 p, vec3 n, vec3 view_dir, vec3 ambient)
{
    vec3 diffuse, spec;
    local_lights(p, n, view_dir, s.shininess, diffuse, spec);
#ifdef APEX_RT
    // Ray-traced AO: corners, undersides and cluttered roofs sink into shadow; it takes
    // part of the direct neon too (large sources wrap into corners less than the model says).
    float ao = rt_ambient_occlusion(p, n);
    ambient *= ao;
    diffuse *= mix(0.55, 1.0, ao);
#endif
    return s.albedo * (ambient + diffuse * 0.6 + day_light(p, n)) + s.emissive + spec * s.specular;
}

// Anti-aliased box: 1 inside [lo, hi], filtered over the pixel footprint w.
float aa_box(float x, float lo, float hi, float w)
{
    return clamp((min(x - lo, hi - x)) / max(w, 1e-4) + 0.5, 0.0, 1.0);
}

vec3 window_light(uint h)
{
    // Weighted like night photos of Asian apartment blocks: mostly fluorescent white.
    float r = hash_f(h);
    if (r < 0.38) return vec3(0.80, 0.90, 1.00);       // cool fluorescent
    if (r < 0.52) return vec3(0.95, 0.95, 0.88);       // neutral LED
    if (r < 0.74) return vec3(1.0, 0.66, 0.36);        // warm tungsten
    if (r < 0.84) return vec3(0.35, 0.55, 1.00);       // screen glow
    if (r < 0.92) return vec3(1.0, 0.35, 0.55);        // pink LED
    return neon_color(hash_u(h ^ 0x5bd1e995u));        // neon-lit interior
}

// Ambient: dark sky from above, coloured street glow from below. Surfaces facing down
// (soffits, balcony undersides) see more of the street.
vec3 building_ambient(uint seed, vec3 p, vec3 n)
{
    float street_glow = exp(-max(p.z, 0.0) / 14.0);
    vec3 glow_tint = mix(neon_color(hash_u(seed)), neon_color(hash_u(seed + 7u)), 0.5);
    // Overcast night sky: teal fill, stronger from one side so volumes read (faces of a
    // box, ledges and balconies against their wall differ in brightness).
    const vec3 kSkyDir = normalize(vec3(0.55, 0.35, 0.75));
    vec3 sky = vec3(0.018, 0.028, 0.033) * (0.55 + 0.45 * n.z) +
               vec3(0.020, 0.036, 0.045) * max(dot(n, kSkyDir), 0.0);
    // City glow bounced up from the streets, reaching higher than the direct neon.
    vec3 bounce = vec3(0.020, 0.008, 0.012) * exp(-max(p.z, 0.0) / 60.0) * (0.6 - 0.4 * n.z);
    return sky + bounce + (glow_tint * 0.5 + vec3(0.25, 0.03, 0.04)) * street_glow * 0.06 * (1.0 - 0.6 * n.z);
}

// Low-rise shack walls: patchwork corrugated siding, small warm windows, roll-down
// shutters or open stalls at street level.
void shanty_wall(float u, float v, uint seed, uint face_seed, float height, bool painted_windows,
                 out vec3 albedo_out, out vec3 emissive)
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
    } else if (painted_windows && v < height - 0.4) {
        // Small windows on the upper floors (Close LOD models them instead).
        vec2 g = vec2(u / 2.1, (v - 3.2) / 2.8);
        vec2 f = fract(g);
        uint wh = hash_u3(uvec3(ucell(g), face_seed));
        float win = clamp((min(f.x - 0.3, 0.7 - f.x)) / max(fwidth(g.x), 1e-4) + 0.5, 0.0, 1.0) *
                    clamp((min(f.y - 0.35, 0.8 - f.y)) / max(fwidth(g.y), 1e-4) + 0.5, 0.0, 1.0);
        float lit = step(hash_f(wh), 0.45);
        emissive += win * lit * vec3(1.0, 0.6, 0.3) * (0.12 + 0.15 * hash_f(wh ^ 9u));
        albedo = mix(albedo, vec3(0.01, 0.015, 0.02), win * (1.0 - lit));
    }
    albedo_out = albedo * 6.0;
}

// Interior mapping (J. van Dongen, "Interior Mapping", CGI 2008): the window is a
// portal into a box room behind the facade. o: position in the room's front face (m,
// origin at the room's lower-left corner), d: view ray in room space (x along the wall,
// y up, z into the building), size: room extent (m). light: the room's light colour
// (zero for an unlit room). Returns the radiance seen through the glass.
vec3 interior_room(vec2 o, vec3 d, vec3 size, vec3 light, uint h)
{
    vec3 inv = 1.0 / max(abs(d), vec3(1e-4));
    float tx = (d.x > 0.0 ? size.x - o.x : o.x) * inv.x;
    float ty = (d.y > 0.0 ? size.y - o.y : o.y) * inv.y;
    float tz = size.z * inv.z;
    float t = min(tx, min(ty, tz));
    vec3 hit = vec3(o, 0.0) + d * t;
    vec3 q = hit / size;  // 0..1 in the room
    // One ceiling fixture: light falls off from it over the room.
    vec2 lamp_xz = vec2(0.3 + 0.4 * hash_f(h ^ 21u), 0.35 + 0.3 * hash_f(h ^ 22u));
    float lamp_d2 = dot(vec2(q.x, q.z) - lamp_xz, vec2(q.x, q.z) - lamp_xz);
    float falloff = 0.35 + 0.65 * exp(-lamp_d2 * 5.0);
    vec3 wall_tint = mix(vec3(0.9, 0.85, 0.8), vec3(0.6, 0.7, 0.8), hash_f(h ^ 23u));
    vec3 c;
    if (t == tz) {
        // Back wall: a cabinet / bed / desk silhouette, maybe a poster.
        float furn_x = hash_f(h ^ 11u), furn_w = 0.15 + 0.25 * hash_f(h ^ 12u);
        float furniture = step(q.y, 0.25 + 0.3 * hash_f(h ^ 13u)) * step(abs(q.x - furn_x), furn_w);
        float poster = step(abs(q.x - (1.0 - furn_x)), 0.12) * step(abs(q.y - 0.62), 0.14) * step(0.5, hash_f(h ^ 14u));
        c = wall_tint * 0.55 * (1.0 - 0.75 * furniture);
        c = mix(c, neon_color(h ^ 15u) * 0.8, poster * 0.7);
    } else if (t == ty) {
        if (d.y > 0.0) {
            // Ceiling with the fixture itself.
            float fixture = exp(-lamp_d2 * 120.0);
            c = wall_tint * 0.45 + vec3(3.0) * fixture;
        } else {
            c = vec3(0.35, 0.3, 0.26) * 0.35;  // floor
        }
    } else {
        c = wall_tint * 0.42;  // side walls
    }
    return c * light * falloff;
}

// Ray from the camera into a wall-aligned room: x along the wall (+u), y up, z inward.
vec3 room_ray(vec3 view_dir, vec3 n)
{
    vec3 tangent = vec3(-n.y, n.x, 0.0);
    return vec3(dot(view_dir, tangent), view_dir.z, max(-dot(view_dir, n), 0.05));
}

vec3 building_base_albedo(uint seed)
{
    return mix(vec3(0.026, 0.034, 0.038), vec3(0.050, 0.052, 0.050), hash_f(seed ^ 0x1234u));
}

// Window facade. u: metres along the face from its centre, v: height (m).
// shop: the lowest 4.6 m is a shopfront band. curtain: continuous glass office floors.
// painted_ac: draw AC units into the texture (the box LOD; meshes have real ones).
Surface facade(float u, float v, uint seed, uint district, uint face_seed, vec3 n, vec3 view_dir, bool shop_band,
               bool curtain, bool painted_ac)
{
    float t = frame.camera_pos.w;
    uint style = curtain ? 1u : district == 0u ? 2u : district == 1u ? 1u : 0u;
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
        // Filter each axis separately: across a floor the bays average out first, but
        // lit / dark floors survive as horizontal stripes at distance.
        float bx = smoothstep(0.35, 0.8, fw.x), by = smoothstep(0.35, 0.8, fw.y);
        float bay_term = mix(mullion * bay_lit * fixtures, 0.96 * 0.75 * 0.8, bx);
        float floor_term = mix(slab * floor_lit * ceiling, 0.82 * 0.25 * 0.6, by);
        windows = office * bay_term * floor_term * 0.3 * 1.6;
        if (far_blend < 0.99) {
            // Near: open-plan office floors seen through the glass, 3 m bays, 9 m deep.
            float bay_u = u / 3.0;
            uint bay_hash = hash_u3(uvec3(ucell(vec2(floor(bay_u), cell.y)), face_seed));
            vec3 light = office * floor_lit * step(hash_f(bay_hash), 0.8) * 0.16;
            light += vec3(0.002, 0.003, 0.005);  // dark floors keep a hint of their interior
            vec2 o = vec2(fract(bay_u) * 3.0, f.y * floor_h);
            vec3 room = interior_room(o, room_ray(view_dir, n), vec3(3.0, floor_h, 9.0), light, bay_hash);
            windows = mix(glass * room, windows, far_blend);
        }
    } else {
        float wx0 = style == 2u ? 0.2 : 0.22, wy0 = style == 2u ? 0.3 : 0.32;
        glass = aa_box(f.x, wx0, 1.0 - wx0, fw.x) * aa_box(f.y, wy0, 0.80, fw.y);
        if (painted_ac && style == 0u && hash_f(cell_hash ^ 0xacu) < 0.35) {
            float ac = aa_box(f.x, 0.52, 0.86, fw.x) * aa_box(f.y, 0.05, 0.24, fw.y);
            vec2 fan = (f - vec2(0.69, 0.145)) * vec2(pitch, floor_h);
            float blades = 0.75 + 0.25 * smoothstep(0.1, 0.08, length(fan));
            extra_albedo += ac * vec3(0.10, 0.11, 0.11) * blades * (1.0 - far_blend);
        }
        float occupancy = style == 2u ? 0.26 : district == 2u ? 0.2 : 0.08;
        // Occupancy varies per floor: at distance, towers read as floor stripes.
        float floor_var = 0.25 + 1.5 * hash_f(floor_hash ^ 0x0cu);
        float lit = step(hash_f(cell_hash), occupancy * floor_var);
        float flicker = hash_f(cell_hash ^ 0x9e37u) < 0.04 ? 0.6 + 0.4 * sin(t * 13.0 + float(cell_hash & 255u)) : 1.0;
        float blind = mix(0.3, 1.0, smoothstep(0.84, 0.4, f.y));
        vec3 wl = window_light(cell_hash) * (0.15 + 0.4 * hash_f(cell_hash ^ 0x77u)) * blind * flicker;
        float coverage = (1.0 - 2.0 * wx0) * (0.80 - wy0);
        // Far average: the mean of the near pattern (lit fraction x coverage x mean
        // brightness ~0.25), keeping per-floor variation while floors still resolve.
        float by = smoothstep(0.35, 0.8, fw.y);
        vec3 far_avg = vec3(0.85, 0.82, 0.78) * occupancy * coverage * 0.25 * mix(floor_var, 1.0, by);
        vec3 near = glass * lit * wl;
        if (far_blend < 0.99) {
            // Rooms behind the windows: one per window cell, 3.5-5 m deep.
            vec3 light = lit * window_light(cell_hash) * (0.2 + 0.7 * hash_f(cell_hash ^ 0x77u)) * flicker;
            // Dark rooms: a few flicker with a TV.
            float tv = step(hash_f(cell_hash ^ 0x7777u), 0.05) * (1.0 - lit);
            light += tv * vec3(0.15, 0.25, 0.6) * (0.6 + 0.4 * sin(t * 7.0 + float(cell_hash & 63u)) * sin(t * 2.3));
            light += vec3(0.002, 0.0025, 0.004);
            vec2 o = f * vec2(pitch, floor_h);
            vec3 room = interior_room(o, room_ray(view_dir, n), vec3(pitch, floor_h, 3.5 + 1.5 * hash_f(cell_hash ^ 5u)),
                                      light, cell_hash);
            // Blinds / curtains on some windows: a lit, translucent sheet at the glass.
            float has_blind = step(hash_f(cell_hash ^ 0xb1u), 0.35);
            float slats = mix(0.75, 1.0, step(0.5, fract(v * 12.0)) * (1.0 - smoothstep(0.3, 0.8, fwidth(v * 12.0))));
            room = mix(room, light * 0.35 * slats * blind, has_blind * smoothstep(0.84, 0.5, f.y));
            near = glass * room;
        }
        windows = mix(near, far_avg, far_blend);
    }

    Surface s = surface_default();
    // Street level: shopfront band with bright interiors.
    float shop = shop_band ? step(v, 4.6) : 0.0;
    uint shop_hash = hash_u(face_seed ^ uint(int(floor(u / 6.0)) + 1000));
    float shop_open = step(0.3, hash_f(shop_hash));
    float shop_win = aa_box(fract(u / 6.0), 0.06, 0.94, fwidth(u / 6.0)) * aa_box(v, 0.4, 3.6, fwidth(v));
    float shelves = 1.0 - 0.7 * aa_box(fract(v / 0.9), 0.0, 0.18, fwidth(v / 0.9)) * step(v, 2.6);
    float interior = mix(0.35, 1.0, smoothstep(0.5, 3.4, v)) * shelves;
    vec3 shop_light = mix(vec3(1.0, 0.8, 0.65), neon_color(shop_hash), 0.75) * 0.16 * shop_open * interior;
    // Neon strip over the shopfront.
    float strip_on = step(0.45, hash_f(shop_hash ^ 0x2du));
    float shop_strip = aa_box(v, 3.95, 4.2, fwidth(v)) * aa_box(fract(u / 6.0), 0.1, 0.9, fwidth(u / 6.0));
    s.emissive += shop_strip * strip_on * neon_color(shop_hash ^ 0x2du) * 10.0 * (shop_band ? 1.0 : 0.0);

    // Dark tinted glass: a faint teal sheen.
    vec3 glass_refl = vec3(0.010, 0.020, 0.024) * (0.6 + 0.8 * pow(1.0 - abs(dot(view_dir, n)), 3.0));
    // By day most rooms are unlit behind reflective glass that mirrors the sky.
    float d = daylight();
    vec3 sky_refl = sky_color(reflect(view_dir, n)) * (0.25 + 0.75 * pow(1.0 - abs(dot(view_dir, n)), 3.0));
    s.emissive += (1.0 - shop) * (windows * (1.0 - 0.8 * d) + glass * glass_refl * (1.0 - far_blend) +
                                  glass * sky_refl * d * 0.6);
    if (shop > 0.0 && far_blend < 0.99) {
        // Shops are rooms too: 6 m bays, 7 m deep, lit in the bay's colour.
        vec3 light = mix(vec3(1.0, 0.85, 0.7), neon_color(shop_hash), 0.65) * shop_open * 0.22 + vec3(0.003);
        vec2 o = vec2(fract(u / 6.0) * 6.0, v);
        vec3 room = interior_room(o, room_ray(view_dir, n), vec3(6.0, 3.8, 7.0), light, shop_hash);
        // Shelving rows seen through the glass.
        room *= 1.0 - 0.5 * aa_box(fract(v / 0.9), 0.0, 0.2, fwidth(v / 0.9)) * step(v, 2.4);
        // Roll-down shutter on closed shops.
        vec3 shutter = vec3(0.05, 0.055, 0.06) * (0.7 + 0.3 * step(0.5, fract(v * 7.0)));
        // Storefront glazing: mullions every 1.5 m, a transom bar, a darker counter.
        float mull = 1.0 - aa_box(fract(u / 1.5), 0.0, 0.04, fwidth(u / 1.5));
        float transom = 1.0 - aa_box(v, 2.75, 2.85, fwidth(v));
        float counter = mix(0.35, 1.0, smoothstep(0.9, 1.1, v));
        room *= mull * transom * counter;
        shop_light = mix(shutter * 0.2, room, shop_open);
    }
    s.emissive += shop * shop_win * shop_light;
    // Walls stay out of the SSR pass: it reflects about a (perturbed) up vector only.
    s.material = vec4(0.0, 1.0, 0.5, 0.5);

    float panel = 0.85 + 0.3 * hash_f3(uvec3(ucell(cell), seed));
    vec3 wall = building_base_albedo(seed) * panel;
    if (style != 1u) {
        float near_detail = 1.0 - far_blend;
        float wx0 = style == 2u ? 0.2 : 0.22, wy0 = style == 2u ? 0.3 : 0.32;
        // Window frame (lighter) and a sill ledge under the glass.
        float frame_outer = aa_box(f.x, wx0 - 0.04, 1.04 - wx0, fw.x) * aa_box(f.y, wy0 - 0.05, 0.83, fw.y);
        float sill = aa_box(f.x, wx0 - 0.07, 1.07 - wx0, fw.x) * aa_box(f.y, wy0 - 0.08, wy0 - 0.03, fw.y);
        wall = mix(wall, vec3(0.09, 0.09, 0.095), max(frame_outer - glass, 0.0) * near_detail);
        wall = mix(wall, vec3(0.12, 0.12, 0.12), sill * near_detail);
        // Dirt streaks running down from the sills.
        float streak_x = aa_box(f.x, wx0 + 0.05, 1.0 - wx0 - 0.05, fw.x) * value_noise(vec2(u * 6.0, cell.y));
        float below = smoothstep(wy0, wy0 - 0.3, f.y) * step(0.0, f.y);
        wall *= 1.0 - 0.45 * streak_x * below * near_detail;
        // Panel seams at each floor line.
        float seam = aa_box(f.y, 0.0, 0.02, fw.y) * near_detail;
        wall *= 1.0 - 0.5 * seam;
    }
    // Large-scale weathering so a facade isn't one flat colour.
    wall *= 0.75 + 0.5 * value_noise(vec2(u * 0.08, v * 0.05) + float(seed & 255u));
    s.albedo = (wall + extra_albedo) * 5.0 * (1.0 - glass * 0.6);
    s.glass = clamp(glass * (1.0 - shop) + shop * shop_win, 0.0, 1.0);
    // Glass catches sharp highlights from the signs around it.
    s.specular = mix(0.03, 0.25, glass * (1.0 - shop));
    s.shininess = mix(24.0, 256.0, glass);
    // Glass mirrors the street: screen-space reflections, ray-traced where they miss.
    float mirror = s.glass * (curtain ? 0.45 : 0.25);
    if (mirror > 0.01) s.material = vec4(mirror, 0.03, oct_encode(n));
    return s;
}

// Flat roofs: dark wet tar with scattered equipment lights.
Surface tar_roof(vec3 p, uint seed)
{
    Surface s = surface_default();
    vec2 cell = floor(p.xy / 3.0);
    float lamp = step(0.985, hash_f3(uvec3(ucell(cell), seed)));
    vec2 f = fract(p.xy / 3.0) - 0.5;
    s.emissive = lamp * smoothstep(0.25, 0.0, length(f)) * vec3(1.0, 0.25, 0.1) * 5.0;
    s.albedo = building_base_albedo(seed) * 4.0;
    float puddle = smoothstep(0.5, 0.65, fbm(p.xy * 0.2 + float(seed & 511u)));
    s.material = vec4(mix(0.25, 0.7, puddle) * frame.fog.w, mix(0.3, 0.05, puddle), 0.5, 0.5);
    s.specular = mix(0.3, 1.0, puddle) * frame.fog.w;
    s.shininess = mix(40.0, 400.0, puddle);
    return s;
}

// Wet corrugated metal: ridges along one axis, rusty patches.
Surface corrugated_roof(vec3 p, uint seed)
{
    Surface s = surface_default();
    bool along_x = (seed & 1u) == 0u;
    float c = along_x ? p.y : p.x;
    float ridge = sin(c * 40.0);
    float fw_c = fwidth(c * 40.0);
    float ridge_aa = ridge * (1.0 - smoothstep(0.5, 2.0, fw_c));  // flatten when sub-pixel
    float rust = smoothstep(0.35, 0.75, fbm(p.xy * 0.35 + float(seed & 1023u)));
    vec3 metal = mix(vec3(0.05, 0.055, 0.06), vec3(0.09, 0.045, 0.03), rust);
    s.albedo = metal * (0.8 + 0.2 * ridge_aa) * 5.0;
    vec2 perturb = along_x ? vec2(0.0, cos(c * 40.0) * 0.35) : vec2(cos(c * 40.0) * 0.35, 0.0);
    perturb *= 1.0 - smoothstep(0.5, 2.0, fw_c);
    s.material = vec4(mix(0.55, 0.2, rust) * frame.fog.w, mix(0.12, 0.4, rust), perturb * 0.5 + 0.5);
    // Wet sheet metal: long streaky highlights from the neon (the look of the target).
    s.specular = mix(1.2, 0.3, rust) * frame.fog.w;
    s.shininess = mix(120.0, 30.0, rust);
    return s;
}

// Modelled window pane (Close LOD): a lit or dark room behind reflective glass. Each
// window hashes its own light from its position (rounded to the window cell).
Surface window_pane(vec3 p, vec3 n, float u, float v, uint seed)
{
    Surface s = surface_default();
    uvec3 cell = uvec3(ivec3(floor(p.x * 0.6 + 1e4), floor(p.y * 0.6 + 1e4), floor(v / 2.8)));
    uint h = hash_u3(cell ^ uvec3(seed));
    float lit = step(hash_f(h ^ 0x11u), 0.6);
    vec3 room = window_light(h);
    // Ceiling light: brighter towards the top of the pane; curtains / blinds on some.
    float fv = fract(v / 2.8);
    float grad = 0.55 + 0.45 * smoothstep(0.0, 0.5, fv);
    float blinds = hash_f(h ^ 0x22u) < 0.35 ? 0.55 + 0.45 * step(0.5, fract(v * 9.0)) : 1.0;
    float curtain = hash_f(h ^ 0x33u) < 0.3 ? 0.35 : 1.0;
    s.emissive = lit * room * (0.35 + 0.5 * hash_f(h ^ 0x44u)) * grad * blinds * curtain;
    s.albedo = vec3(0.01, 0.012, 0.015);
    s.specular = 1.0;
    s.shininess = 300.0;
    s.glass = 1.0;
    s.material = vec4(0.35, 0.02, oct_encode(n));
    return s;
}

#endif
