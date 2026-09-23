#ifndef APEX_SKY_GLSL
#define APEX_SKY_GLSL

// Night sky / fog in-scatter colour for a view direction: polluted magenta-orange glow
// at the horizon over a deep blue zenith.
vec3 sky_color(vec3 dir)
{
    float h = clamp(dir.z, -0.2, 1.0);
    // Smoggy teal night over a red-lit city: dark teal zenith, grey-teal haze at the
    // horizon, a low band of reflected neon.
    vec3 zenith = vec3(0.003, 0.008, 0.011);
    vec3 horizon = vec3(0.032, 0.052, 0.058);
    vec3 glow = vec3(0.11, 0.022, 0.03);
    float t = pow(1.0 - max(h, 0.0), 5.0);
    vec3 c = mix(zenith, horizon, t);
    c += glow * pow(1.0 - max(h, 0.0), 18.0);
    return c;
}

#endif
