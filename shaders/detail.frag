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
    vec3 n = dot(in_normal, in_normal) > 1e-6 ? normalize(in_normal) : vec3(0.0, 0.0, 1.0);
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
    } else if (mat == kMatShantyWall || mat == kMatSiding) {
        shanty_wall(u, v, seed, face_seed, b.pos_size.w, mat == kMatShantyWall, s.albedo, s.emissive);
    } else if (mat == kMatWindow) {
        s = window_pane(p, n, u, v, seed);
    } else if (mat == kMatAppliance) {
        // Off-white painted sheet metal, grimy, with louvre lines on the sides.
        float grime = 0.6 + 0.4 * value_noise(vec2(u * 3.0, v * 4.0) + float(seed & 255u));
        float louvre = abs(n.z) < 0.5 ? 0.85 + 0.15 * step(0.5, fract(v * 25.0)) : 1.0;
        s.albedo = vec3(0.30, 0.31, 0.29) * grime * louvre;
        s.specular = 0.25;
        s.shininess = 40.0;
    } else if (mat == kMatTank) {
        // Blue plastic tanks on most roofs, stainless steel on some.
        bool steel = hash_f(seed ^ 0x7a4u) < 0.35;
        float ribs = 0.85 + 0.15 * step(0.5, fract(v * 3.0));
        s.albedo = (steel ? vec3(0.22, 0.23, 0.24) : vec3(0.03, 0.10, 0.22)) * ribs * 5.0 *
                   (0.7 + 0.3 * value_noise(vec2(u * 2.0, v * 2.0)));
        s.specular = steel ? 0.8 : 0.3;
        s.shininess = steel ? 90.0 : 30.0;
    } else if (mat == kMatLantern) {
        // Paper lantern: glowing, darker ribs, warm core.
        float ribs = 0.7 + 0.3 * step(0.25, fract(v * 12.0));
        vec3 tint = hash_f(seed ^ 0x1a7u) < 0.6 ? vec3(1.0, 0.16, 0.07) : vec3(1.0, 0.55, 0.2);
        s.emissive = tint * 2.2 * ribs;
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
        // Soffits: dark ceiling with a lamp every 2.4 m (corridors, canopies).
        vec2 g = p.xy / 2.4;
        vec2 f = fract(g) - 0.5;
        uint th = hash_u3(uvec3(ucell(g), seed));
        float on = step(0.15, hash_f(th));
        float lamp = aa_box(f.x, -0.14, 0.14, fwidth(g.x)) * aa_box(f.y, -0.14, 0.14, fwidth(g.y));
        vec3 tint = mix(vec3(1.0, 0.88, 0.72), accent, step(0.6, hash_f(seed ^ 0x1eu)) * 0.7);
        s.albedo = vec3(0.06);
        float far_fade = smoothstep(0.3, 0.8, fwidth(g.x));
        s.emissive = mix(tint * lamp * on * 2.5, tint * 0.1, far_fade);
    }
    if (mat == kMatVending) {
        // Vending machine front: rows of backlit product dummies, a bright header, the
        // coin panel dark. u runs across the front, v is height.
        uint vh = hash_u(seed ^ uint(int(floor(u * 0.5 + 64.0))) ^ 0x7e7du);
        vec3 body = neon_color(vh) * 0.5 + 0.2;
        float rows = aa_box(fract(v / 0.32), 0.12, 0.88, fwidth(v / 0.32)) * step(0.7, v) * step(v, 1.6);
        float cols = aa_box(fract(u / 0.14 + 0.5), 0.15, 0.85, fwidth(u / 0.14));
        uint ph = hash_u2(uvec2(uint(int(floor(u / 0.14) + 64.0)), uint(int(floor(v / 0.32)))));
        vec3 product = neon_color(ph) * 0.7 + 0.3;
        float header = step(1.62, v) * step(v, 1.82);
        s.albedo = vec3(0.02);
        s.emissive = (rows * cols * product * 0.9 + header * body * 1.6 + vec3(0.35, 0.4, 0.45) * 0.25) * step(0.3, v);
        s.specular = 0.4;
        s.shininess = 200.0;
    } else if (mat == kMatPlastic) {
        s.albedo = vec3(0.012, 0.016, 0.014) * 5.0;
        s.specular = 0.8;
        s.shininess = 90.0;
    }
    // Photographic material detail: colour variation, normal relief and roughness from the
    // CC0 layers (materials.glsl), faded out with distance where it would only shimmer.
    float layer = -1.0, metres = 3.0;
    if (mat <= kMatGlass) {
        layer = district == 2u ? kTexPlaster : district == 1u ? kTexMetalPlates : kTexConcrete;
        metres = district == 1u ? 4.0 : 3.0;
    } else if (mat == kMatConcrete) {
        layer = kTexConcrete; metres = 2.5;
    } else if (mat == kMatMetal) {
        layer = kTexPaintedMetal; metres = 1.5;
    } else if (mat == kMatRoof) {
        layer = kTexAsphalt; metres = 4.0;
    } else if (mat == kMatCorrugated) {
        layer = kTexCorrugated; metres = 2.0;
    } else if (mat == kMatShantyWall || mat == kMatSiding) {
        layer = hash_f(hash_u2(uvec2(uint(int(floor(u / 2.1)) + 4096), face_seed)) ^ 0x2u) < 0.3 ? kTexRust : kTexCorrugated;
        metres = 2.0;
    } else if (mat == kMatAppliance) {
        layer = kTexPaintedMetal; metres = 1.0;
    } else if (mat == kMatLouvre) {
        layer = kTexMetalPlates; metres = 3.0;
    }
    vec3 shade_n = n;
    if (layer >= 0.0) {
        float dist = distance(p, frame.camera_pos.xyz);
        float strength = 1.0 - smoothstep(60.0, 220.0, dist);
        if (strength > 0.0) {
            vec3 t, b;
            vec2 uv;
            if (abs(n.z) > 0.7) {  // roofs, slabs: world-planar
                t = vec3(1.0, 0.0, 0.0);
                b = vec3(0.0, sign(n.z), 0.0);
                uv = p.xy * vec2(1.0, sign(n.z));
                // Corrugated sheet: the texture's ribs vary along its u; line them up with
                // the ridges corrugated_roof() draws (across y when along_x).
                if (mat == kMatCorrugated && (seed & 1u) == 0u) {
                    t = vec3(0.0, 1.0, 0.0);
                    b = vec3(1.0, 0.0, 0.0);
                    uv = p.yx;
                }
            } else {
                wall_frame(n, t, b);
                uv = vec2(u, v);
            }
            // Corrugated sheet: its ridges carry the relief, the photo only tints it. Painted
            // steel stays smooth (its chipped-paint photo reads as glitter under neon).
            float relief = mat == kMatCorrugated ? 0.35 : (mat == kMatMetal || mat == kMatAppliance) ? 0.4 : 1.0;
            TexSample ts = sample_material(layer, uv, metres, n, t, b, strength * relief * (1.0 - s.glass));
            s.albedo *= ts.tint;
            shade_n = ts.normal;
            // Rougher texels dull the highlights, smooth ones sharpen them.
            float gloss = mix(1.4, 0.5, ts.rough * strength);
            s.specular *= gloss;
            s.shininess *= mix(1.0, gloss, strength);
            ambient = building_ambient(seed, p, shade_n);
        }
    }
    if (mat == kMatCorrugated && n.z > 0.5) {
        // Tilt the shading normal across the ridges: long streak highlights down the sheet.
        shade_n = normalize(shade_n + vec3(s.material.zw * 2.0 - 1.0, 0.0) * 1.6);
    }
    out_color = vec4(apply_fog(shade_surface(s, p, shade_n, view_dir, ambient), p), 1.0);
    out_material = s.material;
}
