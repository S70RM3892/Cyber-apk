#version 460
#extension GL_GOOGLE_include_directive : require
// Streets: arterial roads from the CPU distance field (Voronoi borders), side streets
// from the lot grid, sidewalks, lane paint, rain-soaked asphalt with puddles, and
// analytic street-lamp pools (an infinite periodic lattice of lights, no light list).
#include "include/city_common.glsl"

layout(location = 0) in vec3 in_world_pos;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

const float kBlock = 60.0;          // city::Params::block_size
const float kStreetHalf = 4.0;      // city::Params::street_half_width
const float kRoadHalf = 9.0;        // city::Params::road_half_width
const float kRoadRange = 32.0;      // RoadField::kRange
const float kLampSpacing = 30.0;

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
    float on_side = 1.0 - aa_step(kStreetHalf, d_side, w_side);
    float road = max(on_art, on_side);
    float curb = max(1.0 - aa_step(kRoadHalf + 3.0, d_art, w_art), 1.0 - aa_step(kStreetHalf + 2.5, d_side, w_side));
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

    float puddle = smoothstep(0.52, 0.62, fbm(p * 0.12 + 3.7)) * road;
    float rain = frame.fog.w;
    float wet = mix(0.35, 0.95, puddle) * rain;
    albedo *= mix(1.0, 0.45, wet);  // wet surfaces darken

    // Street lamps: lattice along side streets, offset to the sidewalk edge.
    vec2 lamp_cell = floor(p / kLampSpacing + 0.5);
    vec2 lamp_pos = lamp_cell * kLampSpacing;
    uint lamp_hash = hash_u2(ucell(lamp_cell));
    vec3 lamp_col = hash_f(lamp_hash) < 0.6 ? vec3(1.0, 0.55, 0.2) : vec3(0.6, 0.8, 1.0);
    float lamp_on = step(0.12, hash_f(lamp_hash ^ 0x55u));
    float ld = length(p - lamp_pos);
    float lamp = lamp_on * 6.0 / (1.0 + ld * ld * 0.08) * (1.0 - on_art * 0.5);

    // Neon spill from shopfronts: coloured glow close to lot edges.
    vec2 lot = floor(p / kBlock);
    vec3 spill_col = neon_color(hash_u2(ucell(lot * 7.0 + floor(p / 6.0))));
    float spill = (1.0 - smoothstep(kStreetHalf + 1.0, kStreetHalf + 7.0, d_side)) * 0.15 * (1.0 - on_art);

    vec3 ambient = vec3(0.012, 0.012, 0.03);
    vec3 lit = albedo * (ambient * 4.0 + lamp_col * lamp + spill_col * spill * 8.0);

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
    out_material = vec4(wet, mix(0.35, 0.04, puddle), ripple * 0.5 + 0.5);
}
