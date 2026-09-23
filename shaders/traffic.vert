#version 460
#extension GL_GOOGLE_include_directive : require
// Aerial traffic: vehicles fly along the side-street grid lines (which buildings never
// occupy) in three altitude layers. Position is a pure function of (instance, time,
// camera cell), so there is no CPU simulation and the traffic wraps around the player.
#include "include/city_common.glsl"

layout(location = 0) out vec3 out_local;       // box space, [-1,1]^3
layout(location = 1) out vec3 out_world_pos;
layout(location = 2) flat out uint out_id;
layout(location = 3) flat out vec3 out_normal_local;

const float kBlock = 60.0;
const float kSpan = 1400.0;  // metres of lane simulated around the camera

const vec3 kNormals[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));

vec3 corner_of(int face, int k)
{
    // Build the face's quad from its normal and two tangents; CCW seen from outside.
    vec3 nrm = kNormals[face];
    vec3 t1 = abs(nrm.z) > 0.5 ? vec3(1, 0, 0) : vec3(0, 0, 1);
    vec3 t2 = cross(nrm, t1);
    const vec2 q[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    return nrm + t1 * q[k].x + t2 * q[k].y;
}

void main()
{
    uint id = uint(gl_InstanceIndex);
    int face = gl_VertexIndex / 6;
    vec3 local = corner_of(face, gl_VertexIndex % 6);

    float t = frame.camera_pos.w;
    vec3 cam = frame.camera_pos.xyz;
    bool along_x = (id & 1u) == 0u;
    float layer = float(hash_u(id ^ 0x1u) % 3u);
    float altitude = 42.0 + layer * 28.0 + hash_f(id ^ 0x2u) * 6.0;
    float dir = (hash_u(id ^ 0x3u) & 1u) == 0u ? 1.0 : -1.0;
    float speed = 22.0 + 20.0 * hash_f(id ^ 0x4u);
    // Lane = a grid line near the camera, offset to one side by direction.
    float cam_cross = along_x ? cam.y : cam.x;
    float lane = (floor(cam_cross / kBlock) + float(int(hash_u(id ^ 0x5u) % 21u) - 10)) * kBlock + dir * 3.0;
    float cam_along = along_x ? cam.x : cam.y;
    float s = hash_f(id ^ 0x6u) * kSpan + dir * speed * t;
    float along = cam_along + (fract((s - cam_along) / kSpan) - 0.5) * kSpan;

    vec3 center = along_x ? vec3(along, lane, altitude) : vec3(lane, along, altitude);
    vec3 fwd = along_x ? vec3(dir, 0, 0) : vec3(0, dir, 0);
    vec3 side = vec3(-fwd.y, fwd.x, 0);
    vec3 half_ext = vec3(2.4, 1.0, 0.6);
    // Slight bank/bob so the traffic doesn't look rail-mounted.
    center.z += sin(t * 0.8 + float(id)) * 0.4;
    vec3 world = center + fwd * local.x * half_ext.x + side * local.y * half_ext.y + vec3(0, 0, 1) * local.z * half_ext.z;

    out_local = local;
    out_world_pos = world;
    out_id = id;
    out_normal_local = kNormals[face];
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
