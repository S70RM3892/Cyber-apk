#version 460
#extension GL_GOOGLE_include_directive : require
// One large quad covering the streamed area (the road-field window).
#include "include/city_common.glsl"

layout(location = 0) out vec3 out_world_pos;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 c = corners[gl_VertexIndex];
    vec3 world = vec3(frame.road_field.xy + c * frame.road_field.w, 0.0);
    out_world_pos = world;
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
