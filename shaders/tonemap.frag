#version 460
// Final composite at output resolution: bilinear upscale of the internal-resolution
// HDR image (DRS; a temporal/neural upscaler replaces this later), bloom, ACES tone
// curve, and the lens character of the genre (slight chromatic aberration,
// vignette, grain).
layout(set = 0, binding = 1) uniform sampler2D hdr;
layout(set = 0, binding = 2) uniform sampler2D bloom;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Params {
    float exposure;
    float bloom_strength;
    float time;
    float output_is_srgb;  // 1: hardware sRGB encode, 0: encode here
    int pre_rotation;      // 0, 90, 180, 270: surface transform the swapchain was created with
} params;

// Narkowicz 2015 ACES filmic fit.
vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 srgb_encode(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

float grain_hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Swapchain pixel -> logical image uv. Android pre-rotation: the scene was rendered
// upright, and this pass writes it rotated so the compositor doesn't have to
// (developer.android.com/games/optimize/vulkan-prerotation). Physical clip space is
// R(angle) * logical clip space, so apply the inverse rotation here.
vec2 logical_uv(vec2 physical_uv)
{
    vec2 p = physical_uv * 2.0 - 1.0;
    vec2 l = p;
    if (params.pre_rotation == 90) l = vec2(p.y, -p.x);
    else if (params.pre_rotation == 180) l = -p;
    else if (params.pre_rotation == 270) l = vec2(-p.y, p.x);
    return l * 0.5 + 0.5;
}

void main()
{
    vec2 uv = logical_uv(in_uv);
    vec2 centered = uv - 0.5;
    float r2 = dot(centered, centered);

    // Chromatic aberration grows towards the corners.
    vec2 ca = centered * r2 * 0.012;
    vec3 c;
    c.r = texture(hdr, uv - ca).r;
    c.g = texture(hdr, uv).g;
    c.b = texture(hdr, uv + ca).b;

    c += texture(bloom, uv).rgb * params.bloom_strength;
    c *= params.exposure;

    // Grade: teal shadows, warm highlights, a little extra saturation.
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c *= mix(vec3(0.92, 1.0, 1.03), vec3(1.07, 0.97, 0.93), smoothstep(0.02, 0.7, l));
    c = max(mix(vec3(l), c, 1.18), vec3(0.0));
    c = aces(c);

    c *= mix(1.0, 0.55, smoothstep(0.15, 0.75, r2 * 2.0));  // vignette
    float g = grain_hash(gl_FragCoord.xy + fract(params.time) * 1000.0) - 0.5;
    c += g * 0.018;

    if (params.output_is_srgb < 0.5) c = srgb_encode(clamp(c, 0.0, 1.0));
    out_color = vec4(c, 1.0);
}
