#version 460
#extension GL_GOOGLE_include_directive : require
// Point-light sprites (aviation / warning lights). Camera-facing quads whose radius never
// drops below ~1.5 px, so distant lights stay visible instead of aliasing away.
#include "include/city_common.glsl"

layout(location = 0) out vec2 out_uv;
layout(location = 1) flat out vec3 out_color;
layout(location = 2) flat out float out_energy;  // intensity scale after enlargement

void main()
{
    const vec2 q[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = q[gl_VertexIndex];
    LightSprite ls = lights[gl_InstanceIndex];
    vec3 center = ls.pos_size.xyz;
    vec3 cam = frame.camera_pos.xyz;
    float dist = distance(center, cam);
    // proj[1][1] = cot(fov/2): pixels per radian ~ viewport.y * proj[1][1] / 2.
    float min_radius = dist * 1.6 / (frame.viewport.y * frame.proj[1][1]);
    float radius = max(ls.pos_size.w, min_radius) * 2.0;  // quad covers the halo too
    vec3 fwd = normalize(center - cam);
    vec3 right = normalize(cross(fwd, vec3(0, 0, 1)) + vec3(1e-5, 0, 0));
    vec3 up = cross(right, fwd);
    vec3 world = center + (right * c.x + up * c.y) * radius;

    float blink = ls.color_blink.w;
    float on = blink > 0.0 ? step(0.45, fract(frame.camera_pos.w * blink + hash_f(uint(gl_InstanceIndex)))) : 1.0;
    float grow = ls.pos_size.w / max(ls.pos_size.w, min_radius);
    out_energy = on * max(grow * grow, 0.2) * (1.0 - fog_amount(center));
    out_color = ls.color_blink.rgb;
    out_uv = c;
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
