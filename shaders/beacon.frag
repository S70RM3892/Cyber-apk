#version 460
#extension GL_GOOGLE_include_directive : require
#include "include/frame_ubo.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in int in_part;
layout(location = 2) in vec3 in_world_pos;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;  // additive: zero keeps it

void main()
{
    const vec3 kColor = vec3(1.0, 0.85, 0.15);
    float t = frame.camera_pos.w;
    float glow;
    if (in_part == 0) {
        float x = abs(in_uv.x * 2.0 - 1.0);
        float core = exp(-x * x * 18.0) * 3.0 + exp(-x * x * 3.0) * 0.6;
        // Fade out with height and pulse slowly; scrolling bands read as "energy".
        float bands = 0.75 + 0.25 * sin(in_uv.y * 180.0 - t * 6.0);
        glow = core * bands * (1.0 - smoothstep(0.1, 1.0, in_uv.y));
    } else {
        float r = length(in_uv * 2.0 - 1.0);
        float pulse = fract(t * 0.6);
        float ring = exp(-pow((r - pulse) * 14.0, 2.0)) * (1.0 - pulse) + exp(-pow((r - 0.85) * 30.0, 2.0)) * 0.8;
        glow = ring * 2.5;
    }
    // Fog-ish fade with distance, but keep it visible far away: it's a waypoint.
    float d = length(in_world_pos - frame.camera_pos.xyz);
    glow *= mix(1.0, 0.35, smoothstep(50.0, 800.0, d));
    out_color = vec4(kColor * glow, 0.0);
    out_material = vec4(0.0);
}
