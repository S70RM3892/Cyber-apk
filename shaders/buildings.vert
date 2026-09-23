#version 460
#extension GL_GOOGLE_include_directive : require
// Instanced building boxes. No vertex buffer: the unit box is generated from
// gl_VertexIndex (5 faces x 2 triangles; the bottom face is never visible).
#include "include/city_common.glsl"

layout(location = 0) out vec3 out_world_pos;
layout(location = 1) flat out vec3 out_normal;
layout(location = 2) flat out uint out_instance;

const vec3 kFaceNormal[5] = vec3[5](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1));
// Corner offsets per face in [-1,1]^2 x [0,1] box space, CCW seen from outside.
const vec3 kFaceCorners[20] = vec3[20](
    vec3( 1, -1, 0), vec3( 1,  1, 0), vec3( 1,  1, 1), vec3( 1, -1, 1),   // +X
    vec3(-1,  1, 0), vec3(-1, -1, 0), vec3(-1, -1, 1), vec3(-1,  1, 1),   // -X
    vec3( 1,  1, 0), vec3(-1,  1, 0), vec3(-1,  1, 1), vec3( 1,  1, 1),   // +Y
    vec3(-1, -1, 0), vec3( 1, -1, 0), vec3( 1, -1, 1), vec3(-1, -1, 1),   // -Y
    vec3(-1, -1, 1), vec3( 1, -1, 1), vec3( 1,  1, 1), vec3(-1,  1, 1));  // +Z
const int kQuadIndex[6] = int[6](0, 1, 2, 0, 2, 3);

void main()
{
    int face = gl_VertexIndex / 6;
    vec3 corner = kFaceCorners[face * 4 + kQuadIndex[gl_VertexIndex % 6]];
    Building b = buildings[gl_InstanceIndex];
    float half_size = b.pos_size.z * 0.5;
    float base_z = uintBitsToFloat(b.seed_district_flags_base.w);
    vec3 world = vec3(b.pos_size.xy + corner.xy * half_size, mix(base_z, b.pos_size.w, corner.z));

    out_world_pos = world;
    out_normal = kFaceNormal[face];
    out_instance = uint(gl_InstanceIndex);
    gl_Position = frame.view_proj * vec4(world, 1.0);
    // Buildings with a detailed mesh (detail.vert) keep their box for collision only.
    if ((b.seed_district_flags_base.z & kMeshed) != 0u) gl_Position = vec4(0.0, 0.0, -1.0, 1.0);
}
