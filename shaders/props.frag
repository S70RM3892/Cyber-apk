#version 460
#extension GL_GOOGLE_include_directive : require
#include "include/city_common.glsl"
#include "include/street_layout.glsl"

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) flat in vec3 in_normal;
layout(location = 2) in vec3 in_local;
layout(location = 3) flat in uint in_kind_seed;
layout(location = 4) flat in vec3 in_size;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

const uint kAc = 0u, kTank = 1u, kMast = 2u, kFrame = 3u;

void main()
{
    uint kind = in_kind_seed & 0xFFu;
    uint seed = in_kind_seed >> 8u;
    vec3 n = in_normal;
    vec3 l = in_local;
    vec3 albedo = vec3(0.05, 0.055, 0.06) * (0.8 + 0.4 * hash_f(seed));
    vec3 emissive = vec3(0.0);

    if (kind == kAc) {
        if (n.z > 0.5) {
            // Fan grille on top.
            float r = length(l.xy * vec2(in_size.x, in_size.y) - vec2(in_size.x * 0.35, 0.0));
            albedo *= mix(1.0, 0.35, smoothstep(0.42, 0.38, r)) * (0.85 + 0.15 * sin(r * 60.0));
        } else {
            albedo *= 0.8 + 0.2 * step(0.5, fract(l.z * 6.0));  // side louvres
        }
        albedo *= 1.4;
    } else if (kind == kTank) {
        // Fake cylinder: darken towards the silhouette of the box.
        float edge = max(abs(l.x), abs(l.y));
        albedo = vec3(0.07, 0.06, 0.05) * (1.0 - 0.5 * smoothstep(0.6, 1.0, edge)) * (0.85 + 0.15 * step(0.5, fract(l.z * 3.0)));
    } else if (kind == kFrame) {
        // Lattice: keep only the members (edges + diagonals) of each 1.5 m bay.
        vec2 face = abs(n.x) > 0.5 ? vec2(l.y * in_size.y, (l.z * 0.5 + 0.5) * in_size.z)
                                   : vec2(l.x * in_size.x, (l.z * 0.5 + 0.5) * in_size.z);
        vec2 bay = fract(face / 1.5);
        float member = min(min(bay.x, 1.0 - bay.x), min(bay.y, 1.0 - bay.y));
        float diag = min(abs(bay.x - bay.y), abs(bay.x + bay.y - 1.0)) * 0.7071;
        float w = 0.06 + fwidth(face.x) * 0.75;
        if (min(member, diag) > w && n.z < 0.5) discard;
        albedo = vec3(0.035, 0.035, 0.04);
    }

    vec3 lamps = lamp_light(in_world_pos) * 0.15 * clamp(n.z * 0.5 + 0.5, 0.2, 1.0);
    vec3 ambient = vec3(0.012, 0.018, 0.022) + vec3(0.03, 0.006, 0.008);
    vec3 color = albedo * (ambient * 3.0 + lamps) + emissive;
    out_color = vec4(apply_fog(color, in_world_pos), 1.0);
    out_material = vec4(0.0, 1.0, 0.5, 0.5);
}
