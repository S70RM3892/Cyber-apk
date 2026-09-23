#version 460
#extension GL_GOOGLE_include_directive : require
// Streets: arterial roads from the CPU distance field (Voronoi borders), side streets
// from the lot grid, sidewalks, lane paint, rain-soaked asphalt with puddles, and
// analytic street-lamp pools (an infinite periodic lattice of lights, no light list).
#include "include/city_common.glsl"
#include "include/street_layout.glsl"
#include "include/lighting.glsl"
#include "include/materials.glsl"

layout(location = 0) in vec3 in_world_pos;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

const float kRoadRange = 32.0;      // RoadField::kRange

float aa_step(float edge, float x, float w) { return clamp((x - edge) / max(w, 1e-4) + 0.5, 0.0, 1.0); }

void main()
{
    vec2 p = in_world_pos.xy;
    float t = frame.camera_pos.w;

    // Arterial distance (metres) from the streamed field.
    vec2 field_uv = (p - frame.road_field.xy) / frame.road_field.w;
    float d_art = texture(road_field_tex, field_uv).r * kRoadRange;

    // Side streets: distance to the nearest lot-grid line.
    vec2 g = p / kBlock;
    vec2 gd = abs(fract(g) - 0.0);
    vec2 dist_lines = min(gd, 1.0 - gd) * kBlock;   // metres to the grid lines in x / y
    float d_side = min(dist_lines.x, dist_lines.y);

    float w_art = fwidth(d_art), w_side = fwidth(d_side);
    float on_art = 1.0 - aa_step(kRoadHalf, d_art, w_art);
    float on_side = 1.0 - aa_step(kRoadwayHalf, d_side, w_side);
    float road = max(on_art, on_side);
    float curb = max(1.0 - aa_step(kRoadHalf + 3.0, d_art, w_art), 1.0 - aa_step(kCorridorHalf + 0.5, d_side, w_side));
    float sidewalk = clamp(curb - road, 0.0, 1.0);

    // Lane paint.
    float center_line = (1.0 - aa_step(0.18, d_art, w_art)) * on_art;
    float along = dist_lines.x < dist_lines.y ? p.y : p.x;
    float dash = step(0.5, fract(along / 6.0));
    float side_line = (1.0 - aa_step(0.1, d_side, w_side)) * dash * on_side * (1.0 - on_art);
    vec3 paint = center_line * vec3(0.9, 0.7, 0.1) + side_line * vec3(0.8);

    // Surface: asphalt / concrete, everything wet. Puddles are low-frequency noise.
    float grit = value_noise(p * 3.1) * 0.5 + value_noise(p * 11.0) * 0.5;
    vec3 asphalt = vec3(0.025, 0.026, 0.03) * (0.7 + 0.6 * grit);
    vec3 concrete = vec3(0.06, 0.058, 0.055) * (0.8 + 0.4 * value_noise(p * 0.9));
    float tile = step(0.94, max(fract(p.x / 1.5), fract(p.y / 1.5)));
    concrete *= 1.0 - tile * 0.4;
    vec3 albedo = mix(concrete, asphalt, road) + paint * 0.5;
    // Photographic asphalt / paving detail near the camera.
    float tex_strength = 1.0 - smoothstep(40.0, 160.0, distance(in_world_pos, frame.camera_pos.xyz));
    vec3 ground_n = vec3(0.0, 0.0, 1.0);
    float tex_rough = 0.5;
    if (tex_strength > 0.0) {
        TexSample ta = sample_material(kTexAsphalt, p, 5.0, vec3(0, 0, 1), vec3(1, 0, 0), vec3(0, 1, 0), tex_strength);
        TexSample tp = sample_material(kTexPaving, p, 3.0, vec3(0, 0, 1), vec3(1, 0, 0), vec3(0, 1, 0), tex_strength);
        float r = road;
        albedo *= mix(tp.tint, ta.tint, r);
        ground_n = normalize(mix(tp.normal, ta.normal, r));
        tex_rough = mix(tp.rough, ta.rough, r);
    }

    float puddle = smoothstep(0.52, 0.62, fbm(p * 0.12 + 3.7)) * road;
    float rain = frame.fog.w;
    // Only puddles are mirrors; the rest of the street is a rough, blurry wet sheen with
    // patchy roughness (0.1..0.6), like real rain-soaked asphalt.
    float wet = mix(0.22, 1.0, puddle) * rain;
    float rough = mix(mix(0.1, 0.6, value_noise(p * 0.35 + 11.0)), 0.03, puddle);
    albedo *= mix(1.0, 0.45, wet);  // wet surfaces darken

    // Street lamps (same lattice as the lamp-post geometry in streetlife.vert).
    vec3 lamps = lamp_light(vec3(p, 0.0));

    // Neon spill from the shopfronts: colour varies smoothly along the street (blend of
    // neighbouring 6 m shop bays), strongest at the building line, fading over the road.
    float bay = along / 6.0;
    float bay_f = smoothstep(0.2, 0.8, fract(bay));
    vec2 line_id = floor(p / kBlock + 0.5);
    uint street_seed = hash_u2(ucell(line_id * vec2(dist_lines.x < dist_lines.y ? 1.0 : 0.0, dist_lines.x < dist_lines.y ? 0.0 : 1.0)));
    vec3 spill_col = mix(neon_color(hash_u(street_seed ^ uint(int(floor(bay)) + 5000))),
                         neon_color(hash_u(street_seed ^ uint(int(floor(bay)) + 5001))), bay_f);
    float spill = smoothstep(kRoadwayHalf - 2.0, kCorridorHalf + 1.0, d_side) * (1.0 - smoothstep(kCorridorHalf + 1.0, kCorridorHalf + 4.0, d_side));
    spill *= 0.05 * (1.0 - on_art);

    vec3 ambient = vec3(0.012, 0.012, 0.03);
    // Neon signs and shopfronts light the wet street; puddles take sharp highlights.
    vec3 view_dir = normalize(in_world_pos - frame.camera_pos.xyz);
    vec3 neon_diffuse, neon_spec;
    // Puddles are flat water; elsewhere the texture's relief catches the neon.
    vec3 light_n = normalize(mix(ground_n, vec3(0.0, 0.0, 1.0), puddle));
    local_lights(vec3(p, 0.02), light_n, view_dir, mix(40.0, 600.0, puddle), neon_diffuse, neon_spec);
    // Neon on the ground reads mostly as coloured wet sheen, so the diffuse term uses a
    // brighter "wet film" albedo than the dark asphalt itself.
    vec3 neon_albedo = mix(vec3(0.05), albedo * 2.0, 0.5);
    vec3 lit = albedo * (ambient * 4.0 + lamps + spill_col * spill * 8.0) + neon_albedo * neon_diffuse +
               neon_spec * mix(0.25, 0.9, puddle) * rain;

    // Puddle ripples from rain: perturb the reflection normal.
    vec2 ripple = vec2(0.0);
    if (rain > 0.0) {
        vec2 rc = floor(p * 1.5);
        float phase = fract(t * 1.3 + hash_f2(ucell(rc)));
        vec2 center = (rc + vec2(hash_f2(ucell(rc + 11.0)), hash_f2(ucell(rc + 23.0)))) / 1.5;
        vec2 dv = p - center;
        float r = length(dv);
        float ring = sin((r - phase * 0.5) * 40.0) * exp(-r * 6.0) * (1.0 - phase);
        ripple = dv / max(r, 1e-3) * ring * 0.15 * puddle;
    }

    out_color = vec4(apply_fog(lit, in_world_pos), 1.0);
    // Reflectivity drives the screen-space reflection strength in the resolve pass.
    // Texture relief perturbs the reflection too (not in puddles).
    vec2 perturb = ripple + ground_n.xy * 0.6 * (1.0 - puddle);
    rough = mix(rough, clamp(rough + (tex_rough - 0.5) * 0.4, 0.03, 0.8), 1.0 - puddle);
    out_material = vec4(wet, rough, perturb * 0.5 + 0.5);
}
