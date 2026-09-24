#version 460
#extension GL_GOOGLE_include_directive : require
// Edge-avoiding a-trous wavelet filter over the accumulated irradiance (SVGF step 2):
// a 5x5 B-spline kernel with holes of `step` pixels, weighted by depth, normal and a
// luminance term scaled by the local variance, so noise is smoothed but light edges
// (shadows, neon pools) and geometry edges survive. Run three times (step 1, 2, 4).
#include "include/frame_ubo.glsl"

layout(set = 0, binding = 1) uniform sampler2D irradiance;   // rgb, a luminance^2 moment
layout(set = 0, binding = 2) uniform sampler2D scene_depth;
layout(set = 0, binding = 3) uniform sampler2D scene_material;  // ba octahedral normal
layout(set = 0, binding = 4) uniform sampler2D history_meta;    // g sample count

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_irradiance;

layout(push_constant) uniform Params {
    int step;
} params;

float view_z(float depth) { return frame.proj[3][2] / max(depth, 1e-7); }
vec3 oct_decode(vec2 f)
{
    vec2 e = f * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float fold = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -fold : fold, n.y >= 0.0 ? -fold : fold);
    return normalize(n);
}
float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main()
{
    vec4 c = texture(irradiance, in_uv);
    float depth = texture(scene_depth, in_uv).r;
    if (depth <= 0.0) {
        out_irradiance = c;
        return;
    }
    float z = view_z(depth);
    vec3 n = oct_decode(texture(scene_material, in_uv).ba);
    float l = luminance(c.rgb);
    // Variance from the moments; young pixels (few samples) are assumed noisy.
    float samples = texture(history_meta, in_uv).g;
    float variance = max(c.a - l * l, 0.0) + (samples < 4.0 ? l * l * 0.5 : 0.0);
    float sigma_l = 4.0 * sqrt(variance) + 1e-3;

    const float kernel[3] = float[3](3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);
    vec2 texel = frame.viewport.zw * float(params.step);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            vec2 uv = in_uv + vec2(x, y) * texel;
            vec4 s = texture(irradiance, uv);
            float d = texture(scene_depth, uv).r;
            if (d <= 0.0) continue;
            float zq = view_z(d);
            vec3 nq = oct_decode(texture(scene_material, uv).ba);
            float w = kernel[abs(x)] * kernel[abs(y)];
            w *= exp(-abs(zq - z) / (0.02 * z * float(params.step) + 0.05));
            w *= pow(max(dot(n, nq), 0.0), 32.0);
            w *= exp(-abs(luminance(s.rgb) - l) / sigma_l);
            sum += s * w;
            wsum += w;
        }
    out_irradiance = wsum > 0.0 ? sum / wsum : c;
}
