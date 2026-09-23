// Local lights: neon signs, shopfronts, lit canopies (apex::PointLight), binned on the
// CPU into a kLightGridSize^2 grid over the road-field square (apex::build_light_grid).
#ifndef APEX_LIGHTING_GLSL
#define APEX_LIGHTING_GLSL

#include "frame_ubo.glsl"

struct PointLight {
    vec4 pos_radius;
    vec4 color;  // rgb intensity
};
layout(set = 0, binding = 8, std430) readonly buffer PointLights { PointLight point_lights[]; };
layout(set = 0, binding = 9, std430) readonly buffer LightGrid { uint light_grid[]; };

const int kLightGridSize = 128;  // world.hpp

// Diffuse irradiance and a normalised Blinn-Phong specular from the lights around p.
// view_dir points from the camera to p.
void local_lights(vec3 p, vec3 n, vec3 view_dir, float shininess, out vec3 diffuse, out vec3 specular)
{
    diffuse = vec3(0.0);
    specular = vec3(0.0);
    vec2 g = (p.xy - frame.road_field.xy) / frame.road_field.w * float(kLightGridSize);
    if (any(lessThan(g, vec2(0.0))) || any(greaterThanEqual(g, vec2(float(kLightGridSize))))) return;
    uint cell = uint(g.y) * uint(kLightGridSize) + uint(g.x);
    uint first = light_grid[cell * 2u];
    uint count = light_grid[cell * 2u + 1u];
    float spec_norm = (shininess + 8.0) / 25.13;
    for (uint i = 0u; i < count; ++i) {
        PointLight l = point_lights[light_grid[first + i]];
        vec3 d = l.pos_radius.xyz - p;
        float d2 = dot(d, d);
        float r2 = l.pos_radius.w * l.pos_radius.w;
        if (d2 >= r2) continue;
        float window = 1.0 - d2 / r2;
        window *= window;
        vec3 ld = d * inversesqrt(max(d2, 1e-4));
        float ndl = dot(n, ld);
        // Slight wrap: large area lights still graze surfaces at right angles to them.
        float wrap = max((ndl + 0.15) / 1.15, 0.0);
        vec3 e = l.color.rgb * window / (d2 + 1.0);
        diffuse += e * wrap;
        vec3 h = normalize(ld - view_dir);
        specular += e * max(ndl, 0.0) * pow(max(dot(n, h), 0.0), shininess) * spec_norm;
    }
}

#endif
