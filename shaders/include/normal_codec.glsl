// Two-channel normal decode (spec §3.3). Mirrors engine/include/apex/normal_codec.hpp.
// ASTC has no RG format: encode with `astcenc -normal` (rrrg swizzle) and pass `.ga` here.
#ifndef APEX_NORMAL_CODEC_GLSL
#define APEX_NORMAL_CODEC_GLSL

vec3 apex_decode_normal_xy(vec2 enc)
{
    vec2 xy = enc * 2.0 - 1.0;
    // max() absorbs block-compression error that pushes |xy| past 1.
    float z = sqrt(max(0.0, 1.0 - dot(xy, xy)));
    return normalize(vec3(xy, z));
}

#endif
