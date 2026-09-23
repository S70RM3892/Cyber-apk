#version 460
#extension GL_GOOGLE_include_directive : require
// Instanced box parts (apex::BoxInstance): relief piers and bands, balconies, AC units,
// pods... Expands 6 faces x 2 triangles from gl_VertexIndex and feeds detail.frag the
// same varyings as the triangulated meshes (u = face * 256 + metres from the face centre,
// v = z), so they shade identically.
#include "include/frame_ubo.glsl"

struct BoxInstance {
    vec4 pos_yaw;       // base centre, yaw
    vec4 size;          // half x, half y, height, -
    uvec4 ids;          // building, materials (side | top << 8 | bottom << 16), flags, -
};
layout(set = 0, binding = 14, std430) readonly buffer Boxes { BoxInstance boxes[]; };

layout(location = 0) out vec3 out_world_pos;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec2 out_uv;
layout(location = 3) flat out uint out_building;
layout(location = 4) flat out uint out_material;

// Corners per face in local units (x, y in -1..1, z in 0..1), counter-clockwise from
// outside; faces 0..3 follow the edges of Builder::rect (+x, +y, -x, -y), then top, bottom.
const vec3 kCorners[24] = vec3[24](
    vec3( 1, -1, 0), vec3( 1,  1, 0), vec3( 1,  1, 1), vec3( 1, -1, 1),
    vec3( 1,  1, 0), vec3(-1,  1, 0), vec3(-1,  1, 1), vec3( 1,  1, 1),
    vec3(-1,  1, 0), vec3(-1, -1, 0), vec3(-1, -1, 1), vec3(-1,  1, 1),
    vec3(-1, -1, 0), vec3( 1, -1, 0), vec3( 1, -1, 1), vec3(-1, -1, 1),
    vec3(-1, -1, 1), vec3( 1, -1, 1), vec3( 1,  1, 1), vec3(-1,  1, 1),
    vec3(-1,  1, 0), vec3( 1,  1, 0), vec3( 1, -1, 0), vec3(-1, -1, 0));
const vec3 kNormals[6] = vec3[6](vec3(1, 0, 0), vec3(0, 1, 0), vec3(-1, 0, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
const int kQuad[6] = int[6](0, 1, 2, 0, 2, 3);
const uint kOpenBack = 1u;

void main()
{
    int face = gl_VertexIndex / 6;
    BoxInstance b = boxes[gl_InstanceIndex];
    if (face == 1 && (b.ids.z & kOpenBack) != 0u) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0);  // degenerate: the side against the wall
        return;
    }
    vec3 c = kCorners[face * 4 + kQuad[gl_VertexIndex % 6]];
    vec3 local = vec3(c.xy * b.size.xy, c.z * b.size.z);
    vec2 ax = vec2(cos(b.pos_yaw.w), sin(b.pos_yaw.w));
    vec2 ay = vec2(-ax.y, ax.x);
    vec3 world = vec3(b.pos_yaw.xy + ax * local.x + ay * local.y, b.pos_yaw.z + local.z);
    vec3 ln = kNormals[face];
    out_normal = vec3(ax * ln.x + ay * ln.y, ln.z);
    // Along-face coordinate from the face centre, in the direction of its bottom edge.
    float along = face == 0 ? local.y : face == 1 ? -local.x : face == 2 ? -local.y : local.x;
    out_uv = vec2(float(min(face, 3)) * 256.0 + along, world.z);
    uint mats = b.ids.y;
    out_material = face == 4 ? (mats >> 8u) & 0xFFu : face == 5 ? (mats >> 16u) & 0xFFu : mats & 0xFFu;
    out_building = b.ids.x;
    out_world_pos = world;
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
