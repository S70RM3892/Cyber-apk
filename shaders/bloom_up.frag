#version 460
// 3x3 tent upsample, additively blended onto the next larger mip.
layout(set = 0, binding = 1) uniform sampler2D src;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Params {
    vec2 src_texel;
    float radius;
    float pad;
} params;

void main()
{
    vec2 t = params.src_texel * params.radius;
    vec3 s = texture(src, in_uv).rgb * 4.0;
    s += (texture(src, in_uv + vec2(-t.x, 0)).rgb + texture(src, in_uv + vec2(t.x, 0)).rgb +
          texture(src, in_uv + vec2(0, -t.y)).rgb + texture(src, in_uv + vec2(0, t.y)).rgb) * 2.0;
    s += texture(src, in_uv + vec2(-t.x, -t.y)).rgb + texture(src, in_uv + vec2(t.x, -t.y)).rgb +
         texture(src, in_uv + vec2(-t.x, t.y)).rgb + texture(src, in_uv + vec2(t.x, t.y)).rgb;
    out_color = vec4(s / 16.0, 1.0);
}
