// Procedural box mesh: 6 faces x 2 triangles = 36 vertices, generated from
// gl_VertexIndex. Corners are in [-1,1]^3 box space, CCW seen from outside.
#ifndef APEX_BOX_GLSL
#define APEX_BOX_GLSL

const vec3 kBoxNormals[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));

// Vertex k (0..35) of the unit box; `normal` receives the face normal.
vec3 box_vertex(int k, out vec3 normal)
{
    int face = k / 6;
    normal = kBoxNormals[face];
    vec3 t1 = abs(normal.z) > 0.5 ? vec3(1, 0, 0) : vec3(0, 0, 1);
    vec3 t2 = cross(normal, t1);
    const vec2 q[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = q[k % 6];
    return normal + t1 * c.x + t2 * c.y;
}

#endif
