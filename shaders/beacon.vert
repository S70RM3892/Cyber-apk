#version 460
#extension GL_GOOGLE_include_directive : require
// Gig beacon: a camera-facing pillar of light (vertices 0-5) and a pulsing ring on
// the ground (vertices 6-11) at frame.objective. Drawn additively after opaque geometry.
#include "include/frame_ubo.glsl"

layout(location = 0) out vec2 out_uv;
layout(location = 1) flat out int out_part;
layout(location = 2) out vec3 out_world_pos;

const float kPillarRadius = 1.6;
const float kPillarHeight = 420.0;
const float kRingRadius = 6.0;

void main()
{
    const vec2 q[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    int part = gl_VertexIndex / 6;
    vec2 c = q[gl_VertexIndex % 6];
    vec3 base = vec3(frame.objective.xy, 0.0);
    vec3 world;
    if (part == 0) {
        vec3 to_cam = frame.camera_pos.xyz - base;
        vec3 side = normalize(vec3(-to_cam.y, to_cam.x, 0.0) + vec3(1e-4, 0.0, 0.0));
        // Grow with distance so the pillar keeps a readable on-screen width.
        float radius = max(kPillarRadius, length(to_cam.xy) * 0.012);
        world = base + side * (c.x * 2.0 - 1.0) * radius + vec3(0, 0, c.y * kPillarHeight);
    } else {
        world = base + vec3((c * 2.0 - 1.0) * kRingRadius, 0.05);
    }
    out_uv = c;
    out_part = part;
    out_world_pos = world;
    gl_Position = frame.objective.w > 0.5 ? frame.view_proj * vec4(world, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
}
