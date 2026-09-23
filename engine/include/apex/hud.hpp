// Heads-up display, built on the CPU as a list of instanced quads each frame:
// rectangles, rings/discs and 5x7 bitmap glyphs (the glyph bits travel with the quad,
// so the shader needs no font texture).
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "apex/game.hpp"

namespace apex {

enum class HudKind : std::uint32_t { Rect = 0, Ring = 1, Glyph = 2 };

// Keep in sync with shaders/hud.vert.
struct HudQuad {
    float x, y, w, h;          // logical pixels, origin top-left
    float r, g, b, a;          // straight alpha; rgb may exceed 1 for glow
    std::uint32_t kind;        // HudKind
    std::uint32_t bits_lo;     // glyph rows 0-3 (5 bits each)
    std::uint32_t bits_hi;     // glyph rows 4-6
    float param;               // ring thickness (0..1 of radius); 0 = filled disc
};
static_assert(sizeof(HudQuad) == 48);

struct StickState {
    bool active = false;
    float origin_x = 0, origin_y = 0;  // logical pixels
    float knob_x = 0, knob_y = 0;      // normalised deflection, [-1, 1], +y up
    float radius = 100;                // logical pixels
};

struct HudInput {
    float width = 1, height = 1;  // logical size
    float fps = 0;
    float gpu_ms = 0;
    float render_scale = 1;
    StickState stick;
    bool jump_held = false;
    bool car_held = false;
};

class HudBuilder {
public:
    void clear() { quads_.clear(); }
    const std::vector<HudQuad>& quads() const { return quads_; }

    void rect(float x, float y, float w, float h, float r, float g, float b, float a);
    void ring(float cx, float cy, float radius, float thickness, float r, float g, float b, float a);
    // Draws ASCII text (unsupported characters render as blanks). Returns the width.
    float text(std::string_view s, float x, float y, float px, float r, float g, float b, float a);
    static float text_width(std::string_view s, float px) { return static_cast<float>(s.size()) * 6.0f * px; }

private:
    std::vector<HudQuad> quads_;
};

// On-screen button geometry, shared by the HUD and the touch hit-test.
struct HudButton {
    float cx, cy, radius;
    bool contains(float x, float y) const {
        const float dx = x - cx, dy = y - cy;
        return dx * dx + dy * dy <= radius * radius * 1.3f;  // a little forgiving
    }
};
HudButton jump_button(float width, float height);
HudButton car_button(float width, float height);

// Lay out the game HUD (status panel, compass, gig tracker, crosshair, controls).
void build_hud(HudBuilder& hud, const Game& game, const HudInput& in);

// 5x7 glyph rows (bit 4 = leftmost pixel) for printable ASCII, or nullptr.
const std::uint8_t* glyph_rows(char c);

const char* district_name(city::District d);

}  // namespace apex
