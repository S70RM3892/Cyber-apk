#version 460
// 13-tap downsample (Jimenez, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare", SIGGRAPH 2014). The first pass uses a Karis average so single
// very bright pixels (neon tube cores) don't turn into flickering blobs.
layout(set = 0, binding = 1) uniform sampler2D src;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Params {
    vec2 src_texel;
    float first_pass;
    float threshold;
} params;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 karis(vec3 c) { return c / (1.0 + luma(c)); }

void main()
{
    vec2 t = params.src_texel;
    vec2 uv = in_uv;
    vec3 a = texture(src, uv + t * vec2(-2, -2)).rgb;
    vec3 b = texture(src, uv + t * vec2( 0, -2)).rgb;
    vec3 c = texture(src, uv + t * vec2( 2, -2)).rgb;
    vec3 d = texture(src, uv + t * vec2(-2,  0)).rgb;
    vec3 e = texture(src, uv).rgb;
    vec3 f = texture(src, uv + t * vec2( 2,  0)).rgb;
    vec3 g = texture(src, uv + t * vec2(-2,  2)).rgb;
    vec3 h = texture(src, uv + t * vec2( 0,  2)).rgb;
    vec3 i = texture(src, uv + t * vec2( 2,  2)).rgb;
    vec3 j = texture(src, uv + t * vec2(-1, -1)).rgb;
    vec3 k = texture(src, uv + t * vec2( 1, -1)).rgb;
    vec3 l = texture(src, uv + t * vec2(-1,  1)).rgb;
    vec3 m = texture(src, uv + t * vec2( 1,  1)).rgb;

    vec3 result;
    if (params.first_pass > 0.5) {
        vec3 g0 = karis((a + b + d + e) * 0.25);
        vec3 g1 = karis((b + c + e + f) * 0.25);
        vec3 g2 = karis((d + e + g + h) * 0.25);
        vec3 g3 = karis((e + f + h + i) * 0.25);
        vec3 g4 = karis((j + k + l + m) * 0.25);
        result = (g0 + g1 + g2 + g3) * 0.125 + g4 * 0.5;
        // Undo the Karis weighting's scale and apply a soft threshold.
        result = result / max(1.0 - luma(result), 1e-3);
        float br = luma(result);
        result *= max(br - params.threshold, 0.0) / max(br, 1e-4);
    } else {
        result = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    out_color = vec4(result, 1.0);
}
