#include "apex/hero.hpp"

#include "apex/buildgen.hpp"
#include "build_kit.hpp"

namespace apex::hero {

namespace {

using namespace kit;

const V2 kF{std::cos(kHeading), std::sin(kHeading)};   // local +x in world
const V2 kL{-std::sin(kHeading), std::cos(kHeading)};  // local +y in world

V2 w2(float fx, float ly) { return V2{kOriginX, kOriginY} + kF * fx + kL * ly; }
V2 w2(V2 l) { return w2(l.x, l.y); }
Vec3 w3(float fx, float ly, float z) { return at(w2(fx, ly), z); }
V2 wdir(V2 l) { return kF * l.x + kL * l.y; }

// Lowest seed from `start` that satisfies the shader-side hashes we need (colour pick,
// no flicker...), so the authored signs look the same on every run.
template <class Pred>
std::uint32_t pick_seed(std::uint32_t start, Pred pred) {
    for (std::uint32_t s = start;; ++s)
        if (pred(s)) return s;
}
bool steady(std::uint32_t s) { return shader_hash_f(s ^ 0x99u) >= 0.07f; }

std::uint32_t pack(SignStyle st, std::uint32_t text, std::uint32_t zone, std::uint32_t creative = 0) {
    return static_cast<std::uint32_t>(st) | (text << 8) | (zone << 16) | (creative << 24);
}

// Sign whose text runs from local point a to b (left to right as seen from the front),
// centred at height z: the front faces whoever sees a on the left.
SignInstance sign_span(V2 a, V2 b, float z, float height, std::uint32_t seed, std::uint32_t style) {
    const V2 t = wdir((b - a) * (1.0f / len(b - a)));
    const V2 c = w2((a + b) * 0.5f);
    // signs.vert: tangent = (-sin yaw, cos yaw).
    return {c.x, c.y, z, std::atan2(-t.x, t.y), len(b - a), height, seed, style};
}
// Sign centred at local c, facing the local direction `facing` (radians, 0 = +x).
SignInstance sign_facing(V2 c, float facing, float z, float width, float height, std::uint32_t seed,
                         std::uint32_t style) {
    const V2 n{std::cos(facing), std::sin(facing)}, t{-n.y, n.x};
    return sign_span(c - t * (width * 0.5f), c + t * (width * 0.5f), z, height, seed, style);
}

// Japanese / Latin string ids (sign_text_data.hpp).
constexpr std::uint32_t kText24h = 0, kTextMirai = 9, kTextYoichi = 10, kTextHonjitsu = 38;
constexpr std::uint32_t kLatin = 52;  // signtext::kJapaneseCount
constexpr std::uint32_t kHalcyon = kLatin + 2, kSynapse = kLatin + 1, kNeotek = kLatin + 0, kData = kLatin + 22;
constexpr std::uint32_t kNightMarket = kLatin + 17, kHyperion = kLatin + 19, kOrbital = kLatin + 18;
constexpr std::uint32_t kZoneRed = 0, kZonePink = 2;
constexpr float kPiF = std::numbers::pi_v<float>;
constexpr float kFaceCamera = kPiF;  // local yaw of a face looking back at the viewpoint

// Overpass centre line (local x, y, deck top z).
constexpr Vec3 kRamp[] = {{26.0f, 60.0f, 16.0f}, {58.0f, 30.0f, 15.2f}, {118.0f, -10.0f, 5.6f}, {150.0f, -26.0f, 1.4f}};

float dist_to_ramp(V2 p) {
    float best = 1e9f;
    for (std::size_t i = 0; i + 1 < std::size(kRamp); ++i) {
        const V2 a{kRamp[i].x, kRamp[i].y}, b{kRamp[i + 1].x, kRamp[i + 1].y};
        const V2 ab = b - a, ap = p - a;
        const float t = std::clamp((ap.x * ab.x + ap.y * ab.y) / (ab.x * ab.x + ab.y * ab.y), 0.0f, 1.0f);
        best = std::min(best, len(ap - ab * t));
    }
    return best;
}

// World to local.
V2 local_of(float x, float y) {
    const V2 d = V2{x, y} - V2{kOriginX, kOriginY};
    return {d.x * kF.x + d.y * kF.y, d.x * kL.x + d.y * kL.y};
}

// Neon palette entries (neon_color) of the reference: reds, cyans, warm white.
bool on_palette(std::uint32_t h) {
    const std::uint32_t i = h & 15u;
    return i <= 3u || (i >= 8u && i <= 10u) || i == 13u;
}

// Deck height (bottom) of the ramp above the point nearest to p.
float ramp_height(V2 p) {
    float best = 1e9f, z = 0.0f;
    for (std::size_t i = 0; i + 1 < std::size(kRamp); ++i) {
        const V2 a{kRamp[i].x, kRamp[i].y}, b{kRamp[i + 1].x, kRamp[i + 1].y};
        const V2 ab = b - a, ap = p - a;
        const float t = std::clamp((ap.x * ab.x + ap.y * ab.y) / (ab.x * ab.x + ab.y * ab.y), 0.0f, 1.0f);
        const float d = len(ap - ab * t);
        if (d < best) {
            best = d;
            z = kRamp[i].z + (kRamp[i + 1].z - kRamp[i].z) * t - 1.3f;
        }
    }
    return z;
}

city::Building lot_at(float fx, float ly, float footprint, float height, city::District d, bool shanty = false) {
    const V2 p = w2(fx, ly);
    city::Building b{p.x, p.y, footprint, height, 0, d, shanty};
    // The building's LED accent colour comes from its lot hash: shift the lot by whole
    // metres until it lands on the palette.
    for (int k = 0; k < 24; ++k) {
        city::Building c = b;
        c.x += static_cast<float>(k % 5) - 2.0f;
        c.y += static_cast<float>(k / 5) - 2.0f;
        if (on_palette(shader_hash(static_cast<std::uint32_t>(building_hash(c) >> 32) ^ 0xbeefu))) return c;
    }
    return b;
}

// Local-frame footprints the bespoke pieces occupy (no generated lot may overlap them).
struct Rect {
    float x0, y0, x1, y1;
};
constexpr Rect kBespoke[] = {
    {-13.0f, -8.0f, 3.0f, 8.0f},    // the roof the player stands on
    {4.0f, -16.0f, 24.0f, -0.5f},   // 24h shack, gantry posts, billboard
    {8.0f, 6.0f, 22.0f, 20.0f},     // dark block on the left
};

std::vector<Lot> make_lots() {
    std::vector<Lot> out;
    using city::District;
    auto add = [&](city::Building b, bool procedural = true) {
        out.push_back({b, procedural, {}});
        return &out.back();
    };

    // ---- Downtown towers ------------------------------------------------------
    {
        // The big screen tower on the left.
        Lot* t = add(lot_at(190.0f, 48.0f, 56.0f, 100.0f, District::Corporate), false);
        const Massing m = massing_of(t->building);
        const V2 c = local_of(t->building.x, t->building.y);
        const float face = c.x - m.shaft * 0.5f - 1.2f;
        const float z0 = m.base_top + 2.0f, z1 = std::min(m.shaft_top - 4.0f, z0 + 58.0f);
        // Pinned creative 0 (model) with a cyan field: ad_palette(hash) == cyan.
        const std::uint32_t s = pick_seed(900u, [](std::uint32_t v) {
            const std::uint32_t light = shader_hash(v ^ 0x5cu) & 15u;  // sign_light: cyan spill
            return (shader_hash(v) & 7u) == 1u && light >= 8u && light <= 10u;
        });
        t->signs.push_back(sign_facing({face, c.y - 8.0f}, kFaceCamera, (z0 + z1) * 0.5f, 17.0f, z1 - z0, s,
                                       pack(SignStyle::Screen, kHalcyon, kZoneRed, 1)));
        // Corporate name and a vertical blue neon down the left corner.
        const std::uint32_t cs = pick_seed(40u, [](std::uint32_t v) { return steady(v) && shader_hash_f(v ^ 0x44u) >= 0.5f; });
        t->signs.push_back(sign_facing({face - 0.3f, c.y + 14.0f}, kFaceCamera, m.shaft_top - 9.0f, 14.0f, 3.2f, cs,
                                       pack(SignStyle::WallPanel, kSynapse, 1)));
        const std::uint32_t bl = pick_seed(70u, [](std::uint32_t v) { return steady(v) && shader_hash_f(v ^ 0x7eu) >= 0.55f; });
        t->signs.push_back(sign_facing({face - 0.3f, c.y + 20.0f}, kFaceCamera, m.base_top + 34.0f, 2.6f, 40.0f, bl,
                                       pack(SignStyle::Blade, kTextMirai, 0)));
    }
    add(lot_at(236.0f, 104.0f, 28.0f, 74.0f, District::Corporate));
    add(lot_at(150.0f, 122.0f, 30.0f, 38.0f, District::Residential));
    {
        // Centre: the tall one with the red vertical neon.
        Lot* t = add(lot_at(300.0f, -18.0f, 38.0f, 178.0f, District::Corporate));
        const Massing m = massing_of(t->building);
        const V2 c = local_of(t->building.x, t->building.y);
        const float face = c.x - m.shaft * 0.5f - 1.0f;
        const std::uint32_t s = pick_seed(300u, [](std::uint32_t v) { return steady(v) && shader_hash_f(v ^ 0x7eu) < 0.55f; });
        t->signs.push_back(sign_facing({face, c.y + 8.0f}, kFaceCamera, 118.0f, 5.0f, 56.0f, s,
                                       pack(SignStyle::Blade, kTextHonjitsu, kZoneRed)));
        const std::uint32_t w = pick_seed(500u, [](std::uint32_t v) { return steady(v) && shader_hash_f(v ^ 0x44u) < 0.5f; });
        t->signs.push_back(sign_facing({face - 0.2f, c.y - 6.0f}, kFaceCamera, 74.0f, 20.0f, 5.0f, w,
                                       pack(SignStyle::WallPanel, kTextYoichi, 3)));
    }
    {
        // Red billboard high up, right of centre.
        Lot* t = add(lot_at(338.0f, -70.0f, 34.0f, 136.0f, District::Corporate));
        const Massing m = massing_of(t->building);
        const std::uint32_t s = pick_seed(1200u, [](std::uint32_t v) { return (shader_hash(v) & 7u) == 4u; });
        const V2 c = local_of(t->building.x, t->building.y);
        t->signs.push_back(sign_facing({c.x - m.top_footprint * 0.5f + 1.0f, c.y}, kFaceCamera,
                                       136.0f + kBillboardLift + 6.0f, std::min(18.0f, m.top_footprint - 1.0f), 11.0f,
                                       s, pack(SignStyle::Screen, kNeotek, kZoneRed, 3)));
    }
    {
        // Bright white-ish screen behind the centre.
        Lot* t = add(lot_at(368.0f, 6.0f, 30.0f, 112.0f, District::Corporate));
        const Massing m = massing_of(t->building);
        const std::uint32_t s = pick_seed(1500u, [](std::uint32_t v) { return (shader_hash(v) & 7u) == 2u; });
        const V2 c = local_of(t->building.x, t->building.y);
        t->signs.push_back(sign_facing({c.x - m.shaft * 0.5f - 1.0f, c.y}, kFaceCamera, 88.0f, 16.0f, 14.0f, s,
                                       pack(SignStyle::Screen, kData, 1, 3)));
    }
    // Fillers: a wall of lit towers to the right, a few behind on the left.
    const float fill[][4] = {{226, -62, 34, 118}, {262, -118, 40, 150}, {312, -166, 44, 192}, {206, -142, 30, 92},
                             {392, -122, 36, 162}, {432, -40, 40, 214}, {452, -118, 36, 300}, {244, -26, 24, 62},
                             {272, 22, 22, 56},    {262, 150, 40, 128}, {330, 110, 36, 170}, {420, 70, 44, 240},
                             {180, -210, 34, 70},  {350, -230, 40, 140}, {480, 10, 40, 180}, {200, 170, 34, 88}};
    for (const auto& f : fill) add(lot_at(f[0], f[1], f[2], f[3], District::Corporate));

    // ---- Left foreground block --------------------------------------------------
    add(lot_at(15.0f, 13.5f, 12.0f, 13.0f, District::Residential, true), false);

    // ---- Shack market below the roofline ---------------------------------------------
    auto blocked = [&](float fx, float ly, float half) {
        for (const Rect& r : kBespoke)
            if (fx + half > r.x0 && fx - half < r.x1 && ly + half > r.y0 && ly - half < r.y1) return true;
        for (const Lot& l : out) {
            const V2 d = w2(fx, ly) - V2{l.building.x, l.building.y};
            if (std::fabs(d.x) < half + l.building.footprint * 0.5f + 2.0f &&
                std::fabs(d.y) < half + l.building.footprint * 0.5f + 2.0f)
                return true;
        }
        return false;
    };
    std::uint64_t h = 0x4e70a11ull;
    auto rnd = [&h] {
        h = city::hash64(h);
        return unit(h);
    };
    // Dense: 9 m pitch, 1.5-3 m alleys, a mix of one- to four-storey shacks.
    for (float fx = 22.0f; fx < 175.0f; fx += 9.0f)
        for (float ly = -(fx * 0.95f + 8.0f); ly < fx * 0.95f + 8.0f; ly += 9.0f) {
            const float x = fx + (rnd() - 0.5f) * 1.6f, y = ly + (rnd() - 0.5f) * 1.6f;
            const float fp = 6.2f + rnd() * 1.8f;
            if (x < 70.0f && y > -2.5f && y < 6.5f) continue;  // the alley straight ahead
            const float tall = rnd();
            // Low in front so the view drops into the market, taller further out.
            float hgt = x < 45.0f ? 3.5f + rnd() * 3.5f : x < 70.0f ? 4.5f + rnd() * 4.5f : 5.0f + rnd() * 6.0f;
            if (tall > 0.8f && x > 40.0f) hgt += 5.0f + rnd() * 4.0f;
            // Under the ramp only if the deck clears the roof.
            if (dist_to_ramp({x, y}) < 5.0f + fp * 0.5f && hgt + 2.5f > ramp_height({x, y})) continue;
            if (blocked(x, y, fp * 0.5f)) continue;
            add(lot_at(x, y, fp, hgt, District::Residential, true));
        }
    return out;
}

}  // namespace

Vec3 to_world(float fx, float ly, float z) { return w3(fx, ly, z); }

const std::vector<Lot>& lots() {
    static const std::vector<Lot> kLots = make_lots();
    return kLots;
}

bool suppresses(const city::Building& b) {
    // Back to local coordinates.
    const V2 l = local_of(b.x, b.y);
    const float fx = l.x, ly = l.y;
    const float r = b.footprint * 0.5f;
    if (fx + r > -30.0f && fx - r < 300.0f && std::fabs(ly) - r < 30.0f + std::max(fx, 0.0f) * 0.95f) return true;
    for (const Lot& l : lots())
        if (std::fabs(b.x - l.building.x) < r + l.building.footprint * 0.5f + 3.0f &&
            std::fabs(b.y - l.building.y) < r + l.building.footprint * 0.5f + 3.0f)
            return true;
    return false;
}

void build(CitySnapshot& snap) {
    CityMesh& mesh = snap.mesh;
    MeshChunk chunk;
    chunk.first_index = static_cast<std::uint32_t>(mesh.indices.size());
    chunk.first_box = static_cast<std::uint32_t>(mesh.boxes.size());

    // Collision / shading boxes (axis-aligned squares in world space).
    auto instance = [&](float fx, float ly, float footprint, float z1, city::District d, std::uint32_t seed,
                        std::uint32_t flags) {
        const V2 p = w2(fx, ly);
        snap.buildings.push_back({p.x, p.y, footprint, z1, seed, static_cast<std::uint32_t>(d),
                                  BuildingInstance::kMeshed | BuildingInstance::kTopTier | flags, 0.0f});
        return static_cast<std::uint32_t>(snap.buildings.size() - 1);
    };
    const std::uint32_t roof_id = instance(-5.0f, 0.0f, 14.0f, kRoofZ, city::District::Industrial, 0x51a7u, 0);
    const std::uint32_t shack_id = instance(14.0f, -7.0f, 9.0f, 9.5f, city::District::Residential, 0x2417u,
                                            BuildingInstance::kShanty);
    std::vector<PointLight>& lights = snap.point_lights;

    // ---- The roof the player starts on --------------------------------------------
    {
        Builder g(mesh, roof_id, {}, lights);
        const std::vector<V2> plan = {w2(2.0f, -7.0f), w2(2.0f, 7.0f), w2(-12.0f, 7.0f), w2(-12.0f, -7.0f)};
        g.walls(plan, 0.0f, kRoofZ, M::Concrete);
        g.cap(plan, kRoofZ, true, M::Roof);
        // Parapet along the back and sides only: the front edge stays open to the view.
        g.box(w2(-5.0f, 6.85f), kF, 7.0f, 0.15f, kRoofZ, kRoofZ + 1.0f, M::Concrete, M::Concrete, M::Concrete);
        g.box(w2(-5.0f, -6.85f), kF, 7.0f, 0.15f, kRoofZ, kRoofZ + 1.0f, M::Concrete, M::Concrete, M::Concrete);
        g.box(w2(-11.85f, 0.0f), kL, 7.0f, 0.15f, kRoofZ, kRoofZ + 1.0f, M::Concrete, M::Concrete, M::Concrete);
        // Railing on the front edge, low enough to look over.
        for (int i = 0; i <= 7; ++i) {
            const float y = -6.5f + static_cast<float>(i) * (13.0f / 7.0f);
            g.beam(w3(1.8f, y, kRoofZ), w3(1.8f, y, kRoofZ + 0.9f), 0.05f, M::Metal);
        }
        g.beam(w3(1.8f, -6.8f, kRoofZ + 0.9f), w3(1.8f, 6.8f, kRoofZ + 0.9f), 0.06f, M::Metal, 6);
        // Stair hut and AC units.
        g.box(w2(-9.0f, 4.0f), kF, 2.0f, 1.6f, kRoofZ, kRoofZ + 2.8f, M::Concrete, M::Roof, M::Concrete);
        snap.props.push_back({w2(-3.0f, -5.2f).x, w2(-3.0f, -5.2f).y, kRoofZ, kHeading, 0.8f, 0.5f, 1.2f, 0x1200u});
        snap.props.push_back({w2(-5.0f, -5.2f).x, w2(-5.0f, -5.2f).y, kRoofZ, kHeading, 0.8f, 0.5f, 1.2f, 0x3400u});
    }

    // ---- 24h shack: corrugated roof under the rooftop neon -------------------------
    Builder g(mesh, shack_id, {}, lights);
    const V2 kNeonA{20.0f, -2.6f}, kNeonB{12.3f, -8.2f};  // neon text runs from A (left) to B
    {
        // Roof plan (local, counter-clockwise from above), sloping down towards the viewer.
        std::vector<V2> local = {{4.5f, -12.0f}, {7.0f, -14.5f}, {12.8f, -9.4f}, {21.2f, -1.6f}, {8.5f, -3.4f}};
        std::vector<V2> plan;
        for (V2 p : local) plan.push_back(w2(p));
        auto roof_z = [](V2 world) {
            const V2 d = world - V2{kOriginX, kOriginY};
            const float fx = d.x * kF.x + d.y * kF.y;
            return 8.6f + 0.09f * fx;
        };
        g.walls(plan, 0.0f, [&](V2 q) { return roof_z(q) - 0.1f; }, M::ShantyWall);
        std::vector<V2> eave;
        for (V2 p : local) eave.push_back(w2(p + (p - V2{12.0f, -7.5f}) * 0.06f));
        g.cap(eave, [&](V2 q) { return roof_z(q) + 0.08f; }, true, M::Corrugated);
        g.cap(eave, [&](V2 q) { return roof_z(q) - 0.05f; }, false, M::Metal);
        // Pipe along the near edge of the roof, on brackets.
        const V2 pa = local[4] + V2{0.0f, 0.4f}, pb = local[0] + V2{-0.3f, 0.6f};
        const float za = roof_z(w2(pa)) + 0.45f, zb = roof_z(w2(pb)) + 0.45f;
        g.beam(w3(pa.x, pa.y, za), w3(pb.x, pb.y, zb), 0.22f, M::Metal, 8);
        for (int i = 0; i <= 4; ++i) {
            const float t = static_cast<float>(i) / 4.0f;
            const V2 p = pa + (pb - pa) * t;
            const float z = za + (zb - za) * t;
            g.beam(w3(p.x, p.y, z - 0.6f), w3(p.x, p.y, z), 0.08f, M::Metal);
        }
        // The neon's steel frame: a rail under the letters and posts down to the roof.
        const float nz = roof_z(w2(kNeonA)) + 0.35f;
        const V2 dir = (kNeonB - kNeonA) * (1.0f / len(kNeonB - kNeonA));
        const V2 back{-dir.y, dir.x};  // away from the viewer (sign_span's front is (dir.y, -dir.x))
        g.beam(w3(kNeonA.x, kNeonA.y, nz), w3(kNeonB.x, kNeonB.y, nz), 0.12f, M::Metal);
        g.beam(w3(kNeonA.x + back.x * 0.4f, kNeonA.y + back.y * 0.4f, nz + 1.9f),
               w3(kNeonB.x + back.x * 0.4f, kNeonB.y + back.y * 0.4f, nz + 1.9f), 0.08f, M::Metal);
        for (int i = 0; i <= 5; ++i) {
            const V2 p = kNeonA + (kNeonB - kNeonA) * (static_cast<float>(i) / 5.0f) + back * 0.4f;
            g.beam(w3(p.x, p.y, roof_z(w2(p))), w3(p.x, p.y, nz + 2.0f), 0.1f, M::Metal);
        }
        // "24時間営業" in red neon, and its spill on the wet sheet roof.
        const std::uint32_t s = pick_seed(0x24u, [](std::uint32_t v) {
            return steady(v) && shader_hash_f(v ^ 0x7eu) < 0.8f && shader_hash_f(v ^ 0x33u) > 0.75f;
        });
        const float sh = 1.55f;
        snap.signs.push_back(sign_span(kNeonA + back * 0.1f, kNeonB + back * 0.1f, nz + 0.1f + sh * 0.5f, sh, s,
                                       pack(SignStyle::NeonText, kText24h, kZoneRed)));
        for (int i = 0; i < 4; ++i) {
            const V2 p = kNeonA + (kNeonB - kNeonA) * ((static_cast<float>(i) + 0.5f) / 4.0f) - back * 1.2f;
            g.light(w3(p.x, p.y, nz + 0.6f), {1.0f, 0.07f, 0.1f}, 12.0f);
        }
        // AC units and a water drum on the roof.
        snap.props.push_back({w2(10.0f, -9.0f).x, w2(10.0f, -9.0f).y, roof_z(w2(10.0f, -9.0f)), kHeading + 0.5f, 0.7f,
                              0.5f, 1.0f, 0x2200u});
        snap.props.push_back({w2(15.5f, -4.0f).x, w2(15.5f, -4.0f).y, roof_z(w2(15.5f, -4.0f)), kHeading + 0.5f, 0.6f,
                              0.45f, 0.9f, 0x2300u});
    }

    // ---- Steel gantry with red warning lights ------------------------------------
    {
        auto red_lamp = [&](Vec3 p, float blink) {
            snap.lights.push_back({p.x, p.y, p.z, 0.22f, 9.0f, 0.25f, 0.18f, blink});
        };
        const Vec3 posts[2] = {{22.0f, -11.8f, 23.5f}, {35.0f, -5.4f, 20.3f}};
        for (const Vec3& p : posts) {
            g.box(w2(p.x, p.y), kF, 0.35f, 0.35f, 0.0f, p.z, M::Metal, M::Metal, M::Metal);
            red_lamp(w3(p.x - 0.4f, p.y, p.z - 1.0f), 0.0f);
        }
        // Box truss: two top chords and two bottom chords, zigzag lattice between them.
        const Vec3 a{3.0f, -15.5f, 26.8f}, b{46.0f, -3.0f, 19.2f};
        const float depth = 1.5f, width = 1.3f;
        const V2 ab = V2{b.x - a.x, b.y - a.y} * (1.0f / len(V2{b.x - a.x, b.y - a.y}));
        const V2 side{-ab.y * width * 0.5f, ab.x * width * 0.5f};
        const Vec3 top_l{a.x + side.x, a.y + side.y, a.z}, top_r{a.x - side.x, a.y - side.y, a.z};
        const Vec3 tb_l{b.x + side.x, b.y + side.y, b.z}, tb_r{b.x - side.x, b.y - side.y, b.z};
        auto W = [](Vec3 l) { return w3(l.x, l.y, l.z); };
        const Vec3 down{0.0f, 0.0f, -depth};
        g.beam(W(top_l), W(tb_l), 0.28f, M::Metal);
        g.beam(W(top_r), W(tb_r), 0.28f, M::Metal);
        g.beam(W(top_l + down), W(tb_l + down), 0.28f, M::Metal);
        g.beam(W(top_r + down), W(tb_r + down), 0.28f, M::Metal);
        const int n = 16;
        for (int i = 0; i < n; ++i) {
            const float t0 = static_cast<float>(i) / n, t1 = static_cast<float>(i + 1) / n;
            for (int c = 0; c < 2; ++c) {
                const Vec3 s0 = c == 0 ? top_l : top_r, e = c == 0 ? tb_l : tb_r;
                const Vec3 p0 = s0 + (e - s0) * t0, p1 = s0 + (e - s0) * t1;
                g.beam(W((i & 1) ? p0 : p0 + down), W((i & 1) ? p1 + down : p1), 0.1f, M::Metal);
            }
            const Vec3 pl = top_l + (tb_l - top_l) * t0, pr = top_r + (tb_r - top_r) * t0;
            g.beam(W(pl), W(pr), 0.08f, M::Metal);
            if (i % 3 == 1) red_lamp(W(pl + down + Vec3{0, 0, -0.2f}), 0.0f);
        }
        // Posts meet the truss.
        for (const Vec3& p : posts) {
            const float t = std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len(V2{b.x - a.x, b.y - a.y}), 0.0f, 1.0f);
            const Vec3 q = a + (b - a) * t + down;
            g.beam(w3(p.x, p.y, p.z - 3.0f), W(q), 0.18f, M::Metal);
            g.beam(w3(p.x, p.y, p.z), W(q), 0.22f, M::Metal);
        }
        // A second, lighter arm branching left from the far post.
        g.beam(w3(35.0f, -5.4f, 20.3f), w3(44.0f, 4.0f, 17.0f), 0.2f, M::Metal);
        g.beam(w3(35.0f, -5.4f, 18.0f), w3(44.0f, 4.0f, 17.0f), 0.12f, M::Metal);

        // Utility pole with crossarms, transformer cans and a cluster of red lamps.
        const V2 pole{18.5f, -14.2f};
        g.beam(w3(pole.x, pole.y, 0.0f), w3(pole.x, pole.y, 30.0f), 0.36f, M::Metal, 8);
        for (int k = 0; k < 4; ++k) {
            const float z = 21.0f + static_cast<float>(k) * 2.4f;
            const float hw = 2.8f - static_cast<float>(k) * 0.35f;
            const V2 d = V2{0.55f, 0.83f} * hw;
            g.beam(w3(pole.x - d.x, pole.y - d.y, z), w3(pole.x + d.x, pole.y + d.y, z), 0.14f, M::Metal);
            for (int s = -1; s <= 1; s += 2) {
                const V2 e = pole + d * static_cast<float>(s);
                red_lamp(w3(e.x, e.y, z + 0.25f), k == 3 ? 0.7f : 0.0f);
            }
            red_lamp(w3(pole.x - 0.3f, pole.y, z + 0.3f), 0.0f);
        }
        for (int k = 0; k < 3; ++k) {
            const V2 c = pole + V2{-0.5f, static_cast<float>(k - 1) * 0.9f};
            g.beam(w3(c.x, c.y, 18.2f), w3(c.x, c.y, 19.6f), 0.55f, M::Metal, 8);
        }
        g.box(w2(pole.x - 0.4f, pole.y + 1.6f), kF, 0.5f, 0.9f, 15.0f, 16.6f, M::Metal, M::Metal, M::Metal);
        g.light(w3(pole.x - 1.0f, pole.y, 24.0f), {1.0f, 0.08f, 0.06f}, 30.0f);
        g.light(w3(28.0f, -8.5f, 21.0f), {1.0f, 0.08f, 0.06f}, 30.0f);
    }

    // ---- Fashion billboard on the right ----------------------------------------------
    {
        const V2 c{15.6f, -11.0f};
        const V2 t = V2{-0.45f, -0.89f} * (1.0f / len(V2{-0.45f, -0.89f}));  // left edge far, right edge near
        const float w = 3.6f, h = 5.4f, zc = 15.0f;
        const std::uint32_t s = pick_seed(0xb00u, [](std::uint32_t) { return true; });
        snap.signs.push_back(sign_span(c - t * (w * 0.5f), c + t * (w * 0.5f), zc, h, s,
                                       pack(SignStyle::Screen, kHalcyon, kZonePink, 5)));
        const V2 n{-t.y, t.x};  // away from the viewer
        g.box(w2(c + n * 0.25f), wdir(t), w * 0.5f + 0.15f, 0.2f, zc - h * 0.5f - 0.25f, zc + h * 0.5f + 0.25f, M::Metal,
              M::Metal, M::Metal);
        for (int k = -1; k <= 1; k += 2) {
            const V2 p = c + t * (static_cast<float>(k) * (w * 0.5f - 0.3f)) + n * 0.6f;
            g.beam(w3(p.x, p.y, 0.0f), w3(p.x, p.y, zc + h * 0.5f), 0.22f, M::Metal);
        }
        g.box(w2(c - n * 0.5f), wdir(t), w * 0.5f, 0.4f, zc - h * 0.5f - 0.5f, zc - h * 0.5f - 0.38f, M::Metal, M::Metal,
              M::Metal);
    }

    // ---- Lattice billboard frame and lamp post on the left block ------------------------
    {
        const V2 c{17.0f, 11.0f};
        const V2 t = V2{0.6f, -0.8f};  // runs towards the view centre
        const float z0 = 14.0f, z1 = 21.5f, hw = 2.6f;
        for (int k = -1; k <= 1; k += 2) {
            const V2 p = c + t * (static_cast<float>(k) * hw);
            g.beam(w3(p.x, p.y, z0), w3(p.x, p.y, z1), 0.2f, M::Metal);
        }
        for (int i = 0; i < 9; ++i) {
            const float z = z0 + 3.2f + static_cast<float>(i) * 0.5f;
            const V2 p0 = c - t * hw, p1 = c + t * hw;
            g.beam(w3(p0.x, p0.y, z), w3(p1.x, p1.y, z + 0.25f), 0.12f, M::Metal);
        }
        const V2 lp{9.0f, 6.2f};
        g.beam(w3(lp.x, lp.y, 0.0f), w3(lp.x, lp.y, 17.0f), 0.2f, M::Metal, 8);
        g.beam(w3(lp.x, lp.y, 17.0f), w3(lp.x - 0.5f, lp.y - 1.8f, 17.3f), 0.1f, M::Metal);
        g.box(w2(lp.x - 0.5f, lp.y - 1.8f), kF, 0.35f, 0.18f, 16.9f, 17.2f, M::Metal, M::Metal, M::LitPanel);
        g.light(w3(lp.x - 0.5f, lp.y - 1.8f, 16.4f), {0.75f, 0.95f, 1.0f}, 10.0f);
    }

    // ---- Pink vertical market sign in the alley ------------------------------------------
    {
        const std::uint32_t s = pick_seed(0x7a0u, [](std::uint32_t v) { return steady(v) && shader_hash_f(v ^ 0x7eu) < 0.55f; });
        snap.signs.push_back(sign_facing({26.0f, 8.8f}, kFaceCamera + 0.35f, 6.8f, 1.3f, 4.4f, s,
                                         pack(SignStyle::Blade, kTextYoichi, kZonePink)));
        const std::uint32_t s2 = pick_seed(0x7c0u, [](std::uint32_t v) { return steady(v) && shader_hash_f(v ^ 0x44u) < 0.5f; });
        snap.signs.push_back(sign_facing({34.0f, -1.2f}, kFaceCamera - 0.3f, 4.2f, 4.0f, 0.9f, s2,
                                         pack(SignStyle::WallPanel, kNightMarket, 3)));
    }

    // ---- Overpass ramp ----------------------------------------------------------------
    {
        const float half = 4.2f, thick = 1.3f;
        auto side_of = [](Vec3 a, Vec3 b) {
            const V2 d = V2{b.x - a.x, b.y - a.y} * (1.0f / len(V2{b.x - a.x, b.y - a.y}));
            return V2{-d.y, d.x};
        };
        for (std::size_t i = 0; i + 1 < std::size(kRamp); ++i) {
            const Vec3 a = kRamp[i], b = kRamp[i + 1];
            const V2 s = side_of(a, b) * half;
            const Vec3 al{a.x + s.x, a.y + s.y, a.z}, ar{a.x - s.x, a.y - s.y, a.z};
            const Vec3 bl{b.x + s.x, b.y + s.y, b.z}, br{b.x - s.x, b.y - s.y, b.z};
            const Vec3 dn{0, 0, -thick};
            const Vec3 mid = w3((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f - thick * 0.5f);
            auto W = [](Vec3 l) { return w3(l.x, l.y, l.z); };
            g.quad_out(W(al), W(ar), W(br), W(bl), mid, M::Roof);
            g.quad_out(W(al + dn), W(ar + dn), W(br + dn), W(bl + dn), mid, M::Concrete);
            g.quad_out(W(al + dn), W(bl + dn), W(bl), W(al), mid, M::Concrete);
            g.quad_out(W(ar + dn), W(br + dn), W(br), W(ar), mid, M::Concrete);
            // Parapets.
            g.beam(W(al + Vec3{0, 0, 0.45f}), W(bl + Vec3{0, 0, 0.45f}), 0.35f, M::Concrete);
            g.beam(W(ar + Vec3{0, 0, 0.45f}), W(br + Vec3{0, 0, 0.45f}), 0.35f, M::Concrete);
            // Piers and lamps.
            const float seg = len(V2{b.x - a.x, b.y - a.y});
            const int piers = static_cast<int>(seg / 18.0f);
            for (int k = 1; k <= piers; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(piers + 1);
                const Vec3 p = a + (b - a) * t;
                if (p.z - thick > 1.0f)
                    g.box(w2(p.x, p.y), wdir(side_of(a, b)), half * 0.55f, 0.7f, 0.0f, p.z - thick, M::Concrete,
                          M::Concrete, M::Concrete);
                const V2 lamp = V2{p.x, p.y} + side_of(a, b) * (half - 0.3f);
                g.beam(w3(lamp.x, lamp.y, p.z), w3(lamp.x, lamp.y, p.z + 5.0f), 0.12f, M::Metal);
                const Vec3 lw = w3(lamp.x, lamp.y, p.z + 5.0f);
                snap.lights.push_back({lw.x, lw.y, lw.z, 0.3f, 7.0f, 5.0f, 3.0f, 0.0f});
                g.light(w3(lamp.x, lamp.y, p.z + 4.5f), {1.0f, 0.72f, 0.42f}, 18.0f);
            }
        }
    }

    // Everything above: one chunk (generous bounds; the overpass reaches 150 m out).
    chunk.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - chunk.first_index;
    chunk.box_count = static_cast<std::uint32_t>(mesh.boxes.size()) - chunk.first_box;
    const V2 c0 = w2(-20.0f, -120.0f), c1 = w2(160.0f, 120.0f);
    chunk.min[0] = std::min(c0.x, c1.x);
    chunk.min[1] = std::min(c0.y, c1.y);
    chunk.min[2] = 0.0f;
    chunk.max[0] = std::max(c0.x, c1.x);
    chunk.max[1] = std::max(c0.y, c1.y);
    chunk.max[2] = 40.0f;
    if (chunk.index_count || chunk.box_count) mesh.chunks.push_back(chunk);
}

}  // namespace apex::hero
