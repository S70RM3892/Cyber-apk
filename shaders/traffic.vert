#version 460
#extension GL_GOOGLE_include_directive : require
// Traffic: aerial vehicles fly along the side-street grid lines (which buildings never
// occupy) in three altitude layers; ground cars drive those streets on the right.
// Position is a pure function of (instance, time, camera cell), so there is no CPU
// simulation and the traffic wraps around the player.
#include "include/city_common.glsl"
#include "include/box.glsl"
#include "include/street_layout.glsl"
#include "include/highway.glsl"

layout(location = 0) out vec3 out_local;       // box space, [-1,1]^3
layout(location = 1) out vec3 out_world_pos;
layout(location = 2) flat out uint out_id;
layout(location = 3) flat out vec3 out_normal_local;
layout(location = 4) flat out int out_part;  // 0 body, 1 cabin

const float kSpan = 1400.0;      // metres of lane simulated around the camera
const uint kAirCount = 320u;     // instances [0, kAirCount) fly
const uint kGroundEnd = 580u;    // [kAirCount, kGroundEnd) drive side streets; the rest the expressways
const float kGroundSpan = 700.0;
const uint kPlayerCar = 1u << 20;  // instance id of the player's car (transform from the UBO)

// Split the vehicle's bounding box into a low body and a cabin set back on top.
void shape_part(int part, inout vec3 center, inout vec3 half_ext, vec3 fwd)
{
    float h = half_ext.z;
    if (part == 0) {
        center.z -= h * 0.3;
        half_ext.z = h * 0.7;
    } else {
        center += fwd * (-0.2 * half_ext.x) + vec3(0, 0, h * 0.75);
        half_ext = vec3(half_ext.x * 0.52, half_ext.y * 0.86, h * 0.42);
    }
}

void main()
{
    uint id = uint(gl_InstanceIndex);
    vec3 normal_local;
    vec3 local = box_vertex(gl_VertexIndex % 36, normal_local);
    int part = gl_VertexIndex / 36;
    out_part = part;

    float t = frame.camera_pos.w;
    vec3 cam = frame.camera_pos.xyz;
    bool ground = id >= kAirCount && id < kGroundEnd;
    bool expressway = id >= kGroundEnd;
    bool along_x = (id & 1u) == 0u;
    float dir = (hash_u(id ^ 0x3u) & 1u) == 0u ? 1.0 : -1.0;
    float cam_cross = along_x ? cam.y : cam.x;
    float cam_along = along_x ? cam.x : cam.y;

    if (id == kPlayerCar) {
        vec3 fwd = vec3(cos(frame.player_car.z), sin(frame.player_car.z), 0.0);
        vec3 side = vec3(-fwd.y, fwd.x, 0.0);
        vec3 half_ext = vec3(2.3, 0.98, 0.68);
        vec3 center = vec3(frame.player_car.xy, 0.7);
        shape_part(part, center, half_ext, fwd);
        vec3 world = center + fwd * local.x * half_ext.x + side * local.y * half_ext.y + vec3(0, 0, local.z * half_ext.z);
        out_local = local;
        out_world_pos = world;
        out_id = id;
        out_normal_local = normal_local;
        gl_Position = frame.view_proj * vec4(world, 1.0);
        return;
    }

    float altitude, speed, lane, span;
    vec3 half_ext;
    if (expressway) {
        // Two lanes each way on the nearest expressway lines.
        int family = along_x ? 1 : 0;  // family 1 runs along x
        float keep_right = along_x ? -dir : dir;
        float lane_off = (hash_u(id ^ 0x7u) & 1u) == 0u ? 1.9 : 4.1;
        altitude = kDeckHeight[family] + 0.72;
        speed = 18.0 + 12.0 * hash_f(id ^ 0x4u);
        lane = (floor(cam_cross / kHighwayEvery + 0.5) + float(int(hash_u(id ^ 0x5u) % 5u) - 2)) * kHighwayEvery +
               keep_right * lane_off;
        span = kSpan;
        half_ext = vec3(2.2, 0.92, 0.72);
    } else if (ground) {
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
    shape_part(part, center, half_ext, fwd);
    vec3 world = center + fwd * local.x * half_ext.x + side * local.y * half_ext.y + vec3(0, 0, 1) * local.z * half_ext.z;

    out_local = local;
    out_world_pos = world;
    out_id = id;
    out_normal_local = normal_local;
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
