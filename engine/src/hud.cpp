#include "apex/hud.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace apex {

namespace {

struct Glyph {
    char c;
    std::uint8_t rows[7];
};

// Classic 5x7 dot-matrix shapes (HD44780-style), bit 4 = leftmost column.
constexpr Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}}, {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}}, {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}, {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}}, {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}}, {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'|', {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}, {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},
    {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}}, {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'\'', {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},
};

// Neon HUD palette.
constexpr float kCyan[3] = {0.25f, 0.95f, 1.0f};
constexpr float kPink[3] = {1.0f, 0.2f, 0.55f};
constexpr float kYellow[3] = {1.0f, 0.9f, 0.2f};

}  // namespace

const std::uint8_t* glyph_rows(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const Glyph& g : kFont)
        if (g.c == c) return g.rows;
    return nullptr;
}

const char* district_name(city::District d) {
    switch (d) {
        case city::District::Megastructure: return "ARCOLOGY";
        case city::District::Corporate: return "CORPORATE SPIRE";
        case city::District::Residential: return "HIVE BLOCKS";
        case city::District::Industrial: return "RUST YARDS";
        case city::District::Count: break;
    }
    return "";
}

void HudBuilder::rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    quads_.push_back({x, y, w, h, r, g, b, a, static_cast<std::uint32_t>(HudKind::Rect), 0, 0, 0.0f});
}

void HudBuilder::ring(float cx, float cy, float radius, float thickness, float r, float g, float b, float a) {
    quads_.push_back({cx - radius, cy - radius, radius * 2, radius * 2, r, g, b, a,
                      static_cast<std::uint32_t>(HudKind::Ring), 0, 0, thickness});
}

float HudBuilder::text(std::string_view s, float x, float y, float px, float r, float g, float b, float a) {
    float cx = x;
    for (char c : s) {
        if (const std::uint8_t* rows = glyph_rows(c)) {
            std::uint32_t lo = 0, hi = 0;
            for (int i = 0; i < 4; ++i) lo |= std::uint32_t{rows[i]} << (5 * i);
            for (int i = 4; i < 7; ++i) hi |= std::uint32_t{rows[i]} << (5 * (i - 4));
            quads_.push_back({cx, y, 5 * px, 7 * px, r, g, b, a, static_cast<std::uint32_t>(HudKind::Glyph), lo, hi, 0.0f});
        }
        cx += 6 * px;
    }
    return cx - x;
}

HudButton jump_button(float width, float height) {
    const float u = height / 720.0f;
    return {width - 24.0f * u - 90.0f * u, height - 24.0f * u - 150.0f * u, 56.0f * u};
}

HudButton car_button(float width, float height) {
    const HudButton j = jump_button(width, height);
    const float u = height / 720.0f;
    return {j.cx - 20.0f * u, j.cy - 140.0f * u, 44.0f * u};
}

HudButton time_button(float width, float height) {
    // Top-right corner, inside the same safe margins as the rest of the HUD.
    const float u = height / 720.0f, r = 30.0f * u;
    return {width - width * 0.045f - 24.0f * u - r, height * 0.02f + 24.0f * u + r, r};
}

void build_hud(HudBuilder& hud, const Game& game, const HudInput& in) {
    hud.clear();
    // Scale everything from a 720p-tall reference so the HUD is the same physical size
    // across phone resolutions.
    const float u = in.height / 720.0f;
    const float px = 2.0f * u;  // glyph pixel size
    const float margin = 24.0f * u;
    // Safe area: system insets plus room for rounded corners and camera cutouts, which a
    // full-screen game window does not always report (at least 4.5% of the width).
    const float left = std::max(in.inset_left, in.width * 0.045f) + margin;
    const float top = std::max(in.inset_top, in.height * 0.02f) + margin;
    const float right = in.width - std::max(in.inset_right, in.width * 0.045f) - margin;
    const float bottom = in.height - std::max(in.inset_bottom, in.height * 0.03f) - margin;

    // ---- Status panel (top-left) ----
    const Camera& cam = game.camera();
    const city::DistrictSample ds = city::sample(game.world().params(), cam.position.x, cam.position.y);
    char line[64];
    // Sized to its longest line so nothing overhangs the panel.
    std::snprintf(line, sizeof line, "CREDITS %u  GIGS %u", game.credits(), game.gigs_completed());
    const float panel_w = std::max(HudBuilder::text_width("CYBER-APEX", px), HudBuilder::text_width(line, px * 0.9f)) + 24 * u;
    hud.rect(left - 8 * u, top - 8 * u, panel_w, 80 * u, 0.0f, 0.0f, 0.0f, 0.5f);
    hud.rect(left - 8 * u, top - 8 * u, 4 * u, 80 * u, kPink[0], kPink[1], kPink[2], 0.9f);
    hud.text("CYBER-APEX", left + 4 * u, top, px, kPink[0] * 1.5f, kPink[1] * 1.5f, kPink[2] * 1.5f, 1.0f);
    hud.text(ds.on_road ? "ARTERIAL" : district_name(ds.district), left + 4 * u, top + 22 * u, px, kCyan[0],
             kCyan[1], kCyan[2], 1.0f);
    hud.text(line, left + 4 * u, top + 44 * u, px * 0.9f, kYellow[0], kYellow[1], kYellow[2], 1.0f);

    // ---- Performance (top-right, developer builds only) ----
    if (in.show_perf) {
    const float margin_r = in.width - right;
    std::snprintf(line, sizeof line, "%3.0f FPS", static_cast<double>(in.fps));
    const float fps_w = HudBuilder::text_width(line, px);
    hud.text(line, in.width - margin_r - fps_w, top, px, kYellow[0], kYellow[1], kYellow[2], 1.0f);
    std::snprintf(line, sizeof line, "RES %3.0f%%", static_cast<double>(in.render_scale * 100.0f));
    hud.text(line, in.width - margin_r - HudBuilder::text_width(line, px * 0.8f), top + 22 * u, px * 0.8f, 0.8f,
             0.85f, 0.9f, 0.8f);
    if (in.gpu_ms > 0.0f) {
        std::snprintf(line, sizeof line, "GPU %4.1fMS", static_cast<double>(in.gpu_ms));
        hud.text(line, in.width - margin_r - HudBuilder::text_width(line, px * 0.8f), top + 40 * u, px * 0.8f, 0.8f,
                 0.85f, 0.9f, 0.8f);
    }
    }

    // ---- Compass (top-centre) ----
    // Compass heading: 0 = north (+Y), clockwise. Camera yaw: 0 = +X, counter-clockwise.
    const float deg = std::numbers::pi_v<float> / 180.0f;
    const float heading = std::fmod(90.0f - cam.yaw / deg + 720.0f, 360.0f);
    const float cw = 420.0f * u, cx = in.width * 0.5f, cy = top + 6 * u;
    hud.rect(cx - cw * 0.5f, cy + 16 * u, cw, 2 * u, kCyan[0], kCyan[1], kCyan[2], 0.5f);
    const char* labels[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    for (int i = 0; i < 24; ++i) {
        const float a = static_cast<float>(i) * 15.0f;
        float d = std::fmod(a - heading + 540.0f, 360.0f) - 180.0f;  // -180..180
        if (std::fabs(d) > 60.0f) continue;
        const float x = cx + d / 60.0f * cw * 0.5f;
        const float fade = 1.0f - std::fabs(d) / 60.0f;
        if (i % 3 == 0) {
            const char* l = labels[i / 3];
            const float lw = HudBuilder::text_width(l, px);
            const bool north = i == 0;
            hud.text(l, x - lw * 0.5f, cy - 4 * u, px, north ? kPink[0] * 1.4f : 0.9f, north ? kPink[1] : 0.95f,
                     north ? kPink[2] : 1.0f, fade);
        } else {
            hud.rect(x - 1 * u, cy + 10 * u, 2 * u, 6 * u, 0.8f, 0.9f, 1.0f, 0.6f * fade);
        }
    }
    hud.rect(cx - 1.5f * u, cy + 14 * u, 3 * u, 10 * u, kYellow[0], kYellow[1], kYellow[2], 1.0f);

    // ---- Gig tracker: compass marker + distance / reward line ----
    {
        const Gig& gig = game.gig();
        const float dx = gig.target.x - cam.position.x, dy = gig.target.y - cam.position.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        const float bearing = std::fmod(90.0f - std::atan2(dy, dx) / deg + 720.0f, 360.0f);
        float d = std::fmod(bearing - heading + 540.0f, 360.0f) - 180.0f;
        const bool off = std::fabs(d) > 60.0f;
        d = std::clamp(d, -60.0f, 60.0f);
        // On the compass band; an objective behind you pins to the band's end as an arrow.
        const float mx = cx + d / 60.0f * cw * 0.5f;
        if (off) {
            const char* arrow = d < 0.0f ? "<" : ">";
            const float blink = 0.5f + 0.5f * std::sin(game.time() * 8.0f);
            hud.text(arrow, mx - (d < 0.0f ? 0.0f : HudBuilder::text_width(arrow, px * 1.2f)), cy + 22 * u, px * 1.2f,
                     kYellow[0], kYellow[1], kYellow[2], blink);
        } else {
            hud.ring(mx, cy + 26 * u, 6 * u, 0.0f, kYellow[0], kYellow[1], kYellow[2], 1.0f);
        }
        const float late = std::max(0.0f, gig.elapsed - gig.par_time) / gig.par_time;
        const unsigned pay = static_cast<unsigned>(gig.reward * std::max(0.25f, 1.0f - late));
        const float time_left = std::max(0.0f, gig.par_time - gig.elapsed);
        std::snprintf(line, sizeof line, "GIG %u  DIST %.0fM  TIME %.0fS  PAY %u CR", gig.index,
                      static_cast<double>(dist), static_cast<double>(time_left), pay);
        const float lw = HudBuilder::text_width(line, px * 0.9f);
        hud.rect(cx - lw * 0.5f - 10 * u, cy + 46 * u, lw + 20 * u, 26 * u, 0.0f, 0.0f, 0.0f, 0.45f);
        hud.text(line, cx - lw * 0.5f, cy + 52 * u, px * 0.9f, kYellow[0], kYellow[1], kYellow[2], time_left > 0.0f ? 1.0f : 0.6f);

        // Payout flash.
        if (game.since_payout() < 2.5f) {
            const float a = 1.0f - game.since_payout() / 2.5f;
            std::snprintf(line, sizeof line, "GIG COMPLETE  +%u CR", game.last_payout());
            const float w = HudBuilder::text_width(line, px * 1.6f);
            hud.text(line, in.width * 0.5f - w * 0.5f, in.height * 0.32f, px * 1.6f, kYellow[0] * 1.5f,
                     kYellow[1] * 1.5f, kYellow[2], a);
        }
    }

    // ---- Time of day ----
    {
        const HudButton tb = time_button(in.width, in.height);
        hud.ring(tb.cx, tb.cy, tb.radius, 0.0f, 0.0f, 0.0f, 0.0f, 0.3f);
        hud.ring(tb.cx, tb.cy, tb.radius, 0.08f, kYellow[0], kYellow[1], kYellow[2], 0.35f);
        const char* label = in.dusk ? "NIGHT" : "DUSK";
        hud.text(label, tb.cx - HudBuilder::text_width(label, px * 0.7f) * 0.5f, tb.cy - 3.5f * px * 0.7f, px * 0.7f,
                 1.0f, 1.0f, 1.0f, 0.75f);
    }

    // ---- Crosshair ----
    hud.ring(in.width * 0.5f, in.height * 0.5f, 5 * u, 0.35f, 1.0f, 1.0f, 1.0f, 0.7f);

    const bool driving = game.mode() == PlayerMode::Driving;

    // ---- Car button (summon / exit) ----
    {
        const HudButton cb = car_button(in.width, in.height);
        const float a = in.car_held ? 0.55f : 0.12f;
        hud.ring(cb.cx, cb.cy, cb.radius, 0.0f, 0.0f, 0.0f, 0.0f, in.car_held ? 0.5f : 0.3f);  // backing: legible over signs
        hud.ring(cb.cx, cb.cy, cb.radius, 0.08f, kCyan[0], kCyan[1], kCyan[2], a + 0.2f);
        if (in.car_held) hud.ring(cb.cx, cb.cy, cb.radius * 0.9f, 0.0f, kCyan[0], kCyan[1], kCyan[2], 0.25f);
        const char* label = driving ? "EXIT" : "CAR";
        hud.text(label, cb.cx - HudBuilder::text_width(label, px * 0.9f) * 0.5f, cb.cy - 3.5f * px * 0.9f, px * 0.9f,
                 1.0f, 1.0f, 1.0f, 0.7f);
    }

    // ---- Speedometer (driving) ----
    if (driving) {
        std::snprintf(line, sizeof line, "%3.0f", static_cast<double>(std::fabs(game.car().speed) * 3.6f));
        const float big = px * 2.6f;
        const float w = HudBuilder::text_width(line, big);
        const float sx = in.width * 0.5f - w * 0.5f, sy = in.height - margin - 7 * big - 18 * u;
        hud.text(line, sx, sy, big, kCyan[0], kCyan[1], kCyan[2], 0.95f);
        hud.text("KM/H", in.width * 0.5f - HudBuilder::text_width("KM/H", px * 0.8f) * 0.5f, sy + 7 * big + 6 * u,
                 px * 0.8f, 0.8f, 0.9f, 1.0f, 0.8f);
        const float frac = std::clamp(std::fabs(game.car().speed) / Car::kMaxSpeed, 0.0f, 1.0f);
        hud.rect(in.width * 0.5f - 120 * u, sy - 12 * u, 240 * u, 4 * u, 1.0f, 1.0f, 1.0f, 0.15f);
        hud.rect(in.width * 0.5f - 120 * u, sy - 12 * u, 240 * u * frac, 4 * u, kPink[0], kPink[1], kPink[2], 0.9f);
    }

    // ---- Jump button (on foot) ----
    if (!driving) {
        const HudButton jb = jump_button(in.width, in.height);
        const float a = in.jump_held ? 0.55f : 0.12f;
        hud.ring(jb.cx, jb.cy, jb.radius, 0.0f, 0.0f, 0.0f, 0.0f, in.jump_held ? 0.5f : 0.3f);
        hud.ring(jb.cx, jb.cy, jb.radius, 0.08f, kPink[0], kPink[1], kPink[2], a + 0.2f);
        if (in.jump_held) hud.ring(jb.cx, jb.cy, jb.radius * 0.9f, 0.0f, kPink[0], kPink[1], kPink[2], 0.25f);
        // Charge: a growing yellow disc; full = roof-height jump.
        const float charge = game.jump_charge();
        if (charge > 0.1f)
            hud.ring(jb.cx, jb.cy, jb.radius * 0.85f * charge, 0.0f, kYellow[0], kYellow[1], kYellow[2],
                     charge >= 1.0f ? 0.7f : 0.4f);
        hud.text("JUMP", jb.cx - HudBuilder::text_width("JUMP", px * 0.9f) * 0.5f, jb.cy - 3.5f * px * 0.9f,
                 px * 0.9f, 1.0f, 1.0f, 1.0f, 0.7f);
    }

    // ---- Touch stick ----
    if (in.stick.active) {
        const float r = in.stick.radius;
        hud.ring(in.stick.origin_x, in.stick.origin_y, r, 0.06f, kCyan[0], kCyan[1], kCyan[2], 0.5f);
        hud.ring(in.stick.origin_x + in.stick.knob_x * r, in.stick.origin_y - in.stick.knob_y * r, r * 0.38f, 0.0f,
                 kCyan[0], kCyan[1], kCyan[2], 0.35f);
    } else {
        // Hint where the stick lives.
        // Faint while idle: the controls shouldn't cover the view.
        const float r = 56.0f * u;
        const float sx = left + 70 * u, sy = bottom - 70 * u;
        hud.ring(sx, sy, r, 0.05f, 1.0f, 1.0f, 1.0f, 0.12f);
        const char* stick_label = driving ? "DRIVE" : "MOVE";
        hud.text(stick_label, sx - HudBuilder::text_width(stick_label, px * 0.8f) * 0.5f, sy - 6 * u, px * 0.8f, 1.0f,
                 1.0f, 1.0f, 0.25f);
    }
}

}  // namespace apex
