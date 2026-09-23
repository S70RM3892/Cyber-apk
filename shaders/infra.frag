#version 460
#extension GL_GOOGLE_include_directive : require
// Expressway shading: stained concrete, a wet reflective deck, a strip of sodium lights
// under the slab edge, red reflectors along the parapets.
#include "include/city_common.glsl"
#include "include/street_layout.glsl"

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) flat in vec3 in_normal;
layout(location = 2) in vec3 in_local;
layout(location = 3) flat in uint in_part;
layout(location = 4) flat in vec3 in_half;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

void main()
{
    vec3 p = in_world_pos;
    vec3 n = in_normal;
    float stain = 0.7 + 0.3 * value_noise(p.xy * 0.15 + p.z * 0.3);
    vec3 albedo = vec3(0.05, 0.055, 0.055) * stain;
    vec3 emissive = vec3(0.0);
    vec4 material = vec4(0.0, 1.0, 0.5, 0.5);

    if (in_part == 0u) {
        if (n.z > 0.5) {
            // Wet asphalt deck with lane dashes.
            albedo = vec3(0.02, 0.022, 0.025);
            float across = in_local.y * in_half.y;
            float along = in_local.x * in_half.x;
            float dash = step(0.5, fract(along / 6.0)) * (1.0 - smoothstep(0.06, 0.1, abs(abs(across) - 1.9)));
            albedo += dash * 0.25;
            material = vec4(0.6 * frame.fog.w, 0.15, 0.5, 0.5);
        } else if (n.z < -0.5) {
            // Underside: sodium light strips near both edges.
            float edge = abs(in_local.y);
            emissive += vec3(1.0, 0.55, 0.2) * 2.2 * smoothstep(0.86, 0.9, edge) * (1.0 - smoothstep(0.93, 0.96, edge)) *
                        step(0.3, fract(in_local.x * in_half.x / 4.0));
            albedo *= 0.6;
        } else {
            // Slab fascia: a thin light line.
            emissive += vec3(0.9, 0.95, 1.0) * 1.2 * (1.0 - smoothstep(0.05, 0.12, abs(in_local.z - 0.3)));
        }
    } else if (in_part <= 2u) {
        // Parapet: red reflectors every 4 m, lit by headlights passing by.
        float along = in_local.x * in_half.x;
        float refl = (1.0 - smoothstep(0.1, 0.18, abs(fract(along / 4.0) - 0.5) * 4.0)) *
                     (1.0 - smoothstep(0.2, 0.4, abs(in_local.z)));
        emissive += vec3(1.0, 0.05, 0.05) * 3.0 * refl * step(0.5, abs(n.x) + abs(n.y));
    }

    // Street lamps light the piers from below; a little neon spill everywhere.
    vec3 lamps = lamp_light(vec3(p.xy, max(p.z - 4.0, 0.0))) * 0.25;
    vec3 ambient = vec3(0.012, 0.018, 0.021) + vec3(0.03, 0.006, 0.008) * exp(-p.z / 20.0);
    vec3 color = albedo * (ambient * 4.0 + lamps + day_light(p, n) * 1.5) + emissive;
    out_color = vec4(apply_fog(color, p), 1.0);
    out_material = material;
}
