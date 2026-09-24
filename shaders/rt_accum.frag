#version 460
#extension GL_GOOGLE_include_directive : require
// Temporal accumulation of the ray-traced irradiance (SVGF step 1, Schied et al. 2017):
// reproject last frame's history through the depth buffer, keep it only where the
// surface is the same (depth test), and average with an exponential window that starts
// as a plain mean so a fresh pixel converges quickly. Also tracks the second moment of
// luminance, which the a-trous filter turns into a variance estimate.
#include "include/frame_ubo.glsl"

layout(set = 0, binding = 1) uniform sampler2D current;     // rt_light irradiance
layout(set = 0, binding = 2) uniform sampler2D history;     // rgb irradiance, a luminance^2 moment
layout(set = 0, binding = 3) uniform sampler2D history_meta;  // r linear depth, g sample count
layout(set = 0, binding = 4) uniform sampler2D scene_depth;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_history;
layout(location = 1) out vec4 out_meta;

layout(push_constant) uniform Params {
    int reset;
} params;

float view_z(float depth) { return frame.proj[3][2] / max(depth, 1e-7); }

void main()
{
    vec3 cur = texture(current, in_uv).rgb;
    float depth = texture(scene_depth, in_uv).r;
    float z = view_z(depth);
    float lum = dot(cur, vec3(0.2126, 0.7152, 0.0722));
    out_history = vec4(cur, lum * lum);
    out_meta = vec4(z, 1.0, 0.0, 0.0);
    if (params.reset != 0 || depth <= 0.0) return;

    vec4 wp = frame.inv_view_proj * vec4(in_uv * 2.0 - 1.0, depth, 1.0);
    vec4 prev = frame.prev_view_proj * vec4(wp.xyz / wp.w, 1.0);
    if (prev.w <= 0.0) return;
    vec2 puv = prev.xy / prev.w * 0.5 + 0.5;
    if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) return;
    vec2 meta = textureLod(history_meta, puv, 0.0).rg;
    // Same surface? Previous linear depth vs where this point was then.
    if (abs(meta.r - prev.w) > 0.04 * prev.w + 0.05) return;
    vec4 h = textureLod(history, puv, 0.0);
    float n = min(meta.g + 1.0, 24.0);
    float a = 1.0 / n;
    out_history = vec4(mix(h.rgb, cur, a), mix(h.a, lum * lum, a));
    out_meta = vec4(z, n, 0.0, 0.0);
}
