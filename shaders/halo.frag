#version 460
#extension GL_GOOGLE_include_directive : require
#include "include/frame_ubo.glsl"

layout(set = 0, binding = 10) uniform sampler2D scene_depth;

layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in vec3 in_color;
layout(location = 2) flat in float in_view_z;
layout(location = 3) flat in float in_radius;

layout(location = 0) out vec4 out_color;

void main()
{
    float r2 = dot(in_uv, in_uv);
    if (r2 > 1.0) discard;
    float glow = (exp(-r2 * 5.0) - exp(-5.0)) * (1.0 - r2);
    // Soft particle: fade out where the halo passes behind geometry (reversed-Z depth:
    // view z = znear / depth).
    float depth = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
    float scene_z = frame.proj[3][2] / max(depth, 1e-7);
    float fade = clamp((scene_z - in_view_z + in_radius * 0.5) / in_radius, 0.0, 1.0);
    out_color = vec4(in_color * glow * fade, 0.0);
}
