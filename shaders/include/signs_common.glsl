// Sign shading, compiled twice:
//   SIGN_GLOW_PASS 0  opaque pass: wall panels, blades, rooftop billboards, video screens
//   SIGN_GLOW_PASS 1  additive pass: free-standing neon lettering (no backplate), so the
//                     halo adds light over whatever is behind it instead of a dark box
//
// Text comes from the SDF glyph atlas (spec §3.1: signage as SDF vector textures), so it
// stays sharp at any distance and costs one texture fetch per pixel.
#include "city_common.glsl"

layout(location = 0) in vec2 in_uv;           // [0,1]^2, v down (text order)
layout(location = 1) in vec3 in_world_pos;
layout(location = 2) flat in uint in_instance;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_material;

const uint kWallPanel = 0u, kBlade = 1u, kRooftop = 2u, kScreen = 3u, kNeonText = 4u;

// Neon tube response for a glyph distance d (cell units, + inside): a hot, slightly
// white core with a coloured halo. `aa` is the pixel footprint in cell units.
vec3 neon_glyph(float d, float aa, vec3 col, float intensity, out float coverage)
{
    float fill = smoothstep(-aa, aa, d);
    float core = fill * (0.75 + 0.25 * smoothstep(0.0, 0.04, d));
    float halo = exp(-max(-d, 0.0) * 40.0) * (1.0 - fill);
    coverage = max(fill, halo);
    vec3 hot = mix(col, vec3(1.0), 0.3);
    return (hot * core + col * halo * 0.35) * intensity;
}

// Distance to the text laid out along one axis. `along` in [0,1] across the whole
// string, `across` in [0,1] across the line; glyph cells are square in world space
// when the sign's aspect is n : 1.
float text_distance(uint text, float along, float across, bool vertical)
{
    uint n = max(sign_string_length(text), 1u);
    float pos = along * float(n);
    uint i = min(uint(pos), n - 1u);
    vec2 local = vertical ? vec2(across, fract(pos)) : vec2(fract(pos), across);
    uint g = sign_string_glyph(text, i);
    // Katakana long-vowel mark turns 90 degrees in vertical writing.
    if (vertical && g == kGlyphChoonpu) local = vec2(local.y, 1.0 - local.x);
    // Leave a margin inside each cell.
    local = (local - 0.5) * 1.12 + 0.5;
    return glyph_distance(g, local);
}

// ---- Procedural video advertisement (original imagery) --------------------------
float sd_circle(vec2 p, float r) { return length(p) - r; }
float sd_capsule(vec2 p, vec2 a, vec2 b, float r)
{
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

vec3 ad_palette(uint h)
{
    const vec3 p[8] = vec3[8](vec3(1.0, 0.35, 0.55), vec3(0.2, 0.9, 1.0), vec3(1.0, 0.75, 0.3), vec3(0.6, 0.35, 1.0),
                              vec3(1.0, 0.2, 0.25), vec3(0.3, 1.0, 0.7), vec3(1.0, 0.95, 0.85), vec3(0.95, 0.5, 0.9));
    return p[h & 7u];
}

vec3 screen_ad(vec2 uv, uint seed, uint text, float t, vec2 size)
{
    // Cycle between creatives every 8 s with a top-down wipe.
    float slot = floor(t / 8.0 + hash_f(seed) * 7.0);
    float phase = fract(t / 8.0 + hash_f(seed) * 7.0);
    if (phase < 0.05 && uv.y > phase / 0.05) slot -= 1.0;  // new creative wipes in from the top
    uint h = hash_u(seed ^ uint(slot) * 2654435761u);
    uint variant = h % 4u;
    vec3 c1 = ad_palette(h), c2 = ad_palette(h >> 3u), c3 = ad_palette(h >> 6u);
    vec2 p = vec2((uv.x - 0.5) * size.x / size.y, uv.y);  // aspect-correct, y down
    vec3 col;

    if (variant == 0u) {
        // Portrait: gradient backdrop, sun disc, figure silhouette with rim light.
        col = mix(c1, c2, uv.y) * 0.9;
        float sun = sd_circle(p - vec2(0.12, 0.32), 0.22);
        col = mix(col, c3 * 1.2, smoothstep(0.01, -0.01, sun));
        float head = sd_circle(p - vec2(-0.05, 0.42), 0.09);
        float body = sd_capsule(p, vec2(-0.05, 0.62), vec2(-0.05, 1.2), 0.2);
        float fig = min(head, body);
        col = mix(col, vec3(0.02, 0.02, 0.04), smoothstep(0.01, -0.01, fig));
        col += c3 * smoothstep(0.02, 0.0, abs(fig)) * 1.5;  // rim
    } else if (variant == 1u) {
        // Soft sky with drifting bokeh.
        col = mix(c1 * 0.6, c2, pow(uv.y, 0.8));
        for (int i = 0; i < 6; ++i) {
            vec2 c = vec2(hash_f(h + uint(i)) - 0.5, fract(hash_f(h ^ uint(i * 7)) - t * 0.03 * (1.0 + float(i) * 0.3)));
            c.x *= size.x / size.y;
            float r = 0.05 + 0.08 * hash_f(h + uint(i) * 13u);
            col += c3 * 0.6 * smoothstep(r, r * 0.6, length(p - c));
        }
    } else if (variant == 2u) {
        // Product stripes + big brand text.
        float stripe = step(0.5, fract((uv.x + uv.y) * 4.0 - t * 0.4));
        col = mix(c1 * 0.8, c2 * 0.8, stripe);
        float band = step(0.35, uv.y) * step(uv.y, 0.65);
        col = mix(col, vec3(0.03), band * 0.85);
        uint n = max(sign_string_length(text), 1u);
        float ta = (uv.x - 0.08) / 0.84;
        float ty = (uv.y - 0.35) / 0.3;
        float d = -1.0;
        // Fit the word into the band keeping square cells.
        float cell_w = 0.84 * size.x / float(n), cell_h = 0.3 * size.y;
        if (cell_w > cell_h) ta = (uv.x - 0.5) * size.x / (cell_h * float(n)) + 0.5;
        else ty = (uv.y - 0.5) * size.y / cell_w + 0.5;
        if (ta > 0.0 && ta < 1.0) d = text_distance(text, ta, ty, false);
        col = mix(col, c3 * 1.6, smoothstep(-0.01, 0.01, d));
    } else {
        // Data wall: scrolling cells.
        vec2 g = vec2(uv.x * 12.0, uv.y * 12.0 * size.y / size.x - t * 0.8);
        float on = step(0.55, hash_f2(ucell(g)));
        vec2 f = fract(g);
        float cellmask = step(0.12, f.x) * step(0.12, f.y);
        col = mix(c1 * 0.15, c2, on * cellmask);
    }
    return col;
}

void main()
{
    Sign s = signs[in_instance];
    uint style = s.style & 0xFFu;
    uint text = (s.style >> 8u) & 0xFFu;
    uint h = s.seed;
    float t = frame.camera_pos.w;
    vec2 uv = in_uv;

#if SIGN_GLOW_PASS
    if (style != kNeonText) discard;
#else
    if (style == kNeonText) discard;
#endif

    vec3 col = neon_color(hash_u(h));
    vec3 col2 = neon_color(hash_u(h ^ 0x51u));
    // HDR level of the tube cores: bright enough to bloom, low enough that the letter
    // shapes survive the tone curve (the target look has readable neon text).
    float intensity = 3.0 + 2.5 * hash_f(h ^ 0x33u);
    // Failing tubes: some signs stutter.
    float broken = hash_f(h ^ 0x99u) < 0.07 ? step(0.3, fract(sin(floor(t * 9.0) + float(h & 1023u)) * 43758.5)) : 1.0;

    // Distance to the sign's edge in metres, for tube borders.
    vec2 e = min(uv, 1.0 - uv) * s.size;
    float edge_d = min(e.x, e.y);
    float border = clamp((0.09 - abs(edge_d - 0.12)) / max(fwidth(edge_d), 1e-3), 0.0, 1.0);

    vec3 emissive = vec3(0.0);
    vec3 plate = vec3(0.012, 0.012, 0.016) + col * 0.03;

    if (style == kNeonText) {
        float d = text_distance(text, uv.x, uv.y, false);
        float aa = fwidth(d) + 1e-4;
        float cov;
        emissive = neon_glyph(d, aa, col, intensity * 1.2, cov) * broken;
        out_color = vec4(emissive * (1.0 - fog_amount(in_world_pos)), 0.0);
        out_material = vec4(0.0);
        return;
    } else if (style == kScreen) {
        vec3 img = screen_ad(uv, h, text, t, s.size);
        // LED pixel structure up close, fading out once it's below a pixel.
        vec2 px = uv * s.size * 12.0;  // ~8 cm pixels
        float grid_fade = 1.0 - smoothstep(0.3, 0.7, max(fwidth(px.x), fwidth(px.y)));
        vec2 f = fract(px);
        float cellmask = mix(1.0, smoothstep(0.0, 0.15, f.x) * smoothstep(0.0, 0.15, f.y) * 1.3, grid_fade);
        float scan = 0.92 + 0.08 * sin(uv.y * 300.0 - t * 20.0);
        float bezel = step(0.012, min(uv.x, 1.0 - uv.x)) * step(0.008, min(uv.y, 1.0 - uv.y));
        emissive = img * 1.5 * cellmask * scan * bezel;
    } else if (style == kRooftop) {
        // Animated billboard with a brand line.
        float scanl = 0.5 + 0.5 * sin(uv.y * 40.0 - t * 3.0);
        vec3 bg = mix(col, col2, uv.x + 0.2 * sin(t * 0.7 + uv.y * 3.0)) * (0.3 + 0.2 * scanl);
        float tw = float(max(sign_string_length(text), 1u)) * s.size.y * 0.5;  // text width in metres at half height
        float ta = (uv.x - 0.5) * s.size.x / tw + 0.5;
        float d = (ta > 0.0 && ta < 1.0) ? text_distance(text, ta, (uv.y - 0.25) / 0.5, false) : -1.0;
        float cov;
        emissive = bg * 1.6 + neon_glyph(d, fwidth(d) + 1e-4, vec3(1.0, 0.95, 0.9), 3.0, cov);
        emissive += border * col * intensity;
    } else if (style == kBlade) {
        // Vertical Japanese text, one glyph per cell down the blade.
        uint n = max(sign_string_length(text), 1u);
        float inner = 0.35 / (float(n) * 0.92 + 0.35) * 0.5;  // top/bottom margin (see place_signs)
        float along = (uv.y - inner) / (1.0 - 2.0 * inner);
        float d = (along > 0.0 && along < 1.0) ? text_distance(text, along, uv.x, true) : -1.0;
        float cov;
        emissive = neon_glyph(d, fwidth(d) + 1e-4, col, intensity, cov) + border * col2 * intensity * 0.7;
    } else {
        // Wall panel: half are backlit light boxes with dark lettering, half neon on black.
        uint n = max(sign_string_length(text), 1u);
        float inner = 0.2 / (float(n) * 0.8 + 0.4);
        float along = (uv.x - inner) / (1.0 - 2.0 * inner);
        float d = (along > 0.0 && along < 1.0) ? text_distance(text, along, uv.y, false) : -1.0;
        float aa = fwidth(d) + 1e-4;
        if (hash_f(h ^ 0x44u) < 0.5) {
            vec3 box = mix(col, vec3(1.0), 0.25) * (0.45 + 0.2 * uv.y);
            float letter = smoothstep(-aa, aa, d);
            emissive = mix(box, vec3(0.02), letter);
        } else {
            float cov;
            emissive = neon_glyph(d, aa, col, intensity, cov) + border * col2 * intensity * 0.6;
        }
    }

    out_color = vec4(apply_fog(plate + emissive * broken, in_world_pos), 1.0);
    out_material = vec4(0.0, 1.0, 0.5, 0.5);
}
