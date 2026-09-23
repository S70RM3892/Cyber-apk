// Photographic material layers (CC0, ambientCG; tools/gen_materials.py). Albedo is
// normalised to mean 0.5 so `albedo * 2` modulates the palette-driven colour; the second
// array holds the OpenGL-convention normal XY and roughness.
#ifndef APEX_MATERIALS_GLSL
#define APEX_MATERIALS_GLSL

layout(set = 0, binding = 12) uniform sampler2DArray mat_albedo;
layout(set = 0, binding = 13) uniform sampler2DArray mat_nrm;

const float kTexConcrete = 0.0;
const float kTexPaintedMetal = 1.0;
const float kTexCorrugated = 2.0;
const float kTexAsphalt = 3.0;
const float kTexPaving = 4.0;
const float kTexRust = 5.0;
const float kTexPlaster = 6.0;
const float kTexMetalPlates = 7.0;

struct TexSample {
    vec3 tint;     // albedo modulator, mean ~1
    vec3 normal;   // perturbed world normal
    float rough;   // 0..1
};

// uv in metres on the surface; t, b: world directions of +u and +v. `metres` = size of
// one texture repeat. The image's v runs down, so v is flipped to keep +v = up/+b.
TexSample sample_material(float layer, vec2 uv, float metres, vec3 n, vec3 t, vec3 b, float strength)
{
    vec3 st = vec3(uv.x / metres, -uv.y / metres, layer);
    TexSample s;
    s.tint = mix(vec3(1.0), texture(mat_albedo, st).rgb * 2.0, strength);
    vec3 nr = texture(mat_nrm, st).rgb;
    vec2 xy = (nr.xy * 2.0 - 1.0) * strength;
    s.normal = normalize(t * xy.x + b * xy.y + n * sqrt(max(1.0 - dot(xy, xy), 0.05)));
    s.rough = nr.z;
    return s;
}

// Wall frame from a (roughly vertical) normal: u runs along the wall, v up.
void wall_frame(vec3 n, out vec3 t, out vec3 b)
{
    t = normalize(vec3(-n.y, n.x, 0.0) + vec3(1e-5, 0.0, 0.0));
    b = vec3(0.0, 0.0, 1.0);
}

#endif
