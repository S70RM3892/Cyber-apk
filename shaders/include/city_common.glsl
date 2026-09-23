// Shared frame data, instance layouts and helpers for the city passes.
// Layouts mirror engine/include/apex/world.hpp and renderer/renderer.cpp (FrameUniforms).
#ifndef APEX_CITY_COMMON_GLSL
#define APEX_CITY_COMMON_GLSL

#include "frame_ubo.glsl"
#include "sky.glsl"

struct Building {
    vec4 pos_size;      // x, y, footprint, top z
    uvec4 seed_district_flags_base;  // w: floatBitsToUint(base z)
};
const uint kTopTier = 1u;

struct Sign {
    vec4 pos_yaw;       // x, y, z, yaw
    vec2 size;          // width, height
    uint seed;
    uint style;         // 0 wall panel, 1 blade, 2 rooftop
};

layout(set = 0, binding = 1, std430) readonly buffer Buildings { Building buildings[]; };
layout(set = 0, binding = 2, std430) readonly buffer Signs { Sign signs[]; };
layout(set = 0, binding = 3) uniform sampler2D road_field_tex;

const float PI = 3.14159265;

// PCG-style integer hash (Jarzynski & Olano 2020), 32-bit only: no int64 on mobile.
uint hash_u(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint hash_u2(uvec2 v) { return hash_u(v.x ^ hash_u(v.y)); }
uint hash_u3(uvec3 v) { return hash_u(v.x ^ hash_u(v.y ^ hash_u(v.z))); }
float hash_f(uint v) { return float(hash_u(v) & 0x00FFFFFFu) / 16777216.0; }
float hash_f2(uvec2 v) { return float(hash_u2(v) & 0x00FFFFFFu) / 16777216.0; }
float hash_f3(uvec3 v) { return float(hash_u3(v) & 0x00FFFFFFu) / 16777216.0; }
uvec2 ucell(vec2 p) { return uvec2(ivec2(floor(p))); }

float value_noise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash_f2(ucell(i)), b = hash_f2(ucell(i + vec2(1, 0)));
    float c = hash_f2(ucell(i + vec2(0, 1))), d = hash_f2(ucell(i + vec2(1, 1)));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) {
        s += a * value_noise(p);
        p = p * 2.03 + vec2(17.1, 9.7);
        a *= 0.5;
    }
    return s;
}

// Neon palette: saturated, HDR-friendly primaries of the genre.
vec3 neon_color(uint h)
{
    const vec3 palette[8] = vec3[8](
        vec3(1.00, 0.10, 0.55),  // hot magenta
        vec3(0.05, 0.85, 1.00),  // cyan
        vec3(1.00, 0.85, 0.10),  // sodium yellow
        vec3(1.00, 0.18, 0.12),  // red
        vec3(0.55, 0.20, 1.00),  // violet
        vec3(0.20, 1.00, 0.45),  // acid green
        vec3(1.00, 0.45, 0.05),  // orange
        vec3(0.25, 0.45, 1.00)); // electric blue
    return palette[h & 7u];
}

// Exponential height fog with sky-coloured in-scattering. Heavy on purpose: it hides
// the streaming radius and gives the neon something to glow through.
vec3 apply_fog(vec3 color, vec3 world_pos)
{
    vec3 cam = frame.camera_pos.xyz;
    vec3 d = world_pos - cam;
    float dist = length(d);
    float density = frame.fog.x;
    float falloff = frame.fog.y;
    // Analytic integral of density * exp(-falloff * z) along the ray.
    float dz = d.z;
    float base = exp(-falloff * max(cam.z, 0.0));
    float integral = abs(dz) > 1e-3 ? base * (1.0 - exp(-falloff * dz)) / (falloff * dz) : base;
    float amount = 1.0 - exp(-density * dist * integral);
    amount = min(amount, frame.fog.z);
    vec3 in_scatter = sky_color(normalize(d)) * 1.6;
    return mix(color, in_scatter, amount);
}

#endif
