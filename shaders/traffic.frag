#version 460
#extension GL_GOOGLE_include_directive : require
// Vehicle shading: dark body, white headlights, red tail lights, neon underglow,
// cyan cabin strip.
#include "include/city_common.glsl"

layout(location = 0) in vec3 in_local;
layout(location = 1) in vec3 in_world_pos;
layout(location = 2) flat in uint in_id;
layout(location = 3) flat in vec3 in_normal_local;
layout(location = 4) flat in int in_part;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

void main()
{
    vec3 l = in_local;
    vec3 nl = in_normal_local;
    bool player = in_id == (1u << 20);
    // The player's ride: yellow paint catching the neon, magenta underglow.
    vec3 body = player ? vec3(0.09, 0.07, 0.01) : vec3(0.02, 0.02, 0.025);
    vec3 e = vec3(0.0);

    if (in_part == 1) {
        // Cabin: tinted glass with a sliver of interior light and a roof-edge LED.
        vec3 glass = vec3(0.01, 0.015, 0.02) + vec3(0.05, 0.25, 0.3) * step(nl.z, 0.5) * 0.3;
        vec3 roof = (player ? vec3(1.0, 0.1, 0.6) : neon_color(hash_u(in_id ^ 7u))) * 2.0 *
                    step(0.5, nl.z) * smoothstep(0.8, 1.0, max(abs(l.x), abs(l.y)));
        out_color = vec4(apply_fog(glass + roof, in_world_pos), 1.0);
        out_material = vec4(0.0, 1.0, 0.5, 0.5);
        return;
    }
    float lamp_y = smoothstep(0.75, 0.55, abs(abs(l.y) - 0.55));
    if (nl.x > 0.5) e += vec3(1.0, 0.95, 0.85) * 20.0 * lamp_y * step(abs(l.z), 0.4);    // headlights
    if (nl.x < -0.5) e += vec3(1.0, 0.05, 0.03) * 14.0 * step(abs(l.z), 0.35);          // tail bar
    vec3 glow_col = player ? vec3(1.0, 0.1, 0.6) : neon_color(hash_u(in_id));
    if (nl.z < -0.5) e += glow_col * 3.0 * smoothstep(1.0, 0.4, abs(l.y));  // underglow
    if (player && abs(nl.y) > 0.5) e += vec3(1.0, 0.1, 0.6) * 2.5 * step(abs(l.z + 0.85), 0.12);  // side LED line
    if (abs(nl.y) > 0.5) e += vec3(0.1, 0.8, 1.0) * 1.5 * step(0.2, l.z) * step(abs(l.x), 0.6); // cabin strip

    // Daylight on the paint (no world normal here: lit as if from above, sides dimmer).
    vec3 paint = player ? vec3(0.55, 0.42, 0.05) : vec3(0.12, 0.12, 0.13);
    body += paint * day_light(in_world_pos, vec3(0.0, 0.0, 1.0)) * (nl.z > 0.5 ? 0.6 : 0.3);
    out_color = vec4(apply_fog(body + e, in_world_pos), 1.0);
    out_material = vec4(0.0, 1.0, 0.5, 0.5);
}
