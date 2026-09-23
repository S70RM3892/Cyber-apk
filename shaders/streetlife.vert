#version 460
#extension GL_GOOGLE_include_directive : require
// Street life, generated entirely on the GPU around the camera:
//   instances [0, kLampCount)            lamp posts (pole, arm, head) — the same
//                                        lattice ground.frag lights the street with
//   instances [kLampCount, +kPedCount)   pedestrians walking the sidewalks; ~40% carry
//                                        LED umbrellas
#include "include/city_common.glsl"
#include "include/box.glsl"
#include "include/street_layout.glsl"

layout(location = 0) out vec3 out_world_pos;
layout(location = 1) flat out vec3 out_normal;
layout(location = 2) flat out uvec2 out_kind_part;  // x: 0 lamp / 1 pedestrian, y: part index
layout(location = 3) flat out uint out_seed;
layout(location = 4) out vec3 out_local;

const int kLampLines = 9;        // grid lines per family around the camera
const int kLampsPerLine = 20;    // per side
const uint kLampCount = uint(2 * kLampLines * 2 * kLampsPerLine);
const float kPedSpan = 360.0;    // metres of sidewalk simulated per line
const float kPedRange = 170.0;   // don't draw pedestrians beyond this

vec4 degenerate() { return vec4(2.0, 2.0, 2.0, 1.0); }  // outside clip space: culled

void emit(vec3 center, vec3 half_ext, vec3 fwd, vec3 local, vec3 nrm, uint kind, uint part, uint seed)
{
    vec3 side = vec3(-fwd.y, fwd.x, 0.0);
    vec3 world = center + fwd * local.x * half_ext.x + side * local.y * half_ext.y + vec3(0, 0, local.z * half_ext.z);
    out_world_pos = world;
    out_normal = fwd * nrm.x + side * nrm.y + vec3(0, 0, nrm.z);
    out_kind_part = uvec2(kind, part);
    out_seed = seed;
    out_local = local;
    gl_Position = frame.view_proj * vec4(world, 1.0);
}

void main()
{
    uint id = uint(gl_InstanceIndex);
    int part = gl_VertexIndex / 36;
    vec3 nrm;
    vec3 local = box_vertex(gl_VertexIndex % 36, nrm);
    vec3 cam = frame.camera_pos.xyz;
    float t = frame.camera_pos.w;

    if (id < kLampCount) {
        // Decode lamp: family, line offset, side, index along the street.
        int k = int(id);
        int family = k % 2; k /= 2;
        int side = (k % 2) * 2 - 1; k /= 2;
        int line_off = k % kLampLines - kLampLines / 2; k /= kLampLines;
        int j_off = k - kLampsPerLine / 2;
        float cross_coord = family == 0 ? cam.x : cam.y;
        float along_coord = family == 0 ? cam.y : cam.x;
        int i = int(floor(cross_coord / kBlock + 0.5)) + line_off;
        int j = int(floor(along_coord / kLampSpacing)) + j_off;
        Lamp l = lamp_at(family, i, j, side);
        vec3 inward = vec3(l.inward, 0.0);
        if (part > 2 || on_arterial(l.base.xy, 1.0)) { gl_Position = degenerate(); return; }
        if (part == 0) {        // pole
            emit(l.base + vec3(0, 0, kLampHeight * 0.5), vec3(0.08, 0.08, kLampHeight * 0.5), inward, local, nrm, 0u, 0u, 0u);
        } else if (part == 1) { // arm
            emit(l.base + inward * kLampReach * 0.5 + vec3(0, 0, kLampHeight + 0.1), vec3(kLampReach * 0.5 + 0.1, 0.06, 0.06),
                 inward, local, nrm, 0u, 1u, 0u);
        } else {                // head
            emit(l.head, vec3(0.35, 0.18, 0.08), inward, local, nrm, 0u, 2u, floatBitsToUint(l.on));
        }
        // Stash the lamp colour in the seed channel for the head only.
        if (part == 2) out_seed = packUnorm4x8(vec4(l.color / max(l.color.r, max(l.color.g, l.color.b)), l.on));
        return;
    }

    // ---- Pedestrian ----
    uint pid = id - kLampCount;
    bool along_x = (pid & 1u) == 0u;
    float cross_coord = along_x ? cam.y : cam.x;
    float along_coord = along_x ? cam.x : cam.y;
    float side = (hash_u(pid ^ 0x11u) & 1u) == 0u ? -1.0 : 1.0;
    float line = (floor(cross_coord / kBlock + 0.5) + float(int(hash_u(pid ^ 0x12u) % 7u) - 3)) * kBlock;
    float lateral = line + side * (kRoadwayHalf + 0.6 + 1.6 * hash_f(pid ^ 0x13u));
    float dir = (hash_u(pid ^ 0x14u) & 1u) == 0u ? 1.0 : -1.0;
    float speed = 1.1 + 0.6 * hash_f(pid ^ 0x15u);
    float s = hash_f(pid ^ 0x16u) * kPedSpan + dir * speed * t;
    float along = along_coord + (fract((s - along_coord) / kPedSpan) - 0.5) * kPedSpan;
    vec3 base = along_x ? vec3(along, lateral, 0.0) : vec3(lateral, along, 0.0);
    vec3 fwd = along_x ? vec3(dir, 0, 0) : vec3(0, dir, 0);
    if (distance(base.xy, cam.xy) > kPedRange || on_arterial(base.xy, 2.5)) { gl_Position = degenerate(); return; }

    float height = 0.9 + 0.2 * hash_f(pid ^ 0x17u);  // body scale
    float phase = t * speed * 3.2 + hash_f(pid ^ 0x18u) * 6.28;
    bool umbrella = hash_f(pid ^ 0x19u) < 0.4;
    vec3 side_v = vec3(-fwd.y, fwd.x, 0.0);

    vec3 c, h;
    switch (part) {
        case 0:   // left leg, swinging about the hip
        case 1: { // right leg
            float swing = sin(phase + (part == 0 ? 0.0 : 3.14159)) * 0.35;
            float lx = local.x * 0.07;
            float lz = (local.z * 0.45 - 0.45);  // hip at the top of the leg
            vec3 leg = vec3(lx * cos(swing) - lz * sin(swing), 0.0, lx * sin(swing) + lz * cos(swing));
            vec3 hip = base + vec3(0, 0, 0.9 * height) + side_v * (part == 0 ? 0.1 : -0.1);
            vec3 world = hip + fwd * leg.x + side_v * local.y * 0.075 + vec3(0, 0, leg.z * height);
            out_world_pos = world;
            out_normal = fwd * nrm.x + side_v * nrm.y + vec3(0, 0, nrm.z);
            out_kind_part = uvec2(1u, uint(part));
            out_seed = pid;
            out_local = local;
            gl_Position = frame.view_proj * vec4(world, 1.0);
            return;
        }
        case 2: c = vec3(0, 0, 1.22); h = vec3(0.13, 0.22, 0.32); break;        // torso + arms
        case 3: c = vec3(0.01, 0, 1.66); h = vec3(0.1, 0.09, 0.11); break;      // head
        case 4: c = vec3(0, 0, 2.05); h = vec3(0.55, 0.55, 0.035); break;       // umbrella canopy
        case 5: c = vec3(0.05, -0.18, 1.72); h = vec3(0.015, 0.015, 0.33); break; // umbrella shaft
        default: gl_Position = degenerate(); return;
    }
    if (part >= 4 && !umbrella) { gl_Position = degenerate(); return; }
    // Slight bob while walking.
    c.z = c.z * height + abs(sin(phase)) * 0.03;
    emit(base + fwd * c.x + side_v * c.y + vec3(0, 0, c.z), h * vec3(1.0, 1.0, part == 4 ? 1.0 : height), fwd, local, nrm,
         1u, uint(part), pid);
}
