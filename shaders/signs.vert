#version 460
#extension GL_GOOGLE_include_directive : require
// Neon sign quads, one instance per sign (see apex::place_signs).
#include "include/city_common.glsl"

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec3 out_world_pos;
layout(location = 2) flat out uint out_instance;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 c = corners[gl_VertexIndex];
    Sign s = signs[gl_InstanceIndex];
    float yaw = s.pos_yaw.w;
    vec3 tangent = vec3(-sin(yaw), cos(yaw), 0.0);
    vec3 world = s.pos_yaw.xyz + tangent * (c.x - 0.5) * s.size.x + vec3(0, 0, 1) * (c.y - 0.5) * s.size.y;

    out_uv = vec2(c.x, 1.0 - c.y);  // v down, like text
    out_world_pos = world;
    out_instance = uint(gl_InstanceIndex);
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
