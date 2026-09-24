#version 460
layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in vec3 in_color;
layout(location = 2) flat in float in_energy;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;  // additive: zero keeps it
layout(location = 2) out vec4 out_albedo;    // RT G-buffer (0: not lit by the ray-traced pass)

void main()
{
    out_albedo = vec4(0.0);
    float r = length(in_uv) * 2.0;             // 1 = light radius
    float core = exp(-r * r * 3.0);
    float halo = exp(-r * 2.2) * 0.12;
    out_color = vec4(in_color * (core + halo) * in_energy, 0.0);
    out_material = vec4(0.0);
}
