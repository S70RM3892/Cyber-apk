#version 460
#extension GL_GOOGLE_include_directive : require
// G-buffer pass for opaque city geometry: base layer (1K/512px, spec §3.1) plus the
// triplanar micro-detail layer (spec §3.2). Writes the inputs the upscaler needs:
// normal, material, and motion vectors.

#include "include/triplanar_microdetail.glsl"

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) in vec3 in_world_normal;
layout(location = 2) in vec4 in_world_tangent;  // w = bitangent sign
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_clip_curr;      // unjittered
layout(location = 5) in vec4 in_clip_prev;      // unjittered, previous frame

layout(set = 1, binding = 0) uniform sampler2D base_color;   // sRGB ASTC
layout(set = 1, binding = 1) uniform sampler2D base_normal;  // astcenc -normal (rrrg): X in RGB, Y in A -> sample .ga
layout(set = 1, binding = 2) uniform sampler2D pbr_mask;     // R roughness (vMF-baked mips), G metallic, B AO, A cavity
layout(set = 1, binding = 3) uniform sampler2D micro_normal; // 64x64 tiling micro-detail, same .ga layout

layout(set = 0, binding = 0) uniform FrameData {
    vec4 camera_pos;
    vec4 micro;  // x: base_frequency, y: fade_start, z: fade_end, w: strength
} frame;

layout(location = 0) out vec4 out_albedo_ao;       // RGBA8
layout(location = 1) out vec4 out_normal_rough;    // RGB10A2: octahedral normal would be tighter; kept simple here
layout(location = 2) out vec2 out_motion;          // RG16F, uv-space delta (curr - prev)

void main()
{
    vec3 n_geo = normalize(in_world_normal);
    vec3 t = normalize(in_world_tangent.xyz);
    vec3 b = cross(n_geo, t) * in_world_tangent.w;
    vec3 n_ts = apex_decode_normal_xy(texture(base_normal, in_uv).ga);
    vec3 n_base = normalize(mat3(t, b, n_geo) * n_ts);

    ApexMicroDetail md = ApexMicroDetail(frame.micro.x, frame.micro.y, frame.micro.z, frame.micro.w);
    vec3 n = apex_apply_micro_detail(micro_normal, md, in_world_pos, frame.camera_pos.xyz, n_base, n_geo);

    vec4 mask = texture(pbr_mask, in_uv);
    out_albedo_ao = vec4(texture(base_color, in_uv).rgb, mask.b);
    out_normal_rough = vec4(n * 0.5 + 0.5, mask.r);

    vec2 curr = in_clip_curr.xy / in_clip_curr.w;
    vec2 prev = in_clip_prev.xy / in_clip_prev.w;
    out_motion = (curr - prev) * vec2(0.5, -0.5);
}
