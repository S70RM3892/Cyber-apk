#version 460
#extension GL_GOOGLE_include_directive : require
// Adds the denoised ray-traced lighting to the scene: albedo * irradiance + specular,
// through the same smog the scene pass applied (blended additively into scene colour).
#include "include/frame_ubo.glsl"

layout(set = 0, binding = 1) uniform sampler2D irradiance;
layout(set = 0, binding = 2) uniform sampler2D specular;
layout(set = 0, binding = 3) uniform sampler2D scene_albedo;
layout(set = 0, binding = 4) uniform sampler2D scene_depth;

layout(location = 0) in vec2 in_uv;

// city_common.glsl fog_amount (not included: its scene bindings would clash with set 0 here).
float fog_amount(vec3 world_pos)
{
    vec3 cam = frame.camera_pos.xyz;
    vec3 d = world_pos - cam;
    float falloff = frame.fog.y;
    float base = exp(-falloff * max(cam.z, 0.0));
    float integral = abs(d.z) > 1e-3 ? base * (1.0 - exp(-falloff * d.z)) / (falloff * d.z) : base;
    return min(1.0 - exp(-frame.fog.x * length(d) * integral), frame.fog.z);
}
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(0.0);
    float depth = texture(scene_depth, in_uv).r;
    vec4 alb = texture(scene_albedo, in_uv);
    if (depth <= 0.0 || alb.a <= 0.0) return;
    vec4 wp = frame.inv_view_proj * vec4(in_uv * 2.0 - 1.0, depth, 1.0);
    vec3 p = wp.xyz / wp.w;
    vec3 lit = alb.rgb * texture(irradiance, in_uv).rgb + texture(specular, in_uv).rgb;
    out_color = vec4(lit * (1.0 - fog_amount(p)), 0.0);
}
