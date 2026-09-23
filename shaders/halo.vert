#version 460
#extension GL_GOOGLE_include_directive : require
// Light halos in the smog around big signs (visual fix spec 5: coloured fog). Additive
// camera-facing sprites drawn after the scene; halo.frag fades them into the depth buffer.
#include "include/frame_ubo.glsl"

struct Halo {
    vec4 pos_radius;
    vec4 color;
};
layout(set = 0, binding = 11, std430) readonly buffer Halos { Halo halos[]; };

layout(location = 0) out vec2 out_uv;
layout(location = 1) flat out vec3 out_color;
layout(location = 2) flat out float out_view_z;
layout(location = 3) flat out float out_radius;

void main()
{
    const vec2 q[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = q[gl_VertexIndex];
    Halo h = halos[gl_InstanceIndex];
    vec3 center = h.pos_radius.xyz;
    vec3 cam = frame.camera_pos.xyz;
    vec3 fwd = normalize(center - cam);
    vec3 right = normalize(cross(fwd, vec3(0, 0, 1)) + vec3(1e-5, 0, 0));
    vec3 up = cross(right, fwd);
    float r = h.pos_radius.w;
    vec3 world = center + (right * c.x + up * c.y) * r;
    // Denser-looking air further away: near halos stay faint, distant ones glow.
    float dist = distance(center, cam);
    float air = 1.0 - exp(-dist * 0.012);
    out_color = h.color.rgb * air * (1.0 - 0.8 * frame.sun.w);  // barely visible by day
    out_uv = c;
    out_view_z = dot(center - cam, fwd);
    out_radius = r;
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
