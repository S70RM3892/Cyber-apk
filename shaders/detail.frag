#version 460
#extension GL_GOOGLE_include_directive : require
// Detailed building meshes: the material id picks the surface model.
#include "include/building_surface.glsl"

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) flat in uint in_building;
layout(location = 4) flat in uint in_material;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

const float kFaceStride = 256.0;  // city_mesh.hpp

void main()
{
    Building b = buildings[in_building];
    uint seed = b.seed_district_flags_base.x;
    uint district = b.seed_district_flags_base.y;
    vec3 p = in_world_pos;
    vec3 n = normalize(in_normal);
    float t = frame.camera_pos.w;
    vec3 view_dir = normalize(p - frame.camera_pos.xyz);
    vec3 ambient = building_ambient(seed, p, n);
    float face = floor(in_uv.x / kFaceStride + 0.5);
    float u = in_uv.x - face * kFaceStride;
    float v = in_uv.y;
    uint face_seed = hash_u(seed ^ uint(int(face) + 3));
    uint mat = in_material;
    vec3 accent = neon_color(hash_u(seed ^ 0xbeefu));

    Surface s = surface_default();
    float wet = frame.fog.w;
    if (mat <= kMatGlass) {
        s = facade(u, v, seed, district, face_seed, n, view_dir, mat == kMatFacadeShop, mat == kMatGlass, false);
    } else if (mat == kMatRoof) {
        s = tar_roof(p, seed);
    } else if (mat == kMatCorrugated) {
        s = corrugated_roof(p, seed);
    } else if (mat == kMatShantyWall) {
        shanty_wall(u, v, seed, face_seed, b.pos_size.w, s.albedo, s.emissive);
    } else if (mat == kMatConcrete) {
        // Stained concrete: blotchy grime plus vertical rain streaks.
        vec2 q = abs(n.z) > 0.5 ? p.xy : vec2(u, v);
        float grime = 0.65 + 0.5 * fbm(q * 0.4 + float(seed & 255u));
        float streaks = 0.8 + 0.3 * value_noise(vec2(u * 2.5, v * 0.06));
        s.albedo = vec3(0.085, 0.088, 0.09) * grime * streaks * 5.0;
        if (n.z > 0.5) {
            s.material = vec4(0.3 * wet, 0.25, 0.5, 0.5);
            s.specular = 0.4 * wet;
            s.shininess = 60.0;
        }
    } else if (mat == kMatMetal) {
        float fresnel = pow(1.0 - abs(dot(view_dir, n)), 4.0);
        s.albedo = vec3(0.03, 0.033, 0.036) * 5.0;
        s.emissive = fresnel * vec3(0.004, 0.006, 0.007);
        s.specular = 0.5;
        s.shininess = 80.0;
        if (n.z > 0.5) s.material = vec4(0.4 * wet, 0.15, 0.5, 0.5);
    } else if (mat == kMatAwning) {
        uint ah = hash_u(face_seed ^ uint(int(floor(u * 0.2)) + 55));
        vec3 cloth = mix(neon_color(ah) * 0.35, vec3(0.3), step(0.5, fract(u * 0.8)) * step(0.5, hash_f(ah ^ 1u)));
        // Lit from the stall below: undersides glow in the cloth colour.
        s.albedo = cloth * 5.0;
        s.emissive = cloth * (n.z < 0.0 ? 0.35 : 0.06);
        s.specular = 0.3 * wet;
        s.shininess = 60.0;
    } else if (mat == kMatLed) {
        float pulse = 0.8 + 0.2 * sin(t * 1.7 - v * 0.05 + float(seed & 63u));
        s.emissive = accent * 4.0 * pulse;
    } else if (mat == kMatLedRed) {
        s.emissive = vec3(1.0, 0.05, 0.06) * 4.5;
    } else if (mat == kMatLouvre) {
        float slat = aa_box(fract(v / 0.3), 0.0, 0.55, fwidth(v / 0.3));
        s.albedo = vec3(0.03) * 5.0 * (0.6 + 0.4 * slat);
        s.emissive = (1.0 - slat) * vec3(0.9, 0.5, 0.3) * 0.02;
    } else if (mat == kMatSawGlass) {
        vec2 g = vec2(u / 1.2, (v - b.pos_size.w) / 0.8);
        float pane = aa_box(fract(g.x), 0.06, 0.94, fwidth(g.x)) * aa_box(fract(g.y), 0.08, 0.92, fwidth(g.y));
        uint ph = hash_u3(uvec3(ucell(g), seed));
        float lit = step(hash_f(ph), 0.7);
        s.albedo = vec3(0.01);
        s.emissive = pane * lit * vec3(1.0, 0.72, 0.42) * (0.3 + 0.3 * hash_f(ph ^ 3u));
    } else if (mat == kMatLitPanel) {
        // Soffit light panels: a grid of tiles, some colour-tinted, a few dead.
        vec2 g = p.xy / 1.2;
        float tile = aa_box(fract(g.x), 0.08, 0.92, fwidth(g.x)) * aa_box(fract(g.y), 0.08, 0.92, fwidth(g.y));
        uint th = hash_u3(uvec3(ucell(g), seed));
        float on = step(0.12, hash_f(th));
        vec3 tint = mix(vec3(1.0, 0.88, 0.72), accent, step(0.6, hash_f(seed ^ 0x1eu)) * 0.7);
        s.albedo = vec3(0.02);
        s.emissive = mix(tint * 0.6, tint * tile * on * 1.6, 1.0 - smoothstep(0.3, 0.8, fwidth(g.x)));
    }
    out_color = vec4(apply_fog(shade_surface(s, p, n, view_dir, ambient), p), 1.0);
    out_material = s.material;
}
