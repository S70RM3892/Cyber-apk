#version 460
#extension GL_GOOGLE_include_directive : require
// Rooftop props (AC units, water tanks, masts, lattice frames), one box per instance.
#include "include/city_common.glsl"
#include "include/box.glsl"

layout(location = 0) out vec3 out_world_pos;
layout(location = 1) flat out vec3 out_normal;
layout(location = 2) out vec3 out_local;      // box space [-1,1]^3
layout(location = 3) flat out uint out_kind_seed;
layout(location = 4) flat out vec3 out_size;

void main()
{
    Prop pr = props[gl_InstanceIndex];
    vec3 nrm;
    vec3 local = box_vertex(gl_VertexIndex, nrm);
    float c = cos(pr.pos_yaw.w), s = sin(pr.pos_yaw.w);
    vec3 ax = vec3(c, s, 0.0), ay = vec3(-s, c, 0.0);
    vec3 world = pr.pos_yaw.xyz + ax * local.x * pr.size.x + ay * local.y * pr.size.y +
                 vec3(0.0, 0.0, (local.z * 0.5 + 0.5) * pr.size.z);
    out_world_pos = world;
    out_normal = ax * nrm.x + ay * nrm.y + vec3(0.0, 0.0, nrm.z);
    out_local = local;
    out_kind_seed = pr.kind_seed;
    out_size = pr.size;
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
