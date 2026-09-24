// Local lights: neon signs, shopfronts, lit canopies (apex::PointLight), binned on the
// CPU into a kLightGridSize^2 grid over the road-field square (apex::build_light_grid).
#ifndef APEX_LIGHTING_GLSL
#define APEX_LIGHTING_GLSL

#include "frame_ubo.glsl"

#ifndef APEX_SCENE_SET
#define APEX_SCENE_SET 0  // the scene descriptor set (1 in the ray-traced lighting pass)
#endif

struct PointLight {
    vec4 pos_radius;
    vec4 color;  // rgb intensity, w source radius (metres, soft shadows)
};
layout(set = APEX_SCENE_SET, binding = 8, std430) readonly buffer PointLights { PointLight point_lights[]; };
layout(set = APEX_SCENE_SET, binding = 9, std430) readonly buffer LightGrid { uint light_grid[]; };

const int kLightGridSize = 128;  // world.hpp

#ifdef APEX_RT
layout(set = APEX_SCENE_SET, binding = 15) uniform accelerationStructureEXT scene_tlas;
const uint kRtMaskWorld = 0x01u, kRtMaskSigns = 0x02u;  // rt_scene.hpp

// 1 if nothing opaque lies between `from` and `to` (hardware ray query, any hit ends it).
float rt_visible(vec3 from, vec3 to)
{
    vec3 d = to - from;
    float len = length(d);
    if (len < 0.05) return 1.0;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, kRtMaskWorld,
                          from, 0.0, d / len, len);
    rayQueryProceedEXT(q);
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
const int kShadowRays = 4;  // lights per pixel that get a shadow ray (the strongest)

// Per-pixel rotation for the few rays we can afford: interleaved gradient noise
// (Jimenez 2014), varied per frame and averaged by TAA.
float rt_noise(float k)
{
    // Shifted every frame (Jimenez's temporal IGN) so TAA averages the samples.
    vec2 f = gl_FragCoord.xy + vec2(k * 5.588238, k * 3.1) + 5.588238 * mod(frame.taa.z, 64.0);
    return fract(52.9829189 * fract(dot(f, vec2(0.06711056, 0.00583715))));
}

// Short-range ambient occlusion: 4 rays over the hemisphere, 2.5 m. 1 = open, 0 = buried.
float rt_ambient_occlusion(vec3 p, vec3 n)
{
    vec3 t = normalize(abs(n.z) < 0.9 ? cross(n, vec3(0.0, 0.0, 1.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, t);
    float rot = rt_noise(0.0) * 6.2831853;
    vec3 origin = p + n * 0.05;
    const float kRange = 2.5;
    float open = 0.0;
    for (int i = 0; i < 4; ++i) {
        float a = rot + float(i) * 1.5707963;
        float r = sqrt((float(i) + 0.5) / 4.0);           // cosine-weighted rings
        vec3 d = normalize(t * (cos(a) * r) + b * (sin(a) * r) + n * sqrt(1.0 - r * r));
        rayQueryEXT q;
        rayQueryInitializeEXT(q, scene_tlas, gl_RayFlagsOpaqueEXT, kRtMaskWorld, origin, 0.0, d, kRange);
        rayQueryProceedEXT(q);
        float hit = rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT
                        ? kRange : rayQueryGetIntersectionTEXT(q, true);
        open += hit / kRange;  // near hits darken more
    }
    return open * 0.25;
}
#endif

// Diffuse irradiance and a normalised Blinn-Phong specular from the lights around p.
// view_dir points from the camera to p. With ray queries the strongest lights are
// occlusion-tested per pixel and the rest take their average visibility, so neon only
// reaches what it can actually see: walls facing away across a building stay dark,
// awnings and ledges cast hard shadows.
void local_lights(vec3 p, vec3 n, vec3 view_dir, float shininess, out vec3 diffuse, out vec3 specular)
{
    diffuse = vec3(0.0);
    specular = vec3(0.0);
    vec2 g = (p.xy - frame.road_field.xy) / frame.road_field.w * float(kLightGridSize);
    if (any(lessThan(g, vec2(0.0))) || any(greaterThanEqual(g, vec2(float(kLightGridSize))))) return;
    uint cell = uint(g.y) * uint(kLightGridSize) + uint(g.x);
    uint first = light_grid[cell * 2u];
    uint count = light_grid[cell * 2u + 1u];
    float spec_norm = (shininess + 8.0) / 25.13;
#ifdef APEX_RT
    uint top_i[kShadowRays];
    float top_w[kShadowRays];
    vec3 top_d[kShadowRays], top_s[kShadowRays];
    for (int k = 0; k < kShadowRays; ++k) {
        top_i[k] = 0xFFFFFFFFu;
        top_w[k] = 0.0;
        top_d[k] = vec3(0.0);
        top_s[k] = vec3(0.0);
    }
#endif
    for (uint i = 0u; i < count; ++i) {
        uint li = light_grid[first + i];
        PointLight l = point_lights[li];
        vec3 d = l.pos_radius.xyz - p;
        float d2 = dot(d, d);
        float r2 = l.pos_radius.w * l.pos_radius.w;
        if (d2 >= r2) continue;
        float window = 1.0 - d2 / r2;
        window *= window;
        vec3 ld = d * inversesqrt(max(d2, 1e-4));
        float ndl = dot(n, ld);
        // Slight wrap: large area lights still graze surfaces at right angles to them.
        float wrap = max((ndl + 0.15) / 1.15, 0.0);
        vec3 e = l.color.rgb * window / (d2 + 1.0);
        vec3 dc = e * wrap;
        vec3 h = normalize(ld - view_dir);
        vec3 sc = e * max(ndl, 0.0) * pow(max(dot(n, h), 0.0), shininess) * spec_norm;
        diffuse += dc;
        specular += sc;
#ifdef APEX_RT
        // Keep the strongest few (insertion into a tiny sorted list).
        float w = dot(dc + sc, vec3(0.3, 0.5, 0.2));
        if (w > top_w[kShadowRays - 1]) {
            int k = kShadowRays - 1;
            while (k > 0 && top_w[k - 1] < w) {
                top_i[k] = top_i[k - 1]; top_w[k] = top_w[k - 1]; top_d[k] = top_d[k - 1]; top_s[k] = top_s[k - 1];
                --k;
            }
            top_i[k] = li; top_w[k] = w; top_d[k] = dc; top_s[k] = sc;
        }
#endif
    }
#ifdef APEX_RT
    // Trace the strongest; the remainder is scaled by their mean visibility.
    vec3 origin = p + n * 0.06;
    float vis_sum = 0.0, traced = 0.0;
    vec3 fix_d = vec3(0.0), fix_s = vec3(0.0), top_sum_d = vec3(0.0), top_sum_s = vec3(0.0);
    for (int k = 0; k < kShadowRays; ++k) {
        if (top_i[k] == 0xFFFFFFFFu) break;
        PointLight l = point_lights[top_i[k]];
        // Area light: aim at a point jittered over the source (signs are metres wide), so
        // shadows get penumbrae. Stop short of it: it hangs just in front of its sign.
        float ja = rt_noise(float(k) + 1.0) * 6.2831853, jr = sqrt(rt_noise(float(k) + 7.0));
        vec3 ax = normalize(cross(l.pos_radius.xyz - origin, vec3(0.0, 0.0, 1.0)) + vec3(1e-4));
        vec3 lp = l.pos_radius.xyz + (ax * cos(ja) + vec3(0.0, 0.0, 1.0) * sin(ja)) * jr * l.color.w;
        vec3 to = lp - normalize(lp - origin) * 0.25;
        float v = rt_visible(origin, to);
        vis_sum += v;
        traced += 1.0;
        fix_d += top_d[k] * v;
        fix_s += top_s[k] * v;
        top_sum_d += top_d[k];
        top_sum_s += top_s[k];
    }
    if (traced > 0.0) {
        float mean = vis_sum / traced;
        diffuse = fix_d + (diffuse - top_sum_d) * mean;
        specular = fix_s + (specular - top_sum_s) * mean;
    }
#endif
}

#endif
