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

#ifdef APEX_RT
// Scene set at set 1 (renderer: resolve_rt_layout_): signs and the ray-query TLAS.
struct Sign {
    vec4 pos_yaw;
    vec2 size;
    uint seed;
    uint style;
};
layout(set = 1, binding = 2, std430) readonly buffer Signs { Sign signs[]; };
layout(set = 1, binding = 15) uniform accelerationStructureEXT scene_tlas;

uint hash_u(uint v)  // city_common.glsl
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
vec3 zone_color(uint zone, uint h)  // signs_common.glsl
{
    const vec3 z[8] = vec3[8](vec3(1.0, 0.07, 0.1), vec3(0.05, 0.85, 1.0), vec3(1.0, 0.85, 0.15), vec3(0.95, 0.1, 0.9),
                              vec3(1.0, 0.12, 0.45), vec3(0.25, 0.45, 1.0), vec3(1.0, 0.45, 0.06), vec3(0.15, 0.95, 0.8));
    uint r = h & 7u;
    if (r == 7u) return vec3(1.0, 0.82, 0.68);
    return z[(zone & 3u) * 2u + (r < 4u ? 0u : 1u)];
}

// What a reflection ray that left the screen really sees: another building (dark), a
// sign (its neon, averaged over the face) or open sky.
vec3 traced_reflection(vec3 origin, vec3 dir)
{
    const float kMax = 180.0;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene_tlas, gl_RayFlagsOpaqueEXT, 0x03u, origin, 0.05, dir, kMax);
    rayQueryProceedEXT(q);
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) return sky_color(dir) * 0.8;
    float t = rayQueryGetIntersectionTEXT(q, true);
    float fade = exp(-t * 0.006);  // through the smog
    if (rayQueryGetIntersectionInstanceCustomIndexEXT(q, true) == 1) {
        Sign s = signs[rayQueryGetIntersectionPrimitiveIndexEXT(q, true) / 2];
        uint style = s.style & 0xFFu, zone = (s.style >> 16u) & 0xFFu;
        vec3 c = zone_color(zone, hash_u(s.seed));
        float level = style == 4u ? 1.6 : style == 1u ? 2.2 : style == 3u ? 1.5 : 1.8;  // neon text is mostly gaps
        if (style == 4u) c = zone_color(zone, 0u);
        return c * level * fade + sky_color(dir) * 0.8 * (1.0 - fade);
    }
    // Façade: mostly dark with scattered lit windows.
    return sky_color(dir) * 0.8 * (1.0 - fade) + vec3(0.02, 0.025, 0.03) * fade;
}
#endif

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
    vec3 ssr_color = vec3(0.0);
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
            // Rough wet asphalt smears the reflection; puddles stay sharp. Blur by
            // spreading 4 taps over a footprint that grows with roughness and distance.
            float spread = mat.g * mat.g * 0.08 * min(float(i) + 1.0, 12.0) / 12.0;
            vec3 refl = texture(scene_color, huv).rgb;
            if (spread > 0.002) {
                vec2 o = vec2(spread, spread * frame.viewport.x * frame.viewport.w);
                refl = (texture(scene_color, huv + vec2(o.x, 0.3 * o.y)).rgb + texture(scene_color, huv - vec2(o.x, 0.3 * o.y)).rgb +
                        texture(scene_color, huv + vec2(-0.3 * o.x, o.y)).rgb + texture(scene_color, huv + vec2(0.3 * o.x, -o.y)).rgb +
                        refl * 2.0) / 6.0;
            }
            ssr_color = refl;
            hit_color = mix(hit_color, refl, hit_weight);
            break;
        }
    }

#ifdef APEX_RT
    // Off screen or behind something: fall back to a hardware ray instead of the sky.
    if (hit_weight < 0.99) hit_color = mix(traced_reflection(p + n * 0.05, r), ssr_color, hit_weight);
#endif

    // Schlick Fresnel for water (F0 = 0.02).
    float cos_t = clamp(dot(-v, n), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - cos_t, 5.0);
    float strength = reflectivity * mix(0.3, 1.0, fresnel);
    // Rough (non-puddle) wet asphalt smears the reflection: approximate by dimming.
    strength *= mix(1.0, 0.55, smoothstep(0.05, 0.5, mat.g));
    out_color = vec4(base + hit_color * strength, 1.0);
}
