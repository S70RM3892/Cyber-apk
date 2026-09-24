// Per-frame uniforms (set 0, binding 0) shared by every pass.
// Must match FrameUniforms in renderer/renderer.cpp.
#ifndef APEX_FRAME_UBO_GLSL
#define APEX_FRAME_UBO_GLSL

#ifdef APEX_RT
// Ray-query build of the shader (renderer picks it when the device supports it).
#extension GL_EXT_ray_query : require
#endif

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
    vec4 player_car;    // xy position, z yaw, w 1 = spawned
    vec4 sun;           // xyz direction towards the sun, w daylight (0 = night, 1 = dusk)
    mat4 prev_view_proj;  // last frame's unjittered view-projection (TAA reprojection)
    vec4 taa;           // xy this frame's jitter (NDC), z frame index, w unused
} frame;

#endif
