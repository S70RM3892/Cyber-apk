#version 460
#extension GL_GOOGLE_include_directive : require
// Elevated expressway segments around the camera: deck slab, two parapets, a pier and
// its cap per 32 m segment (5 boxes = 180 vertices per instance).
#include "include/city_common.glsl"
#include "include/box.glsl"
#include "include/highway.glsl"

layout(location = 0) out vec3 out_world_pos;
layout(location = 1) flat out vec3 out_normal;
layout(location = 2) out vec3 out_local;
layout(location = 3) flat out uint out_part;
layout(location = 4) flat out vec3 out_half;

const int kLines = 7;       // per family, centred on the camera
const int kSegments = 61;   // per line, centred on the camera

void main()
{
    int k = gl_InstanceIndex;
    int family = k % 2; k /= 2;
    int line_off = k % kLines - kLines / 2; k /= kLines;
    int seg_off = k - kSegments / 2;
    int part = gl_VertexIndex / 36;

    vec3 cam = frame.camera_pos.xyz;
    float cross_c = family == 0 ? cam.x : cam.y;
    float along_c = family == 0 ? cam.y : cam.x;
    float line = (floor(cross_c / kHighwayEvery + 0.5) + float(line_off)) * kHighwayEvery;
    float along = (floor(along_c / kSegment) + float(seg_off) + 0.5) * kSegment;
    float deck = kDeckHeight[family];

    // Part layout in (along, across, up) space: centre and half extents.
    vec3 c, h;
    if (part == 0) {        // deck slab
        c = vec3(0.0, 0.0, deck - kDeckThickness * 0.5);
        h = vec3(kSegment * 0.5, kDeckHalfWidth, kDeckThickness * 0.5);
    } else if (part <= 2) { // parapets
        float side = part == 1 ? -1.0 : 1.0;
        c = vec3(0.0, side * (kDeckHalfWidth - 0.2), deck + 0.45);
        h = vec3(kSegment * 0.5, 0.2, 0.45);
    } else if (part == 3) { // pier
        c = vec3(-kSegment * 0.5, 0.0, (deck - kDeckThickness) * 0.5);
        h = vec3(kPierHalf, kPierHalf, (deck - kDeckThickness) * 0.5);
    } else {                // pier cap (crossbeam)
        c = vec3(-kSegment * 0.5, 0.0, deck - kDeckThickness - 0.6);
        h = vec3(1.2, kDeckHalfWidth - 0.6, 0.6);
    }

    vec3 nrm;
    vec3 local = box_vertex(gl_VertexIndex % 36, nrm);
    vec3 ax = family == 0 ? vec3(0, 1, 0) : vec3(1, 0, 0);   // along the line
    vec3 ay = family == 0 ? vec3(-1, 0, 0) : vec3(0, 1, 0);  // across (right-handed with up)
    vec3 origin = family == 0 ? vec3(line, along, 0.0) : vec3(along, line, 0.0);
    vec3 p = c + local * h;
    vec3 world = origin + ax * p.x + ay * p.y + vec3(0, 0, p.z);

    out_world_pos = world;
    out_normal = ax * nrm.x + ay * nrm.y + vec3(0, 0, nrm.z);
    out_local = local;
    out_part = uint(part);
    out_half = h;
    gl_Position = frame.view_proj * vec4(world, 1.0);
}
