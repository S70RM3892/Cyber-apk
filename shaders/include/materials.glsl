// Photographic material layers (CC0, ambientCG; tools/gen_materials.py): 1K colour as
// photographed (AO baked in) and a 512 array of OpenGL-convention normal XY + roughness.
#ifndef APEX_MATERIALS_GLSL
#define APEX_MATERIALS_GLSL

layout(set = 0, binding = 12) uniform sampler2DArray mat_albedo;
layout(set = 0, binding = 13) uniform sampler2DArray mat_nrm;

// Layers (tools/gen_materials.py LAYERS).
const float kTexConcrete = 0.0;      // Concrete034: stained grey concrete
const float kTexPaintedMetal = 1.0;  // Metal021: dark weathered steel
const float kTexCorrugated = 2.0;    // CorrugatedSteel005: galvanised sheet
const float kTexAsphalt = 3.0;
const float kTexPaving = 4.0;
const float kTexRust = 5.0;          // Rust009
const float kTexPlaster = 6.0;       // PaintedPlaster006: peeling plaster
const float kTexMetalPlates = 7.0;
const float kTexCorrugatedBlue = 8.0;   // CorrugatedSteel007B: blue paint over rust
const float kTexCorrugatedRed = 9.0;    // CorrugatedSteel002
const float kTexCorrugatedGreen = 10.0; // CorrugatedSteel006A
const float kTexRustyWhite = 11.0;      // Metal041B: white paint, rust runs
const float kTexStainedConcrete = 12.0; // Concrete042C
const float kTexMosaic = 13.0;          // Tiles133A: small facade tiles
const float kTexDarkTiles = 14.0;       // Tiles138
const float kTexWood = 15.0;            // WoodSiding005

// Mean linear albedo per layer (printed by gen_materials.py): turns a layer into a
// mean-1 modulator where a palette colour stays in charge.
const vec3 kLayerMean[16] = vec3[16](vec3(0.482, 0.482, 0.482), vec3(0.107, 0.122, 0.139), vec3(0.261, 0.267, 0.278), vec3(0.185, 0.185, 0.185), vec3(0.319, 0.288, 0.180), vec3(0.248, 0.069, 0.020), vec3(0.555, 0.545, 0.521), vec3(0.062, 0.068, 0.070), vec3(0.159, 0.237, 0.266), vec3(0.307, 0.073, 0.076), vec3(0.018, 0.088, 0.019), vec3(0.331, 0.282, 0.257), vec3(0.111, 0.091, 0.079), vec3(0.642, 0.733, 0.752), vec3(0.037, 0.037, 0.038), vec3(0.079, 0.037, 0.031));

struct TexSample {
    vec3 albedo;   // the photographed colour (linear), faded to the layer mean by strength
    vec3 tint;     // albedo / layer mean: modulator, mean ~1
    vec3 normal;   // perturbed world normal
    float rough;   // 0..1
};

float mat_hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float mat_noise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mat_hash(i), mat_hash(i + vec2(1, 0)), f.x), mix(mat_hash(i + vec2(0, 1)), mat_hash(i + vec2(1, 1)), f.x), f.y);
}

// Tiling without visible repeats (Inigo Quilez, "texture repetition", technique 3): a
// low-frequency noise picks one of 8 random offsets per region; two neighbouring offsets
// are blended across region borders, with the blend biased by the texture itself so the
// seam follows its features. Same gradients for both fetches, so mips stay correct.
vec4 sample_untiled(sampler2DArray tex, vec2 uv, float layer, out float k_out)
{
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    float k = mat_noise(uv * 0.37) * 8.0;
    float i = floor(k), f = fract(k);
    vec2 off_a = sin(vec2(3.0, 7.0) * i);
    vec2 off_b = sin(vec2(3.0, 7.0) * (i + 1.0));
    vec4 a = textureGrad(tex, vec3(uv + off_a, layer), dx, dy);
    vec4 b = textureGrad(tex, vec3(uv + off_b, layer), dx, dy);
    k_out = smoothstep(0.2, 0.8, f - 0.1 * dot(a.rgb - b.rgb, vec3(1.0)));
    return mix(a, b, k_out);
}
vec4 sample_with(sampler2DArray tex, vec2 uv, float layer, float k)
{
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    float i = floor(mat_noise(uv * 0.37) * 8.0);
    vec4 a = textureGrad(tex, vec3(uv + sin(vec2(3.0, 7.0) * i), layer), dx, dy);
    vec4 b = textureGrad(tex, vec3(uv + sin(vec2(3.0, 7.0) * (i + 1.0)), layer), dx, dy);
    return mix(a, b, k);
}

// uv in metres on the surface; t, b: world directions of +u and +v. `metres` = size of
// one texture repeat. The image's v runs down, so v is flipped to keep +v = up/+b.
// strength fades the whole layer (distance); relief scales only the normal.
TexSample sample_material(float layer, vec2 uv, float metres, vec3 n, vec3 t, vec3 b, float strength,
                          float relief)
{
    vec2 st = vec2(uv.x / metres, -uv.y / metres);
    int li = clamp(int(layer), 0, 15);
    float k;
    vec3 photo = sample_untiled(mat_albedo, st, layer, k).rgb;
    TexSample s;
    s.albedo = mix(kLayerMean[li], photo, strength);
    s.tint = s.albedo / max(kLayerMean[li], vec3(0.01));
    vec3 nr = sample_with(mat_nrm, st, layer, k).rgb;
    vec2 xy = (nr.xy * 2.0 - 1.0) * strength * relief;
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
