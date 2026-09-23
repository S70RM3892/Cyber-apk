#version 460
#extension GL_GOOGLE_include_directive : require
// Detailed building meshes (engine/src/buildgen.cpp): plain indexed triangles.
#include "include/city_common.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_normal;   // snorm8
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uint in_building_material;

layout(location = 0) out vec3 out_world_pos;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec2 out_uv;
layout(location = 3) flat out uint out_building;
layout(location = 4) flat out uint out_material;

void main()
{
    out_world_pos = in_pos;
    out_normal = in_normal.xyz;
    out_uv = in_uv;
    out_building = in_building_material & 0xFFFFFFu;
    out_material = in_building_material >> 24u;
    gl_Position = frame.view_proj * vec4(in_pos, 1.0);
}
