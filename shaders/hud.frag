#version 460
// HUD shapes: rectangles, anti-aliased rings/discs, and 5x7 dot-matrix glyphs with a
// faint neon halo.
struct HudQuad {
    vec4 rect;
    vec4 color;
    uvec4 kind_bits_param;
};

layout(set = 0, binding = 0, std430) readonly buffer Quads { HudQuad quads[]; };

layout(location = 0) in vec2 in_local;
layout(location = 1) flat in uint in_index;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Params {
    vec2 logical_size;
    int pre_rotation;
    int output_is_srgb;
} params;

vec3 encode(vec3 c)
{
    // HUD colours are authored in display (sRGB) space; undo that for sRGB targets
    // so they look the same either way.
    return params.output_is_srgb != 0 ? pow(max(c, vec3(0.0)), vec3(2.2)) : c;
}

void main()
{
    HudQuad q = quads[in_index];
    uint kind = q.kind_bits_param.x;
    float coverage = 1.0;

    if (kind == 1u) {
        // Ring (param = thickness as a fraction of the radius; 0 = filled disc).
        float thickness = uintBitsToFloat(q.kind_bits_param.w);
        float r = length(in_local * 2.0 - 1.0);
        float aa = fwidth(r) * 1.5;
        float outer = 1.0 - smoothstep(1.0 - aa, 1.0, r);
        float inner = thickness > 0.0 ? smoothstep(1.0 - thickness - aa, 1.0 - thickness, r) : 1.0;
        coverage = outer * inner;
    } else if (kind == 2u) {
        vec2 g = in_local * vec2(5.0, 7.0);
        ivec2 cell = clamp(ivec2(floor(g)), ivec2(0), ivec2(4, 6));
        uint row_bits = cell.y < 4 ? (q.kind_bits_param.y >> uint(5 * cell.y)) : (q.kind_bits_param.z >> uint(5 * (cell.y - 4)));
        uint on = (row_bits >> uint(4 - cell.x)) & 1u;
        // Rounded dots read as a dot-matrix display.
        vec2 f = fract(g) - 0.5;
        float d = length(f);
        float dot_mask = 1.0 - smoothstep(0.42, 0.55, d);
        coverage = float(on) * mix(dot_mask, 1.0, 0.5);
    }

    vec4 c = q.color;
    out_color = vec4(encode(c.rgb), clamp(c.a * coverage, 0.0, 1.0));
}
