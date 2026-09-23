#version 460
#extension GL_GOOGLE_include_directive : require
// Night sky: light-polluted overcast. A low smoggy cloud deck lit from below by the
// city: teal-grey, warmer where the neon districts are.
#include "include/city_common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

void main()
{
    vec4 clip = vec4(in_uv * 2.0 - 1.0, 1e-4, 1.0);
    vec4 wp = frame.inv_view_proj * clip;
    vec3 dir = normalize(wp.xyz / wp.w - frame.camera_pos.xyz);
    float t = frame.camera_pos.w;

    vec3 c = sky_color(dir);

    if (dir.z > 0.01) {
        // Cloud deck at 350 m.
        float dist = (350.0 - frame.camera_pos.z) / dir.z;
        vec2 cp = frame.camera_pos.xy + dir.xy * dist;
        float n = fbm(cp * 0.004 + vec2(t * 0.01, t * 0.004));
        float cover = smoothstep(0.2, 0.7, n);
        vec3 under_lit = mix(vec3(0.035, 0.018, 0.024), vec3(0.018, 0.032, 0.036), value_noise(cp * 0.0015));
        float horizon_fade = smoothstep(0.01, 0.25, dir.z);
        c = mix(c, under_lit * (0.45 + 0.55 * n), cover * horizon_fade * 0.85);
    }

    out_color = vec4(c, 1.0);
    out_material = vec4(0.0, 1.0, 0.5, 0.5);
}
