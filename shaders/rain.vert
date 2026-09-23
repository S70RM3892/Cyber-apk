#version 460
#extension GL_GOOGLE_include_directive : require
// Rain streaks: camera-facing thin quads in a box that wraps around the camera, so
// the rain is infinite without any CPU-side particles.
#include "include/city_common.glsl"

layout(location = 0) out float out_alpha;
layout(location = 1) out float out_u;

const float kBox = 36.0;        // metres, wraps around the camera
const float kFallSpeed = 14.0;  // m/s
const float kLength = 0.9;      // streak length (metres)

void main()
{
    uint id = uint(gl_InstanceIndex);
    vec3 seed = vec3(hash_f(id), hash_f(id ^ 0x1u), hash_f(id ^ 0x2u));
    float t = frame.camera_pos.w;
    vec3 cam = frame.camera_pos.xyz;

    vec3 wind = vec3(1.5, 0.8, 0.0);
    vec3 pos = seed * kBox + wind * t;
    pos.z -= t * kFallSpeed * (0.85 + 0.3 * hash_f(id ^ 0x3u));
    // Wrap into the box centred on the camera.
    pos = cam + (fract((pos - cam) / kBox + 0.5) - 0.5) * kBox;

    vec3 fall = normalize(vec3(wind.xy, -kFallSpeed));
    vec3 to_cam = normalize(cam - pos);
    vec3 side = normalize(cross(fall, to_cam)) * 0.012;

    const vec2 corners[6] = vec2[6](vec2(-1, 0), vec2(1, 0), vec2(1, 1), vec2(-1, 0), vec2(1, 1), vec2(-1, 1));
    vec2 c = corners[gl_VertexIndex];
    vec3 world = pos + side * c.x + fall * kLength * c.y;

    float dist = length(pos - cam);
    out_alpha = frame.fog.w * smoothstep(kBox * 0.5, kBox * 0.2, dist) * smoothstep(0.5, 2.0, dist);
    out_u = c.x;
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
