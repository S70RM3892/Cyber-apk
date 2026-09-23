#version 460
// HUD quads in logical pixels, rotated into the physical swapchain orientation with
// the same pre-rotation the tonemap pass uses.
struct HudQuad {
    vec4 rect;    // x, y, w, h (logical px, top-left origin)
    vec4 color;
    uvec4 kind_bits_param;  // kind, glyph rows 0-3, glyph rows 4-6, floatBits(param)
};

layout(set = 0, binding = 0, std430) readonly buffer Quads { HudQuad quads[]; };

layout(push_constant) uniform Params {
    vec2 logical_size;
    int pre_rotation;
    int pad;
} params;

layout(location = 0) out vec2 out_local;  // 0..1 across the quad
layout(location = 1) flat out uint out_index;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 c = corners[gl_VertexIndex];
    HudQuad q = quads[gl_InstanceIndex];
    vec2 px = q.rect.xy + c * q.rect.zw;
    vec2 l = px / params.logical_size * 2.0 - 1.0;  // logical clip space (y down)
    // Physical = R(angle) * logical (developer.android.com/games/optimize/vulkan-prerotation).
    vec2 p = l;
    if (params.pre_rotation == 90) p = vec2(-l.y, l.x);
    else if (params.pre_rotation == 180) p = -l;
    else if (params.pre_rotation == 270) p = vec2(l.y, -l.x);
    out_local = c;
    out_index = uint(gl_InstanceIndex);
    gl_Position = vec4(p, 0.0, 1.0);
}
