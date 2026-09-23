#version 460
#extension GL_GOOGLE_include_directive : require
// Procedural neon: glyph strings from 5x5 hashed bitmaps (vertical on blades,
// horizontal on panels), a tube border, animated billboards on rooftops, and the
// occasional failing tube. Output is HDR so the bloom pass does the glow.
#include "include/city_common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_world_pos;
layout(location = 2) flat in uint in_instance;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

// Glyph mask for a character cell: 5x5 bitmap from the hash, strokes drawn as
// rounded pixels so they read like bent tubes rather than blocks.
float glyph(vec2 uv, uint h, float aa)
{
    vec2 g = uv * 5.0;
    ivec2 ci = ivec2(floor(g));
    if (ci.x < 0 || ci.y < 0 || ci.x > 4 || ci.y > 4) return 0.0;
    // Symmetric-ish glyphs look more like real characters.
    int mx = ci.x > 2 ? 4 - ci.x : ci.x;
    uint bit = uint(ci.y * 3 + mx);
    uint bits = hash_u(h) | 0x2000u;  // ensure the centre column has something
    float on = float((bits >> bit) & 1u);
    vec2 f = fract(g) - 0.5;
    float d = max(abs(f.x), abs(f.y));
    return on * clamp((0.42 - d) / max(aa * 5.0, 1e-3), 0.0, 1.0);
}

void main()
{
    Sign s = signs[in_instance];
    uint h = s.seed;
    float t = frame.camera_pos.w;
    vec2 uv = in_uv;
    vec2 aa2 = fwidth(uv);
    float aa = max(aa2.x, aa2.y);

    vec3 col = neon_color(hash_u(h));
    vec3 col2 = neon_color(hash_u(h ^ 0x51u));
    float intensity = 9.0 + 9.0 * hash_f(h ^ 0x33u);

    // Failing tubes: some signs stutter.
    float broken = hash_f(h ^ 0x99u) < 0.08 ? step(0.35, fract(sin(floor(t * 9.0) + float(h & 1023u)) * 43758.5)) : 1.0;

    // Border tube.
    vec2 e = min(uv, 1.0 - uv) * s.size;             // metres to the edges
    float edge_d = min(e.x, e.y);
    float border = clamp((0.12 - abs(edge_d - 0.12)) / max(fwidth(edge_d), 1e-3), 0.0, 1.0);

    float text = 0.0;
    vec3 emissive = vec3(0.0);
    if (s.style == 2u) {
        // Rooftop billboard: animated gradient ad with a slogan line.
        float scan = 0.5 + 0.5 * sin(uv.y * 40.0 - t * 3.0);
        vec3 bg = mix(col, col2, uv.x + 0.2 * sin(t * 0.7 + uv.y * 3.0)) * (0.35 + 0.25 * scan);
        float bars = step(0.5, fract(uv.x * 6.0 + t * 0.4)) * step(uv.y, 0.25);
        vec2 tuv = vec2(fract(uv.x * 8.0), (uv.y - 0.35) / 0.35);
        text = glyph(tuv, h + uint(floor(uv.x * 8.0)), aa * 8.0);
        emissive = bg * 3.0 + bars * col2 * 2.0 + text * vec3(1.0) * 8.0;
        emissive += border * col * intensity;
    } else {
        bool vertical = s.style == 1u;
        float n_chars = vertical ? max(1.0, floor(s.size.y / s.size.x)) : max(1.0, floor(s.size.x / s.size.y * 1.2));
        vec2 cuv = vertical ? vec2(uv.x, uv.y * n_chars) : vec2(uv.x * n_chars, uv.y);
        float idx = vertical ? floor(cuv.y) : floor(cuv.x);
        vec2 local = fract(cuv);
        local = (local - 0.5) * 1.35 + 0.5;  // margin between characters
        text = glyph(local, h + uint(idx) * 131u, aa * n_chars * 1.35);
        // Chase animation on some signs.
        float chase = hash_f(h ^ 0x71u) < 0.3 ? step(0.3, fract(idx / n_chars - t * 0.5)) : 1.0;
        emissive = text * mix(col, vec3(1.0), 0.25) * intensity * chase + border * col2 * intensity * 0.7;
    }

    // Unlit backplate: dark metal, faintly lit by its own glow.
    vec3 plate = vec3(0.02, 0.02, 0.025) + col * 0.04;
    vec3 radiance = plate + emissive * broken;
    out_color = vec4(apply_fog(radiance, in_world_pos), 1.0);
    out_material = vec4(0.0, 1.0, 0.5, 0.5);
}
