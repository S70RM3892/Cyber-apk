// Side-street layout shared by the ground, street-life and traffic shaders.
// Streets run along the lot grid lines (x = i * kBlock and y = j * kBlock); buildings
// never reach closer than city::Params::street_half_width to a grid line.
#ifndef APEX_STREET_LAYOUT_GLSL
#define APEX_STREET_LAYOUT_GLSL

const float kBlock = 60.0;          // city::Params::block_size
const float kCorridorHalf = 7.0;    // city::Params::street_half_width: roadway + sidewalk
const float kRoadwayHalf = 4.5;     // asphalt half-width
const float kRoadHalf = 9.0;        // city::Params::road_half_width (arterials)
const float kLampOffset = 4.9;      // pole distance from the centre line (on the curb)
const float kLampSpacing = 30.0;
const float kLampReach = 1.6;       // arm overhang towards the road
const float kLampHeight = 7.2;

struct Lamp {
    vec3 base;      // pole foot
    vec3 head;      // light position
    vec2 inward;    // unit vector from pole towards the road centre
    vec3 color;
    float on;
    bool exists;
};

// Lamp j on grid line i. family 0: line x = i*kBlock (street along y);
// family 1: line y = i*kBlock (street along x). side = +-1. Opposite sides are
// staggered by half a spacing.
Lamp lamp_at(int family, int i, int j, int side)
{
    float line = float(i) * kBlock;
    float along = (float(j) + (side > 0 ? 0.5 : 0.0)) * kLampSpacing;
    float lateral = line + float(side) * kLampOffset;
    Lamp l;
    l.base = family == 0 ? vec3(lateral, along, 0.0) : vec3(along, lateral, 0.0);
    l.inward = family == 0 ? vec2(-float(side), 0.0) : vec2(0.0, -float(side));
    l.head = l.base + vec3(l.inward * kLampReach, kLampHeight);
    uint h = hash_u3(uvec3(uint(i + 65536), uint(j + 65536), uint(family * 2 + (side > 0 ? 1 : 0))));
    l.color = hash_f(h) < 0.6 ? vec3(1.0, 0.55, 0.22) : vec3(0.55, 0.78, 1.0);
    l.on = step(0.1, hash_f(h ^ 0x55u));
    // No lamps inside a crossing street (intersections): distance to the nearest grid
    // line along the street must clear the crossing corridor.
    float m = mod(along, kBlock);
    l.exists = min(m, kBlock - m) > kCorridorHalf + 0.5;
    if (!l.exists) l.on = 0.0;
    return l;
}

// Irradiance on a horizontal surface at p from the nearby lamps: for the nearest grid
// line of each family, both sides, the three closest lamps along the street.
vec3 lamp_light(vec3 p)
{
    vec3 sum = vec3(0.0);
    for (int family = 0; family < 2; ++family) {
        float cross_coord = family == 0 ? p.x : p.y;
        float along_coord = family == 0 ? p.y : p.x;
        int i = int(floor(cross_coord / kBlock + 0.5));
        for (int side = -1; side <= 1; side += 2) {
            float off = side > 0 ? 0.5 : 0.0;
            int j0 = int(floor(along_coord / kLampSpacing - off + 0.5));
            for (int dj = -1; dj <= 1; ++dj) {
                Lamp l = lamp_at(family, i, j0 + dj, side);
                vec3 d = l.head - p;
                float d2 = dot(d, d);
                float cos_t = max(d.z, 0.0) * inversesqrt(max(d2, 1e-4));
                sum += l.color * l.on * 70.0 * cos_t / (d2 + 6.0);
            }
        }
    }
    return sum;
}

// True if world position p is on an arterial road (Voronoi border), from the streamed
// distance field. Usable from vertex shaders: explicit LOD.
bool on_arterial(vec2 p, float margin)
{
    vec2 uv = (p - frame.road_field.xy) / frame.road_field.w;
    return textureLod(road_field_tex, uv, 0.0).r * 32.0 < kRoadHalf + margin;  // 32 = RoadField::kRange
}

#endif
