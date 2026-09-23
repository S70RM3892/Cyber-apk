// Per-frame uniforms (set 0, binding 0) shared by every pass.
// Must match FrameUniforms in renderer/renderer.cpp.
#ifndef APEX_FRAME_UBO_GLSL
#define APEX_FRAME_UBO_GLSL

layout(set = 0, binding = 0) uniform Frame {
    mat4 view_proj;
    mat4 inv_view_proj;
    mat4 view;
    mat4 proj;
    vec4 camera_pos;    // xyz, w = time (s)
    vec4 road_field;    // xy origin, z metres per texel, w extent (metres)
    vec4 viewport;      // internal w, h, 1/w, 1/h
    vec4 fog;           // x density, y height falloff, z max opacity, w rain intensity
    vec4 objective;     // xy gig beacon position, z seconds since issued, w 1 = active
} frame;

#endif
