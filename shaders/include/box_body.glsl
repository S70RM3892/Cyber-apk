// Instanced box parts (apex::BoxInstance), shared by box_detail.vert (6 flat faces,
// 36 vertices) and box_bevel.vert (BOX_BEVEL: faces inset by a small chamfer, joined by
// edge strips and corner triangles, 132 vertices). The bevel's vertices take the normal
// of the face they belong to, so each edge shades like a rounded one and catches a
// highlight instead of reading as a razor-sharp CG corner.
#include "frame_ubo.glsl"

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

#ifdef BOX_BEVEL
const uint kBevel[132] = uint[132](
    1u, 3u, 7u, 1u, 7u, 5u, 0u, 6u, 2u, 0u, 4u, 6u, 10u, 15u, 11u, 10u, 14u, 15u, 8u, 9u, 13u, 8u,
    13u, 12u, 20u, 21u, 23u, 20u, 23u, 22u, 16u, 19u, 17u, 16u, 18u, 19u, 3u, 15u, 7u, 3u, 11u, 15u, 1u, 5u,
    13u, 1u, 13u, 9u, 2u, 6u, 14u, 2u, 14u, 10u, 0u, 12u, 4u, 0u, 8u, 12u, 5u, 7u, 23u, 5u, 23u, 21u,
    1u, 19u, 3u, 1u, 17u, 19u, 4u, 22u, 6u, 4u, 20u, 22u, 0u, 2u, 18u, 0u, 18u, 16u, 14u, 23u, 15u, 14u,
    22u, 23u, 10u, 11u, 19u, 10u, 19u, 18u, 12u, 13u, 21u, 12u, 21u, 20u, 8u, 17u, 9u, 8u, 16u, 17u, 7u, 15u,
    23u, 3u, 19u, 11u, 5u, 21u, 13u, 1u, 9u, 17u, 6u, 22u, 14u, 2u, 10u, 18u, 4u, 12u, 20u, 0u, 16u, 8u);
#endif

void main()
{
    BoxInstance b = boxes[gl_InstanceIndex];
#ifdef BOX_BEVEL
    uint code = kBevel[gl_VertexIndex];
    vec3 sgn = vec3((code & 1u) != 0u ? 1.0 : -1.0, (code & 2u) != 0u ? 1.0 : -1.0, (code & 4u) != 0u ? 1.0 : -1.0);
    int axis = int(code >> 3u);
    int face = axis == 2 ? (sgn.z > 0.0 ? 4 : 5) : axis == 0 ? (sgn.x > 0.0 ? 0 : 2) : (sgn.y > 0.0 ? 1 : 3);
    // Back side against a wall: drop every vertex on the +y face so its triangles collapse.
    if ((b.ids.z & kOpenBack) != 0u && face == 1) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0);
        return;
    }
    vec3 half_ext = vec3(b.size.xy, b.size.z * 0.5);
    float bevel = min(0.05, 0.2 * min(min(half_ext.x, half_ext.y), half_ext.z));
    vec3 inset = vec3(1.0);
    inset[axis] = 0.0;
    vec3 lc = sgn * (half_ext - bevel * inset);
    vec3 local = vec3(lc.xy, lc.z + half_ext.z);
    vec3 c = vec3(sgn.xy, sgn.z * 0.5 + 0.5);
#else
    int face = gl_VertexIndex / 6;
    if (face == 1 && (b.ids.z & kOpenBack) != 0u) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0);  // degenerate: the side against the wall
        return;
    }
    vec3 c = kCorners[face * 4 + kQuad[gl_VertexIndex % 6]];
    vec3 local = vec3(c.xy * b.size.xy, c.z * b.size.z);
#endif
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
