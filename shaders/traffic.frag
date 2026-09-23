#version 460
#extension GL_GOOGLE_include_directive : require
// Vehicle shading: dark body, white headlights, red tail lights, neon underglow,
// cyan cabin strip.
#include "include/city_common.glsl"

layout(location = 0) in vec3 in_local;
layout(location = 1) in vec3 in_world_pos;
layout(location = 2) flat in uint in_id;
layout(location = 3) flat in vec3 in_normal_local;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

void main()
{
    vec3 l = in_local;
    vec3 nl = in_normal_local;
    vec3 body = vec3(0.02, 0.02, 0.025);
    vec3 e = vec3(0.0);

    float lamp_y = smoothstep(0.75, 0.55, abs(abs(l.y) - 0.55));
    if (nl.x > 0.5) e += vec3(1.0, 0.95, 0.85) * 20.0 * lamp_y * step(abs(l.z), 0.4);    // headlights
    if (nl.x < -0.5) e += vec3(1.0, 0.05, 0.03) * 14.0 * step(abs(l.z), 0.35);          // tail bar
    if (nl.z < -0.5) e += neon_color(hash_u(in_id)) * 3.0 * smoothstep(1.0, 0.4, abs(l.y));  // underglow
    if (abs(nl.y) > 0.5) e += vec3(0.1, 0.8, 1.0) * 1.5 * step(0.2, l.z) * step(abs(l.x), 0.6); // cabin strip

    out_color = vec4(apply_fog(body + e, in_world_pos), 1.0);
    out_material = vec4(0.0, 1.0, 0.5, 0.5);
}
