#ifndef APEX_SKY_GLSL
#define APEX_SKY_GLSL

// Night sky / fog in-scatter colour for a view direction: polluted magenta-orange glow
// at the horizon over a deep blue zenith.
vec3 sky_color(vec3 dir)
{
    float h = clamp(dir.z, -0.2, 1.0);
    vec3 zenith = vec3(0.004, 0.006, 0.018);
    vec3 horizon = vec3(0.10, 0.035, 0.09);
    vec3 glow = vec3(0.16, 0.06, 0.035);
    float t = pow(1.0 - max(h, 0.0), 6.0);
    vec3 c = mix(zenith, horizon, t);
    c += glow * pow(1.0 - max(h, 0.0), 24.0);
    return c;
}

#endif
