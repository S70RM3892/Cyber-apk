#version 460
// Fullscreen triangle. z = 0 is the far plane under reversed-Z, which the sky pass
// relies on (depth test GREATER_OR_EQUAL against a 0-cleared buffer).
layout(location = 0) out vec2 out_uv;

void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    out_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
