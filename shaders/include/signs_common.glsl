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

// District colour zones (world.cpp zone_color): two dominant colours per area.
vec3 zone_color(uint zone, uint h)
{
    const vec3 z[8] = vec3[8](vec3(1.0, 0.07, 0.1), vec3(0.05, 0.85, 1.0), vec3(1.0, 0.85, 0.15), vec3(0.95, 0.1, 0.9),
                              vec3(1.0, 0.12, 0.45), vec3(0.25, 0.45, 1.0), vec3(1.0, 0.45, 0.06), vec3(0.15, 0.95, 0.8));
    uint r = h & 7u;
    if (r == 7u) return vec3(1.0, 0.82, 0.68);
    return z[(zone & 3u) * 2u + (r < 4u ? 0u : 1u)];
}

// Neon tube response for a glyph distance d (cell units, + inside): a hot, slightly
// white core with a coloured halo. `aa` is the pixel footprint in cell units.
vec3 neon_glyph(float d, float aa, vec3 col, float intensity, out float coverage)
{
    float fill = smoothstep(-aa, aa, d);
    float core = fill * (0.75 + 0.25 * smoothstep(0.0, 0.04, d));
    // The SDF saturates 0.125 cells outside the outline: subtract the halo's value there so
    // it reaches exactly zero before the cell edge (otherwise cells show as faint boxes).
    float halo = max(exp(-max(-d, 0.0) * 40.0) - exp(-0.12 * 40.0), 0.0) * (1.0 - fill);
    coverage = max(fill, halo);
    vec3 hot = mix(col, vec3(1.0), 0.15);
    return (hot * core + col * halo * 0.35) * intensity;
}

// Pixel footprint in glyph-cell units for cells `cell_m` metres tall. Derived from the
// world position, not fwidth(d): d jumps where one glyph cell meets the next, and its
// derivative there would smear a line along every cell border.
float glyph_aa(float cell_m)
{
    return length(fwidth(in_world_pos)) / max(cell_m, 1e-3) * 1.12 + 1e-4;
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

// Saturated ad colours (no pastels: they read as washed-out noise at night).
vec3 ad_palette(uint h)
{
    const vec3 p[8] = vec3[8](vec3(1.0, 0.08, 0.45), vec3(0.0, 0.85, 1.0), vec3(1.0, 0.82, 0.05), vec3(0.55, 0.1, 1.0),
                              vec3(1.0, 0.12, 0.1), vec3(0.1, 1.0, 0.45), vec3(1.0, 0.45, 0.05), vec3(0.95, 0.2, 0.95));
    return p[h & 7u];
}

const uint kJapaneseStrings = 52u;  // signtext::kJapaneseCount (static_assert in renderer.cpp)

// Word laid out in a box of the ad: box = (x0, y0, x1, y1) in uv; square glyph cells
// sized to fit. Returns the glyph distance (cell units, + inside) or -1 outside.
float ad_text(uint text, vec2 uv, vec4 box, vec2 size, bool vertical)
{
    uint n = max(sign_string_length(text), 1u);
    vec2 q = (uv - box.xy) / (box.zw - box.xy);
    if (any(lessThan(q, vec2(0.0))) || any(greaterThan(q, vec2(1.0)))) return -1.0;
    vec2 box_m = (box.zw - box.xy) * size;  // metres
    if (!vertical) {
        float cell = min(box_m.y, box_m.x / float(n));
        float along = (q.x - 0.5) * box_m.x / (cell * float(n)) + 0.5;
        float across = (q.y - 0.5) * box_m.y / cell + 0.5;
        if (along < 0.0 || along > 1.0) return -1.0;
        return text_distance(text, along, across, false);
    }
    float cell = min(box_m.x, box_m.y / float(n));
    float along = (q.y - 0.5) * box_m.y / (cell * float(n)) + 0.5;
    float across = (q.x - 0.5) * box_m.x / cell + 0.5;
    if (along < 0.0 || along > 1.0) return -1.0;
    return text_distance(text, along, across, true);
}

// Procedural video advertisements (original, fictional brands). Every creative has one
// clear subject and big readable text, so a viewer can say what it advertises.
vec3 screen_ad(vec2 uv, uint seed, uint text, float t, vec2 size)
{
    // Cycle between creatives every 8 s with a top-down wipe.
    float slot = floor(t / 8.0 + hash_f(seed) * 7.0);
    float phase = fract(t / 8.0 + hash_f(seed) * 7.0);
    if (phase < 0.05 && uv.y > phase / 0.05) slot -= 1.0;
    uint h = hash_u(seed ^ uint(slot) * 2654435761u);
    uint variant = h % 4u;
    vec3 c1 = ad_palette(h), c2 = ad_palette(h >> 3u), c3 = ad_palette(h >> 6u);
    if (all(equal(c1, c2))) c2 = ad_palette((h >> 3u) + 3u);
    vec2 p = vec2((uv.x - 0.5) * size.x / size.y, uv.y);  // aspect-correct, y down
    bool portrait = size.y > size.x;
    vec3 col;
    vec4 text_box = portrait ? vec4(0.06, 0.74, 0.94, 0.9) : vec4(0.5, 0.3, 0.95, 0.62);
    vec3 text_col = vec3(1.0);

    if (variant == 0u) {
        // Model: bold two-tone backdrop, a head-and-shoulders silhouette with a rim light.
        col = mix(c1 * 0.25, c1, smoothstep(0.0, 1.0, uv.y + 0.2 * sin(uv.x * 3.0 + t * 0.3)));
        vec2 fp = portrait ? p - vec2(0.0, 0.0) : p - vec2(-0.25 * size.x / size.y, 0.0);
        float head = sd_circle(fp - vec2(0.0, 0.33), 0.13);
        float body = sd_capsule(fp, vec2(0.0, 0.62), vec2(0.0, 1.3), 0.26);
        float fig = min(head, body);
        col = mix(col, vec3(0.015, 0.01, 0.03), smoothstep(0.008, -0.008, fig));
        col += c2 * smoothstep(0.02, 0.0, abs(fig)) * 1.6;  // rim
        // Eyes: a visor stripe in the accent colour.
        col += c2 * 1.5 * step(abs(fp.y - 0.33), 0.018) * step(abs(fp.x), 0.1);
    } else if (variant == 1u) {
        // Product: a can on a dark stage with a light cone, price roundel.
        col = mix(vec3(0.01), c1 * 0.35, smoothstep(1.0, 0.2, uv.y));
        vec2 cp = portrait ? p - vec2(0.0, 0.42) : p - vec2(-0.3 * size.x / size.y, 0.5);
        vec2 d = abs(cp) - vec2(0.13, 0.28);
        float can = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - 0.03;
        vec3 can_col = mix(c2, c2 * 0.35, smoothstep(-0.1, 0.12, cp.x)) + vec3(1.0) * smoothstep(0.02, 0.0, abs(cp.x + 0.06)) * 0.8;
        can_col = mix(can_col, c3, step(abs(cp.y + 0.02), 0.06));  // label band
        col = mix(col, can_col, smoothstep(0.006, -0.006, can));
        float roundel = sd_circle(portrait ? p - vec2(0.22 * size.x / size.y, 0.16) : p - vec2(0.05, 0.2), 0.08);
        col = mix(col, vec3(1.0, 0.85, 0.1), smoothstep(0.006, -0.006, roundel));
        text_col = c2 * 1.4 + 0.2;
    } else if (variant == 2u) {
        // Typographic poster: huge brand name on a colour field, stripes top and bottom.
        col = c1 * 0.8;
        float band = step(0.14, uv.y) * step(uv.y, 0.86);
        col = mix(c2 * 0.9, col, band);
        col = mix(col, vec3(0.02), step(abs(fract((uv.x - uv.y) * 6.0 - t * 0.3) - 0.5), 0.1) * (1.0 - band));
        text_box = vec4(0.05, 0.25, 0.95, 0.75);
        text_col = vec3(0.02);
    } else {
        // Noodle bar: steaming bowl, vertical Japanese name down the side.
        col = mix(vec3(0.08, 0.01, 0.0), c1 * 0.6, uv.y);
        vec2 bp = portrait ? p - vec2(0.0, 0.5) : p - vec2(-0.25 * size.x / size.y, 0.55);
        float bowl = max(length(bp * vec2(1.0, 1.6)) - 0.3, -bp.y);
        col = mix(col, mix(vec3(0.9, 0.1, 0.05), vec3(1.0, 0.9, 0.7), step(0.04, abs(bp.y - 0.08))), smoothstep(0.006, -0.006, bowl));
        for (int i = 0; i < 3; ++i) {  // steam
            float x = float(i - 1) * 0.1 + 0.02 * sin(bp.y * 20.0 - t * 2.0 + float(i));
            col += vec3(0.6) * smoothstep(0.012, 0.0, abs(bp.x - x)) * step(bp.y, -0.02) * step(-0.35, bp.y) * 0.6;
        }
        text = h % kJapaneseStrings;
        text_box = portrait ? vec4(0.72, 0.05, 0.95, 0.7) : vec4(0.6, 0.08, 0.8, 0.92);
        float d = ad_text(text, uv, text_box, size, true);
        col = mix(col, vec3(1.0, 0.95, 0.85), smoothstep(-0.02, 0.02, d));
        return col;
    }
    // Brand line, with a dark plate behind it for contrast.
    float d = ad_text(text, uv, text_box, size, false);
    float plate = step(text_box.x - 0.02, uv.x) * step(uv.x, text_box.z + 0.02) * step(text_box.y - 0.02, uv.y) *
                  step(uv.y, text_box.w + 0.02);
    if (variant != 2u) col = mix(col, col * 0.25, plate * 0.7);
    col = mix(col, text_col, smoothstep(-0.02, 0.02, d));
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

    uint zone = (s.style >> 16u) & 0xFFu;
    vec3 col = zone_color(zone, hash_u(h));
    vec3 col2 = zone_color(zone, hash_u(h ^ 0x51u));
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
        // Big rooftop lettering is red/pink in 80% of cases, like the target look.
        if (hash_f(h ^ 0x7eu) < 0.8) col = zone_color(zone, 0u);  // the zone's primary
        float d = text_distance(text, uv.x, uv.y, false);
        float aa = glyph_aa(s.size.y);
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
        emissive = img * 1.3 * cellmask * scan * bezel;
    } else if (style == kRooftop) {
        // Animated billboard with a brand line.
        float scanl = 0.5 + 0.5 * sin(uv.y * 40.0 - t * 3.0);
        vec3 bg = mix(col, col2, uv.x + 0.2 * sin(t * 0.7 + uv.y * 3.0)) * (0.3 + 0.2 * scanl);
        float tw = float(max(sign_string_length(text), 1u)) * s.size.y * 0.5;  // text width in metres at half height
        float ta = (uv.x - 0.5) * s.size.x / tw + 0.5;
        float d = (ta > 0.0 && ta < 1.0) ? text_distance(text, ta, (uv.y - 0.25) / 0.5, false) : -1.0;
        float cov;
        emissive = bg * 1.6 + neon_glyph(d, glyph_aa(s.size.y * 0.5), vec3(1.0, 0.95, 0.9), 3.0, cov);
        emissive += border * col * intensity;
    } else if (style == kBlade) {
        if (hash_f(h ^ 0x7eu) < 0.55) col = zone_color(zone, 0u);
        // Vertical Japanese text, one glyph per cell down the blade.
        uint n = max(sign_string_length(text), 1u);
        float inner = 0.35 / (float(n) * 0.92 + 0.35) * 0.5;  // top/bottom margin (see place_signs)
        float along = (uv.y - inner) / (1.0 - 2.0 * inner);
        float d = (along > 0.0 && along < 1.0) ? text_distance(text, along, uv.x, true) : -1.0;
        float cov;
        emissive = neon_glyph(d, glyph_aa(s.size.x), col, intensity, cov) + border * col2 * intensity * 0.7;
    } else {
        // Wall panel: half are backlit light boxes with dark lettering, half neon on black.
        uint n = max(sign_string_length(text), 1u);
        float inner = 0.2 / (float(n) * 0.8 + 0.4);
        float along = (uv.x - inner) / (1.0 - 2.0 * inner);
        float d = (along > 0.0 && along < 1.0) ? text_distance(text, along, uv.y, false) : -1.0;
        float aa = glyph_aa(s.size.y);
        if (hash_f(h ^ 0x44u) < 0.5) {
            vec3 box = mix(col, vec3(1.0), 0.25) * (0.45 + 0.2 * uv.y);
            float letter = smoothstep(-aa, aa, d);
            emissive = mix(box, vec3(0.02), letter);
        } else {
            float cov;
            emissive = neon_glyph(d, aa, col, intensity, cov) + border * col2 * intensity * 0.6;
        }
    }

    // LED ads and lit signs are made to be read in daylight: brighter by day.
    emissive *= 1.0 + 1.2 * frame.sun.w;
    out_color = vec4(apply_fog(plate + emissive * broken, in_world_pos), 1.0);
    out_material = vec4(0.0, 1.0, 0.5, 0.5);
}
