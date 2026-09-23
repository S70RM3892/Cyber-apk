#ifndef APEX_SKY_GLSL
#define APEX_SKY_GLSL

#include "frame_ubo.glsl"

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
    // Dusk: deep blue overhead, pale warm haze at the horizon, a hot low sun.
    float d = frame.sun.w;
    if (d > 0.0) {
        vec3 s = frame.sun.xyz;
        float mu = max(dot(dir, s), 0.0);
        vec3 day = mix(vec3(0.12, 0.24, 0.44), vec3(0.62, 0.66, 0.66), pow(1.0 - max(h, 0.0), 4.0));
        day += vec3(1.0, 0.6, 0.3) * (pow(mu, 8.0) * 0.9 + pow(mu, 64.0) * 2.5);   // glow around the sun
        day += vec3(20.0, 16.0, 11.0) * smoothstep(0.9995, 0.9998, mu);            // the disc
        c = mix(c, day, d);
    }
    return c;
}

#endif
