#version 460
// Additive streak, lit by the ambient city glow.
layout(location = 0) in float in_alpha;
layout(location = 1) in float in_u;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;  // blended additively: zero leaves it unchanged

void main()
{
    float core = 1.0 - abs(in_u);
    out_color = vec4(vec3(0.35, 0.38, 0.5) * core * in_alpha * 0.25, 0.0);
    out_material = vec4(0.0);
}
