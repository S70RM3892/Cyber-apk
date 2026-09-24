#version 460
#extension GL_GOOGLE_include_directive : require
// Ray-traced lighting (ray-query devices). Reads the scene pass G-buffer and writes this
// frame's noisy estimate of
//   irradiance  direct neon / shopfront light with ray-traced visibility, picked by
//               resampled importance sampling from the light grid (RIS, the candidate step
//               of ReSTIR, Bitterli et al. 2020), plus one bounce of indirect light: one
//               cosine-distributed ray that sees open sky, a sign's neon or a lit wall
//   specular    highlights of the sampled lights
// rt_accum / rt_atrous denoise it (SVGF-style, Schied et al. 2017), rt_composite adds
// albedo * irradiance + specular into the scene.
#define APEX_SCENE_SET 1
#include "include/lighting.glsl"
#include "include/sky.glsl"

layout(set = 0, binding = 1) uniform sampler2D scene_material;  // r refl, g rough, ba octahedral normal
layout(set = 0, binding = 2) uniform sampler2D scene_albedo;    // rgb albedo, a packed specular
layout(set = 0, binding = 3) uniform sampler2D scene_depth;

struct Sign {
    vec4 pos_yaw;
    vec2 size;
    uint seed;
    uint style;
};
layout(set = 1, binding = 2, std430) readonly buffer Signs { Sign signs[]; };

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_irradiance;
layout(location = 1) out vec4 out_specular;

const vec3 kSunColor = vec3(1.0, 0.58, 0.32);  // city_common.glsl

uint hash_u(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
vec3 zone_color(uint zone, uint h)
{
    const vec3 z[8] = vec3[8](vec3(1.0, 0.07, 0.1), vec3(0.05, 0.85, 1.0), vec3(1.0, 0.85, 0.15), vec3(0.95, 0.1, 0.9),
                              vec3(1.0, 0.12, 0.45), vec3(0.25, 0.45, 1.0), vec3(1.0, 0.45, 0.06), vec3(0.15, 0.95, 0.8));
    uint r = h & 7u;
    if (r == 7u) return vec3(1.0, 0.82, 0.68);
    return z[(zone & 3u) * 2u + (r < 4u ? 0u : 1u)];
}

vec3 oct_decode(vec2 f)
{
    vec2 e = f * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float fold = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -fold : fold, n.y >= 0.0 ? -fold : fold);
    return normalize(n);
}

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Unshadowed contribution of light i at p (diffuse irradiance, specular radiance).
void light_terms(uint i, vec3 p, vec3 n, vec3 view_dir, float shininess, out vec3 dif, out vec3 spc)
{
    PointLight l = point_lights[i];
    vec3 d = l.pos_radius.xyz - p;
    float d2 = dot(d, d);
    float r2 = l.pos_radius.w * l.pos_radius.w;
    dif = vec3(0.0);
    spc = vec3(0.0);
    if (d2 >= r2) return;
    float window = 1.0 - d2 / r2;
    window *= window;
    vec3 ld = d * inversesqrt(max(d2, 1e-4));
    float ndl = dot(n, ld);
    float wrap = max((ndl + 0.15) / 1.15, 0.0);
    vec3 e = l.color.rgb * window / (d2 + 1.0);
    dif = e * wrap;
    vec3 h = normalize(ld - view_dir);
    spc = e * max(ndl, 0.0) * pow(max(dot(n, h), 0.0), shininess) * (shininess + 8.0) / 25.13;
}

// Direct light: two independent RIS reservoirs over up to 8 uniform candidates each,
// one shadow ray per reservoir aimed at a point jittered over the light's area.
void direct_light(vec3 p, vec3 n, vec3 view_dir, float shininess, float spec_w, out vec3 dif_out, out vec3 spc_out)
{
    dif_out = vec3(0.0);
    spc_out = vec3(0.0);
    vec2 g = (p.xy - frame.road_field.xy) / frame.road_field.w * float(kLightGridSize);
    if (any(lessThan(g, vec2(0.0))) || any(greaterThanEqual(g, vec2(float(kLightGridSize))))) return;
    uint cell = uint(g.y) * uint(kLightGridSize) + uint(g.x);
    uint first = light_grid[cell * 2u];
    uint count = light_grid[cell * 2u + 1u];
    if (count == 0u) return;
    vec3 origin = p + n * 0.06;
    const int kReservoirs = 2;
    uint m = min(count, 8u);
    for (int r = 0; r < kReservoirs; ++r) {
        float wsum = 0.0, chosen_w = 0.0;
        uint chosen = 0xFFFFFFFFu;
        vec3 chosen_d = vec3(0.0), chosen_s = vec3(0.0);
        uint start = uint(rt_noise(float(r) * 13.0 + 3.0) * float(count));
        uint stride = count > m ? count / m : 1u;
        for (uint k = 0u; k < m; ++k) {
            uint li = light_grid[first + (start + k * stride) % count];
            vec3 dif, spc;
            light_terms(li, p, n, view_dir, shininess, dif, spc);
            float target = luminance(dif * 0.6) + luminance(spc) * spec_w;
            float w = target * float(count);  // p_hat / p_source, p_source = 1 / count
            wsum += w;
            if (w > 0.0 && rt_noise(float(r) * 7.0 + float(k) + 20.0) * wsum < w) {
                chosen = li;
                chosen_w = target;
                chosen_d = dif;
                chosen_s = spc;
            }
        }
        if (chosen == 0xFFFFFFFFu || chosen_w <= 0.0) continue;
        float W = wsum / (float(m) * chosen_w);
        PointLight l = point_lights[chosen];
        float ja = rt_noise(float(r) + 31.0) * 6.2831853, jr = sqrt(rt_noise(float(r) + 37.0));
        vec3 ax = normalize(cross(l.pos_radius.xyz - origin, vec3(0.0, 0.0, 1.0)) + vec3(1e-4));
        vec3 lp = l.pos_radius.xyz + (ax * cos(ja) + vec3(0.0, 0.0, 1.0) * sin(ja)) * jr * l.color.w;
        float vis = rt_visible(origin, lp - normalize(lp - origin) * 0.25);
        dif_out += chosen_d * vis * W;
        spc_out += chosen_s * vis * W;
    }
    dif_out /= float(kReservoirs);
    spc_out /= float(kReservoirs);
}

// Radiance leaving a surface a bounce ray hit: its neon if it's a sign, else a rough
// estimate of a wall lit by the lights around it (no shadows at the second bounce).
vec3 bounce_radiance(vec3 hit, vec3 dir, bool sign, uint prim)
{
    if (sign) {
        Sign s = signs[prim / 2u];
        uint style = s.style & 0xFFu, zone = (s.style >> 16u) & 0xFFu;
        vec3 c = style == 4u ? zone_color(zone, 0u) : zone_color(zone, hash_u(s.seed));
        // Average radiance over the face: neon letters are mostly gaps, screens full.
        float level = style == 4u ? 1.2 : style == 3u ? 1.6 : style == 1u ? 1.8 : 1.4;
        return c * level;
    }
    vec3 n = hit.z < 0.05 ? vec3(0.0, 0.0, 1.0) : -dir;  // the street, or a wall facing the ray
    vec2 g = (hit.xy - frame.road_field.xy) / frame.road_field.w * float(kLightGridSize);
    vec3 e = vec3(0.0);
    if (all(greaterThanEqual(g, vec2(0.0))) && all(lessThan(g, vec2(float(kLightGridSize))))) {
        uint cell = uint(g.y) * uint(kLightGridSize) + uint(g.x);
        uint first = light_grid[cell * 2u];
        uint count = min(light_grid[cell * 2u + 1u], 6u);
        for (uint k = 0u; k < count; ++k) {
            vec3 dif, spc;
            light_terms(light_grid[first + k], hit, n, dir, 8.0, dif, spc);
            e += dif;
        }
    }
    const float kWallAlbedo = 0.12;
    // Lit windows and small signage average out to a faint glow of their own.
    return e * 0.6 * kWallAlbedo / 3.14159 + vec3(0.012, 0.011, 0.01);
}

void main()
{
    out_irradiance = vec4(0.0);
    out_specular = vec4(0.0);
    float depth = texture(scene_depth, in_uv).r;
    vec4 alb = texture(scene_albedo, in_uv);
    if (depth <= 0.0 || alb.a <= 0.0) return;  // sky / unlit (signs, props, traffic)
    vec4 wp = frame.inv_view_proj * vec4(in_uv * 2.0 - 1.0, depth, 1.0);
    vec3 p = wp.xyz / wp.w;
    vec3 n = oct_decode(texture(scene_material, in_uv).ba);
    vec3 view_dir = normalize(p - frame.camera_pos.xyz);
    if (dot(n, view_dir) > 0.0) n = -n;  // two-sided thin parts
    float shininess = floor(alb.a) * 4.0;
    float spec_w = fract(alb.a) * 2.0;

    vec3 dif, spc;
    direct_light(p, n, view_dir, max(shininess, 4.0), spec_w, dif, spc);
    vec3 irr = dif * 0.6;

    // Low sun at dusk, with a real shadow.
    if (frame.sun.w > 0.0) {
        vec3 s = frame.sun.xyz;
        float ndl = max(dot(n, s), 0.0);
        float vis = ndl > 0.0 ? rt_visible(p + n * 0.06, p + s * 400.0) : 0.0;
        irr += frame.sun.w * (kSunColor * 2.6 * ndl * vis + vec3(0.30, 0.40, 0.55) * 0.3 * (0.55 + 0.45 * n.z));
    }

    // One bounce: cosine-weighted direction, E = pi * L for a single sample.
    {
        vec3 t = normalize(abs(n.z) < 0.9 ? cross(n, vec3(0.0, 0.0, 1.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
        vec3 b = cross(n, t);
        float u1 = rt_noise(41.0), u2 = rt_noise(43.0);
        float r = sqrt(u1), a = 6.2831853 * u2;
        vec3 d = normalize(t * (r * cos(a)) + b * (r * sin(a)) + n * sqrt(max(1.0 - u1, 0.0)));
        rayQueryEXT q;
        rayQueryInitializeEXT(q, scene_tlas, gl_RayFlagsOpaqueEXT, kRtMaskWorld | kRtMaskSigns, p + n * 0.05, 0.02, d,
                              60.0);
        rayQueryProceedEXT(q);
        vec3 L;
        if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
            // Open sky: the overcast dome lit by the city (the old ambient term, but occluded).
            L = sky_color(d) * 0.35 + vec3(0.003, 0.004, 0.005);
        } else {
            float th = rayQueryGetIntersectionTEXT(q, true);
            bool sign = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true) == 1;
            L = bounce_radiance(p + d * th, d, sign, uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true)));
        }
        irr += 3.14159 * L;
    }
    out_irradiance = vec4(irr, 1.0);
    out_specular = vec4(spc * spec_w, 1.0);
}
