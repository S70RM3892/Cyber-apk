#version 460
#extension GL_GOOGLE_include_directive : require
// Traffic: aerial vehicles fly along the side-street grid lines (which buildings never
// occupy) in three altitude layers; ground cars drive those streets on the right.
// Position is a pure function of (instance, time, camera cell), so there is no CPU
// simulation and the traffic wraps around the player.
#include "include/city_common.glsl"
#include "include/box.glsl"
#include "include/street_layout.glsl"

layout(location = 0) out vec3 out_local;       // box space, [-1,1]^3
layout(location = 1) out vec3 out_world_pos;
layout(location = 2) flat out uint out_id;
layout(location = 3) flat out vec3 out_normal_local;

const float kSpan = 1400.0;      // metres of lane simulated around the camera
const uint kAirCount = 320u;     // instances [0, kAirCount) fly; the rest drive
const float kGroundSpan = 700.0;

void main()
{
    uint id = uint(gl_InstanceIndex);
    vec3 normal_local;
    vec3 local = box_vertex(gl_VertexIndex, normal_local);

    float t = frame.camera_pos.w;
    vec3 cam = frame.camera_pos.xyz;
    bool ground = id >= kAirCount;
    bool along_x = (id & 1u) == 0u;
    float dir = (hash_u(id ^ 0x3u) & 1u) == 0u ? 1.0 : -1.0;
    float cam_cross = along_x ? cam.y : cam.x;
    float cam_along = along_x ? cam.x : cam.y;

    float altitude, speed, lane, span;
    vec3 half_ext;
    if (ground) {
        // Side-street traffic, driving on the right of the centre line.
        altitude = 0.72;
        speed = 7.0 + 7.0 * hash_f(id ^ 0x4u);
        // Right of travel: -y when moving along +x, +x when moving along +y.
        float keep_right = along_x ? -dir : dir;
        lane = (floor(cam_cross / kBlock + 0.5) + float(int(hash_u(id ^ 0x5u) % 13u) - 6)) * kBlock + keep_right * 2.1;
        span = kGroundSpan;
        half_ext = vec3(2.2, 0.92, 0.72);
    } else {
        float layer = float(hash_u(id ^ 0x1u) % 3u);
        altitude = 42.0 + layer * 28.0 + hash_f(id ^ 0x2u) * 6.0 + sin(t * 0.8 + float(id)) * 0.4;
        speed = 22.0 + 20.0 * hash_f(id ^ 0x4u);
        lane = (floor(cam_cross / kBlock) + float(int(hash_u(id ^ 0x5u) % 21u) - 10)) * kBlock + dir * 3.0;
        span = kSpan;
        half_ext = vec3(2.4, 1.0, 0.6);
    }
    float s = hash_f(id ^ 0x6u) * span + dir * speed * t;
    float along = cam_along + (fract((s - cam_along) / span) - 0.5) * span;

    vec3 center = along_x ? vec3(along, lane, altitude) : vec3(lane, along, altitude);
    vec3 fwd = along_x ? vec3(dir, 0, 0) : vec3(0, dir, 0);
    vec3 side = vec3(-fwd.y, fwd.x, 0);
    vec3 world = center + fwd * local.x * half_ext.x + side * local.y * half_ext.y + vec3(0, 0, 1) * local.z * half_ext.z;

    out_local = local;
    out_world_pos = world;
    out_id = id;
    out_normal_local = normal_local;
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
