#version 460
#extension GL_GOOGLE_include_directive : require
// Temporal anti-aliasing / accumulation. The projection is jittered by a Halton(2,3)
// sub-pixel offset every frame; this pass reprojects last frame's result through the
// depth buffer and blends it in, clipped to the current 3x3 neighbourhood in YCoCg
// (variance clipping, Salvi 2016) so moving things don't ghost. It resolves geometric
// aliasing and averages the per-pixel ray-traced shadows / AO / GI over ~10 frames.
#include "include/frame_ubo.glsl"

layout(set = 0, binding = 1) uniform sampler2D current;
layout(set = 0, binding = 2) uniform sampler2D history;
layout(set = 0, binding = 3) uniform sampler2D scene_depth;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Params {
    int reset;  // 1: no valid history (first frame, resize)
} params;

vec3 to_ycocg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 from_ycocg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Compress HDR before blending so single hot neon pixels don't dominate (Karis 2014).
vec3 compress(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 expand(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

// Catmull-Rom history fetch from 5 bilinear taps: keeps the accumulated image sharp.
vec3 sample_history(vec2 uv)
{
    vec2 size = vec2(textureSize(history, 0));
    vec2 pos = uv * size;
    vec2 c = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 tc0 = (c - 1.0) / size, tc3 = (c + 2.0) / size, tc12 = (c + w2 / w12) / size;
    vec3 r = texture(history, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y) +
             texture(history, vec2(tc0.x, tc12.y)).rgb * (w0.x * w12.y) +
             texture(history, tc12).rgb * (w12.x * w12.y) +
             texture(history, vec2(tc3.x, tc12.y)).rgb * (w3.x * w12.y) +
             texture(history, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, vec3(0.0));
}

void main()
{
    vec2 texel = frame.viewport.zw;
    vec3 cur = texture(current, in_uv).rgb;
    if (params.reset != 0) {
        out_color = vec4(cur, 1.0);
        return;
    }
    // Neighbourhood statistics (compressed YCoCg).
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    float nearest = 0.0;  // reversed-Z: largest depth = closest
    vec2 nearest_uv = in_uv;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = in_uv + vec2(x, y) * texel;
            vec3 c = to_ycocg(compress(texture(current, uv).rgb));
            m1 += c;
            m2 += c * c;
            float d = texture(scene_depth, uv).r;
            if (d > nearest) {
                nearest = d;
                nearest_uv = uv;
            }
        }
    vec3 mean = m1 / 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    vec3 lo = mean - sigma * 1.25, hi = mean + sigma * 1.25;

    // Reproject the closest surface in the neighbourhood (sharper edges on thin things).
    float depth = max(nearest, 1e-7);
    vec4 wp = frame.inv_view_proj * vec4(nearest_uv * 2.0 - 1.0, depth, 1.0);
    vec4 prev = frame.prev_view_proj * vec4(wp.xyz / wp.w, 1.0);
    vec2 puv = prev.xy / prev.w * 0.5 + 0.5 + (in_uv - nearest_uv);
    if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0))) || prev.w <= 0.0) {
        out_color = vec4(cur, 1.0);
        return;
    }
    vec3 hist = to_ycocg(compress(sample_history(puv)));
    // Clip towards the mean into the box (not just clamp): fewer colour shifts.
    vec3 centre = (lo + hi) * 0.5, extent = max((hi - lo) * 0.5, vec3(1e-4));
    vec3 offset = hist - centre;
    vec3 units = abs(offset / extent);
    float maxu = max(units.x, max(units.y, units.z));
    if (maxu > 1.0) hist = centre + offset / maxu;

    vec3 c = to_ycocg(compress(cur));
    // Faster response when the camera moved a lot (sub-pixel motion keeps it slow).
    float motion = length((puv - in_uv) / texel);
    float alpha = mix(0.08, 0.25, clamp(motion / 8.0, 0.0, 1.0));
    vec3 result = mix(hist, c, alpha);
    out_color = vec4(expand(from_ycocg(result)), 1.0);
}
