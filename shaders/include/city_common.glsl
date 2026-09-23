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
const uint kShanty = 2u;

struct Sign {
    vec4 pos_yaw;       // x, y, z, yaw
    vec2 size;          // width, height
    uint seed;
    uint style;         // 0 wall panel, 1 blade, 2 rooftop
};

layout(set = 0, binding = 1, std430) readonly buffer Buildings { Building buildings[]; };
layout(set = 0, binding = 2, std430) readonly buffer Signs { Sign signs[]; };
layout(set = 0, binding = 3) uniform sampler2D road_field_tex;
layout(set = 0, binding = 4) uniform sampler2D sign_atlas;          // SDF glyphs, 16 x 9 cells
layout(set = 0, binding = 5, std430) readonly buffer SignStrings { uvec4 sign_strings[]; };

struct Prop {
    vec4 pos_yaw;       // base centre, yaw
    vec3 size;          // half x, half y, full height
    uint kind_seed;
};
struct LightSprite {
    vec4 pos_size;      // centre, radius
    vec4 color_blink;   // HDR rgb, blink Hz (0 = steady)
};
layout(set = 0, binding = 6, std430) readonly buffer Props { Prop props[]; };
layout(set = 0, binding = 7, std430) readonly buffer Lights { LightSprite lights[]; };

const vec2 kAtlasCells = vec2(16.0, 9.0);  // signtext::kCols / kRows (static_assert in renderer.cpp)
const float kGlyphSdfScale = 0.25;         // (2 * kSpread) / kCell: SDF units -> cell units
const uint kGlyphChoonpu = 44u;            // signtext::kGlyphChoonpu

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

// Red / pink family only: the dominant signage colours of the target look.
vec3 neon_warm(uint h)
{
    const vec3 p[4] = vec3[4](vec3(1.0, 0.06, 0.09), vec3(1.0, 0.1, 0.06), vec3(1.0, 0.12, 0.4), vec3(1.0, 0.3, 0.5));
    return p[h & 3u];
}

// Neon palette, weighted towards the reds and pinks that dominate the target look,
// with cyan as the cool counterpoint.
vec3 neon_color(uint h)
{
    const vec3 palette[16] = vec3[16](
        vec3(1.00, 0.07, 0.10), vec3(1.00, 0.07, 0.10), vec3(1.00, 0.10, 0.08), vec3(1.00, 0.16, 0.12),  // reds
        vec3(1.00, 0.12, 0.45), vec3(1.00, 0.10, 0.35), vec3(1.00, 0.25, 0.55),                         // hot pinks
        vec3(0.85, 0.10, 1.00),                                                                          // magenta
        vec3(0.05, 0.85, 1.00), vec3(0.15, 0.95, 0.95), vec3(0.10, 0.65, 1.00),                         // cyans
        vec3(1.00, 0.40, 0.06), vec3(1.00, 0.55, 0.12),                                                  // oranges
        vec3(1.00, 0.82, 0.68),                                                                          // warm white
        vec3(0.25, 0.45, 1.00),                                                                          // electric blue
        vec3(1.00, 0.85, 0.15));                                                                         // yellow
    return palette[h & 15u];
}

// ---- Sign text (SDF atlas) ----
uint sign_string_length(uint id) { return sign_strings[id].w & 0xFFu; }
bool sign_string_japanese(uint id) { return (sign_strings[id].w & 0x100u) != 0u; }
uint sign_string_glyph(uint id, uint i)
{
    uvec4 s = sign_strings[id];
    uint word = i < 4u ? s.x : (i < 8u ? s.y : s.z);
    return (word >> ((i & 3u) * 8u)) & 0xFFu;
}

// Signed distance to glyph g's outline in cell units (+ inside), uv in [0,1]^2 (y down).
// Outside the cell the glyph is treated as empty.
float glyph_distance(uint g, vec2 uv)
{
    if (g == 0u || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return -1.0;
    vec2 cell = vec2(float(g % 16u), float(g / 16u));
    float d = texture(sign_atlas, (cell + clamp(uv, vec2(0.01), vec2(0.99))) / kAtlasCells).r;
    return (d - 0.5) * kGlyphSdfScale;
}

// Fraction of light lost to fog between the camera and world_pos (see apply_fog).
float fog_amount(vec3 world_pos)
{
    vec3 cam = frame.camera_pos.xyz;
    vec3 d = world_pos - cam;
    float falloff = frame.fog.y;
    float base = exp(-falloff * max(cam.z, 0.0));
    float integral = abs(d.z) > 1e-3 ? base * (1.0 - exp(-falloff * d.z)) / (falloff * d.z) : base;
    return min(1.0 - exp(-frame.fog.x * length(d) * integral), frame.fog.z);
}

// Exponential height fog with sky-coloured in-scattering. Heavy on purpose: it hides
// the streaming radius and gives the neon something to glow through.
vec3 apply_fog(vec3 color, vec3 world_pos)
{
    // Analytic integral of density * exp(-falloff * z) along the ray.
    vec3 d = world_pos - frame.camera_pos.xyz;
    float amount = fog_amount(world_pos);
    // Teal haze from the sky, warmed by neon close to street level.
    // Teal-grey smog (the sky's red horizon band is left out: fog shouldn't turn pink),
    // with a little neon warmth right at street level.
    vec3 dir = normalize(d);
    vec3 smog = mix(vec3(0.036, 0.042, 0.046), vec3(0.012, 0.020, 0.025), clamp(dir.z * 2.0, 0.0, 1.0));
    vec3 in_scatter = smog * 2.4 + vec3(0.04, 0.007, 0.008) * exp(-max(world_pos.z, 0.0) / 15.0);
    return mix(color, in_scatter, amount);
}

#endif
