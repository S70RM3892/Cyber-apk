#ifndef APEX_SKY_GLSL
#define APEX_SKY_GLSL

// Night sky / fog in-scatter colour for a view direction: polluted magenta-orange glow
// at the horizon over a deep blue zenith.
vec3 sky_color(vec3 dir)
{
    float h = clamp(dir.z, -0.2, 1.0);
    // Smoggy teal night over a red-lit city: dark teal zenith, grey-teal haze at the
    // horizon, a low band of reflected neon.
    // Overcast, light-polluted: no stars, the whole dome glows teal-grey from the city.
    vec3 zenith = vec3(0.004, 0.008, 0.010);
    vec3 horizon = vec3(0.022, 0.034, 0.038);
    vec3 glow = vec3(0.04, 0.008, 0.012);
    float t = pow(1.0 - max(h, 0.0), 3.0);
    vec3 c = mix(zenith, horizon, t);
    c += glow * pow(1.0 - max(h, 0.0), 24.0);
    return c;
}

#endif
