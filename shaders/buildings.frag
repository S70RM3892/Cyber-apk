#version 460
#extension GL_GOOGLE_include_directive : require
// Massing boxes (far LOD / fallback for buildings without a mesh). Facade, roof and
// shack shading live in include/building_surface.glsl, shared with detail.frag.
#include "include/building_surface.glsl"

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) flat in vec3 in_normal;
layout(location = 2) flat in uint in_instance;

layout(location = 0) out vec4 out_color;     // HDR radiance
layout(location = 1) out vec4 out_material;  // r reflectivity, g roughness, ba normal perturbation
layout(location = 2) out vec4 out_albedo;    // RT G-buffer (0: not lit by the ray-traced pass)

void main()
{
    out_albedo = vec4(0.0);
    Building b = buildings[in_instance];
    uint seed = b.seed_district_flags_base.x;
    uint district = b.seed_district_flags_base.y;
    bool top_tier = (b.seed_district_flags_base.z & kTopTier) != 0u;
    bool shanty = (b.seed_district_flags_base.z & kShanty) != 0u;
    float base_z = uintBitsToFloat(b.seed_district_flags_base.w);
    vec3 n = in_normal;
    vec3 p = in_world_pos;
    float t = frame.camera_pos.w;
    vec3 view_dir = normalize(p - frame.camera_pos.xyz);
    vec3 ambient = building_ambient(seed, p, n);

    Surface s = surface_default();
    if (n.z > 0.5) {
        s = shanty ? corrugated_roof(p, seed) : tar_roof(p, seed);
    } else {
        float u = dot(p.xy - b.pos_size.xy, vec2(-n.y, n.x));
        float face_id = dot(n.xy, vec2(1.0, 2.0));
        uint face_seed = hash_u(seed ^ uint(int(face_id) + 3));
        if (shanty) {
            shanty_wall(u, p.z, seed, face_seed, b.pos_size.w, true, s.albedo, s.emissive);
        } else {
            s = facade(u, p.z, seed, district, face_seed, n, view_dir, base_z < 0.5, false, true);
            float edge = b.pos_size.z * 0.5 - abs(u);
            float v = p.z;
            // LED edge strips on corporate towers, accent bands on megastructures.
            vec3 accent = neon_color(hash_u(seed ^ 0xbeefu));
            if (district == 1u && hash_f(seed ^ 0x51edu) < 0.3) {
                float strip = aa_box(edge, 0.0, 0.3, fwidth(edge));
                float pulse = 0.75 + 0.25 * sin(t * 1.5 - v * 0.08);
                s.emissive += strip * accent * 3.0 * pulse * step(b.pos_size.w * 0.6, v);
            } else if (district == 0u && hash_f(seed ^ 0xbadu) < 0.35) {
                float band_v = fract(v / 38.0);
                float band = aa_box(band_v, 0.0, 0.025, fwidth(v / 38.0));
                s.emissive += band * accent * 1.8 * step(10.0, v);
            }
            // Lit cornice where a setback steps in.
            float cornice = aa_box(v, b.pos_size.w - 0.5, b.pos_size.w, fwidth(v)) * (top_tier ? 0.3 : 1.0);
            s.emissive += cornice * accent * 1.2 * step(20.0, v) * step(hash_f(seed ^ 0xc0u), 0.35);
        }
    }
    out_color = vec4(apply_fog(shade_surface(s, p, n, view_dir, ambient), p), 1.0);
    out_material = s.material;
#ifdef APEX_RT
    out_albedo = g_rt_albedo;
    out_material.ba = oct_encode(g_rt_normal);
#endif
}
