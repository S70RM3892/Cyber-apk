// Dual-layer material detail (spec §3.2): a low-res base texture plus a tiny tiling
// micro-detail normal map projected triplanarly in world space.
//
// The micro layer is faded in with view distance, and its tiling frequency is
// chosen per pixel from two octaves so detail stays near one texel per pixel.
// Branch-free on purpose: every pixel samples the same taps, so there is no
// divergence inside a wave.
#ifndef APEX_TRIPLANAR_MICRODETAIL_GLSL
#define APEX_TRIPLANAR_MICRODETAIL_GLSL

#include "normal_codec.glsl"

struct ApexMicroDetail {
    float base_frequency;   // tiles per metre of the micro map at octave 0 (e.g. 32)
    float fade_start;       // metres: full strength closer than this
    float fade_end;         // metres: no micro detail beyond this
    float strength;         // 0..1 blend of micro normal onto the base normal
};

// Whiteout blend of two tangent-space normals (Barré-Brisebois & Hill, "Blending in Detail").
vec3 apex_blend_whiteout(vec3 base, vec3 detail)
{
    return normalize(vec3(base.xy + detail.xy, base.z * detail.z));
}

// Sample the micro map with triplanar projection and return a world-space normal
// perturbation for the given octave frequency.
vec3 apex_triplanar_micro(sampler2D micro_map, vec3 world_pos, vec3 world_n, vec3 weights, float freq)
{
    vec3 p = world_pos * freq;
    // Per-axis tangent-space samples, swizzled into world space (UDN-style triplanar).
    vec3 tx = apex_decode_normal_xy(texture(micro_map, p.zy).ga);
    vec3 ty = apex_decode_normal_xy(texture(micro_map, p.xz).ga);
    vec3 tz = apex_decode_normal_xy(texture(micro_map, p.xy).ga);
    vec3 nx = vec3(0.0, tx.y, tx.x) * sign(world_n.x);
    vec3 ny = vec3(ty.x, 0.0, ty.y) * sign(world_n.y);
    vec3 nz = vec3(tz.x, tz.y, 0.0) * sign(world_n.z);
    return nx * weights.x + ny * weights.y + nz * weights.z;
}

// Returns the final world-space normal.
//   base_n_ws : normal after applying the base (1K/512px) normal map, world space
//   geo_n_ws  : interpolated geometric normal, world space (drives projection weights)
vec3 apex_apply_micro_detail(sampler2D micro_map, ApexMicroDetail md,
                             vec3 world_pos, vec3 camera_pos, vec3 base_n_ws, vec3 geo_n_ws)
{
    vec3 w = pow(abs(geo_n_ws), vec3(4.0));
    w /= (w.x + w.y + w.z);

    float dist = length(world_pos - camera_pos);
    float fade = 1.0 - smoothstep(md.fade_start, md.fade_end, dist);

    // Two octaves; cross-fade by log2 distance so the pattern never visibly pops.
    float octave = clamp(log2(max(dist, 1e-3) / md.fade_start + 1.0), 0.0, 1.0);
    vec3 d0 = apex_triplanar_micro(micro_map, world_pos, geo_n_ws, w, md.base_frequency);
    vec3 d1 = apex_triplanar_micro(micro_map, world_pos, geo_n_ws, w, md.base_frequency * 0.25);
    vec3 detail = mix(d0, d1, octave) * (md.strength * fade);

    return normalize(base_n_ws + detail);
}

#endif
