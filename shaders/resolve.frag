#version 460
#extension GL_GOOGLE_include_directive : require
// Wet-street screen-space reflections. Only pixels the ground pass marked as
// reflective march a ray; everything else is a pass-through copy.
#include "include/frame_ubo.glsl"
#include "include/sky.glsl"

layout(set = 0, binding = 1) uniform sampler2D scene_color;
layout(set = 0, binding = 2) uniform sampler2D scene_material;
layout(set = 0, binding = 3) uniform sampler2D scene_depth;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Params {
    int max_steps;
} params;

vec3 world_from_depth(vec2 uv, float depth)
{
    vec4 p = frame.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return p.xyz / p.w;
}

float view_z(float depth) { return frame.proj[3][2] / max(depth, 1e-7); }  // znear / depth

void main()
{
    vec3 base = texture(scene_color, in_uv).rgb;
    vec4 mat = texture(scene_material, in_uv);
    float reflectivity = mat.r;
    if (reflectivity < 0.01) {
        out_color = vec4(base, 1.0);
        return;
    }

    float depth = texture(scene_depth, in_uv).r;
    vec3 p = world_from_depth(in_uv, depth);
    vec3 cam = frame.camera_pos.xyz;
    vec3 v = normalize(p - cam);
    vec3 n = normalize(vec3((mat.ba * 2.0 - 1.0), 1.0));
    vec3 r = reflect(v, n);

    // March in world space with growing steps; test against the depth buffer.
    vec3 hit_color = sky_color(r) * 0.8;
    float hit_weight = 0.0;
    float step_len = 0.35;
    vec3 ray = p + n * 0.02;
    vec3 prev = ray;
    for (int i = 0; i < params.max_steps; ++i) {
        prev = ray;
        ray += r * step_len;
        step_len *= 1.18;
        vec4 clip = frame.view_proj * vec4(ray, 1.0);
        if (clip.w <= 0.0) break;
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float scene_z = view_z(texture(scene_depth, uv).r);
        float ray_z = clip.w;
        float thickness = 0.6 + ray_z * 0.04;
        if (ray_z > scene_z && ray_z - scene_z < thickness + step_len) {
            // Binary refinement between prev and ray.
            vec3 a = prev, b = ray;
            for (int k = 0; k < 4; ++k) {
                vec3 m = (a + b) * 0.5;
                vec4 mc = frame.view_proj * vec4(m, 1.0);
                vec2 muv = mc.xy / mc.w * 0.5 + 0.5;
                if (mc.w > view_z(texture(scene_depth, muv).r)) b = m; else a = m;
            }
            vec4 bc = frame.view_proj * vec4(b, 1.0);
            vec2 huv = bc.xy / bc.w * 0.5 + 0.5;
            vec2 edge = min(huv, 1.0 - huv);
            hit_weight = smoothstep(0.0, 0.08, min(edge.x, edge.y)) * (1.0 - float(i) / float(params.max_steps));
            hit_color = mix(hit_color, texture(scene_color, huv).rgb, hit_weight);
            break;
        }
    }

    // Schlick Fresnel for water (F0 = 0.02).
    float cos_t = clamp(dot(-v, n), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - cos_t, 5.0);
    float strength = reflectivity * mix(0.3, 1.0, fresnel);
    // Rough (non-puddle) wet asphalt smears the reflection: approximate by dimming.
    strength *= mix(1.0, 0.45, smoothstep(0.05, 0.35, mat.g));
    out_color = vec4(base + hit_color * strength, 1.0);
}
