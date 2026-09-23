#version 460
#extension GL_GOOGLE_include_directive : require
// Shading for lamp posts and pedestrians: dark silhouettes lit by the street lamps
// and neon, with emissive lamp heads, jacket trims and LED umbrellas.
#include "include/city_common.glsl"
#include "include/street_layout.glsl"

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) flat in vec3 in_normal;
layout(location = 2) flat in uvec2 in_kind_part;
layout(location = 3) flat in uint in_seed;
layout(location = 4) in vec3 in_local;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

void main()
{
    vec3 p = in_world_pos;
    vec3 n = normalize(in_normal);
    uint part = in_kind_part.y;
    vec3 emissive = vec3(0.0);
    vec3 albedo;

    if (in_kind_part.x == 0u) {
        albedo = vec3(0.04, 0.04, 0.045);
        if (part == 2u) {
            vec4 lc = unpackUnorm4x8(in_seed);
            // Downward-facing diffuser glows; the housing stays dark.
            emissive = n.z < -0.5 ? lc.rgb * 5.0 * lc.a : vec3(0.0);
        }
    } else {
        uint h = in_seed;
        vec3 clothes = mix(vec3(0.02, 0.02, 0.025), vec3(0.09, 0.07, 0.06), hash_f(h ^ 0x21u));
        albedo = part == 3u ? vec3(0.12, 0.08, 0.06) : clothes;  // head: skin in shadow
        vec3 accent = neon_color(hash_u(h ^ 0x22u));
        if (part == 2u && hash_f(h ^ 0x23u) < 0.5) {
            // Jacket LED trim: a band across the chest.
            float band = smoothstep(0.08, 0.0, abs(in_local.z - 0.35));
            emissive += accent * band * 3.0;
        }
        if (part == 3u && hash_f(h ^ 0x24u) < 0.25) {
            // Visor / mask glow on the face.
            float visor = step(0.5, in_local.x) * smoothstep(0.3, 0.0, abs(in_local.z - 0.2));
            emissive += accent * visor * 4.0;
        }
        if (part == 4u) {
            // LED umbrella: glowing rim, softly lit canopy.
            float r = max(abs(in_local.x), abs(in_local.y));
            emissive += accent * (0.35 + 4.0 * smoothstep(0.85, 1.0, r));
            albedo = vec3(0.02);
        }
    }

    // Lighting: street lamps from above (only on up-facing-ish surfaces) + ambient.
    vec3 lamp = lamp_light(p) * clamp(n.z * 0.5 + 0.5, 0.2, 1.0);
    vec3 ambient = vec3(0.02, 0.02, 0.04);
    vec3 color = albedo * (ambient + lamp) + emissive;
    out_color = vec4(apply_fog(color, p), 1.0);
    out_material = vec4(0.0, 1.0, 0.5, 0.5);
}
