// Host-side unit tests for the CPU reference paths. No framework dependency on purpose:
// the tools must build in a bare CI container.
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "apex/audio.hpp"
#include "apex/citygen.hpp"
#include "apex/cluster_cull.hpp"
#include "apex/normal_codec.hpp"
#include "apex/specular_aa.hpp"
#include "apex/game.hpp"
#include "apex/world.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

using namespace apex;

void test_normal_codec() {
    const Vec3 samples[] = {{0, 0, 1}, normalize({0.3f, -0.4f, 0.8f}), normalize({0.9f, 0.1f, 0.05f})};
    for (Vec3 n : samples) {
        float u, v;
        encode_normal_xy(n, u, v);
        const Vec3 r = decode_normal_xy(u, v);
        CHECK(near(r.x, n.x) && near(r.y, n.y) && near(r.z, n.z));
    }
    // Compression error pushing x^2 + y^2 past 1 must not produce NaN.
    const Vec3 r = decode_normal_xy(1.0f, 1.0f);
    CHECK(!std::isnan(r.z) && near(length(r), 1.0f));
}

void test_vmf_alpha() {
    // A perfectly flat footprint keeps its roughness.
    CHECK(near(vmf_widened_alpha(0.2f, {0, 0, 1}), 0.2f));
    // More spread (shorter average normal) -> rougher, monotonically.
    const float a1 = vmf_widened_alpha(0.1f, {0, 0, 0.99f});
    const float a2 = vmf_widened_alpha(0.1f, {0, 0, 0.9f});
    const float a3 = vmf_widened_alpha(0.1f, {0, 0, 0.5f});
    CHECK(a1 > 0.1f && a2 > a1 && a3 > a2 && a3 <= 1.0f);
    // Fully cancelling normals -> maximum roughness.
    CHECK(near(vmf_widened_alpha(0.1f, {0, 0, 0}), 1.0f));
}

void test_roughness_bake() {
    // Checkerboard of normals tilted +-30 degrees around X: flat on average, but the
    // highlight must widen as the mip chain averages them.
    constexpr std::uint32_t kSize = 8;
    const float t = std::numbers::pi_v<float> / 6.0f;
    std::vector<Vec3> normals(kSize * kSize);
    std::vector<float> rough(kSize * kSize, 0.2f);
    for (std::uint32_t y = 0; y < kSize; ++y)
        for (std::uint32_t x = 0; x < kSize; ++x)
            normals[y * kSize + x] = ((x + y) & 1) ? Vec3{std::sin(t), 0, std::cos(t)}
                                                   : Vec3{-std::sin(t), 0, std::cos(t)};

    const auto mips = bake_roughness_mips(normals, rough, kSize, kSize);
    CHECK(mips.size() == 4);  // 8, 4, 2, 1
    CHECK(mips.back().width == 1 && mips.back().height == 1);
    CHECK(near(mips[0].roughness[0], 0.2f));  // level 0 is untouched
    CHECK(mips[1].roughness[0] > 0.2f);       // variance folded in from level 1 on
    // Every coarser level sees the same spread here, so roughness is stable, not runaway.
    CHECK(near(mips[1].roughness[0], mips[3].roughness[0], 1e-3f));

    // A flat normal map must not gain roughness at any level.
    std::vector<Vec3> flat(kSize * kSize, Vec3{0, 0, 1});
    for (const auto& m : bake_roughness_mips(flat, rough, kSize, kSize))
        for (float r : m.roughness) CHECK(near(r, 0.2f));

    bool threw = false;
    try {
        bake_roughness_mips(std::vector<Vec3>(12), std::vector<float>(12), 4, 3);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void test_cluster_cull() {
    const Frustum f = make_view_frustum(std::numbers::pi_v<float> / 2.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    const Vec3 cam{0, 0, 0};

    ClusterBounds front{{0, 0, -10}, 1.0f, {0, 0, -10}, {0, 0, 1}, 1.0f};
    ClusterBounds behind{{0, 0, 10}, 1.0f, {0, 0, 10}, {0, 0, 1}, 1.0f};
    ClusterBounds beyond_far{{0, 0, -200}, 1.0f, {0, 0, -200}, {0, 0, 1}, 1.0f};
    ClusterBounds straddling_left{{-19.5f, 0, -10}, 2.0f, {-19.5f, 0, -10}, {0, 0, 1}, 1.0f};
    // Normals all pointing away from the camera within a 30-degree cone -> back-facing.
    ClusterBounds backfacing{{0, 0, -10}, 1.0f, {0, 0, -10}, {0, 0, -1}, std::cos(std::numbers::pi_v<float> / 6.0f)};
    // Same cone, but facing the camera -> kept.
    ClusterBounds frontfacing = backfacing;
    frontfacing.cone_axis = {0, 0, 1};

    const std::vector<ClusterBounds> clusters{front, behind, beyond_far, straddling_left, backfacing, frontfacing};
    const auto visible = cull_clusters(clusters, f, cam);
    CHECK((visible == std::vector<std::uint32_t>{0, 3, 5}));
}

void test_citygen_determinism() {
    city::Params p;
    p.seed = 2077;
    const auto a = city::generate_tile(p, 3, -2, 512.0f);
    const auto b = city::generate_tile(p, 3, -2, 512.0f);
    CHECK(!a.empty());
    CHECK(a.size() == b.size());
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        CHECK(a[i].x == b[i].x && a[i].height == b[i].height && a[i].module_id == b[i].module_id);

    city::Params q = p;
    q.seed = 2078;
    const auto c = city::generate_tile(q, 3, -2, 512.0f);
    bool differs = a.size() != c.size();
    for (std::size_t i = 0; !differs && i < a.size(); ++i) differs = a[i].height != c[i].height;
    CHECK(differs);
}

void test_citygen_tiling() {
    // Expanding a 2x2 tile region must give the same lots as one tile of double size:
    // no building duplicated or lost at tile seams.
    city::Params p;
    p.seed = 42;
    std::set<std::pair<float, float>> small;
    for (int ty = 0; ty < 2; ++ty)
        for (int tx = 0; tx < 2; ++tx)
            for (const auto& bld : city::generate_tile(p, tx, ty, 300.0f)) {
                const bool inserted = small.insert({bld.x, bld.y}).second;
                CHECK(inserted);
            }
    std::set<std::pair<float, float>> big;
    for (const auto& bld : city::generate_tile(p, 0, 0, 600.0f)) big.insert({bld.x, bld.y});
    CHECK(small == big);
}

void test_citygen_roads() {
    // Buildings never overlap arterial roads, and roads actually exist.
    city::Params p;
    p.seed = 7;
    int road_samples = 0;
    std::map<city::District, int> seen;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x) {
            const auto s = city::sample(p, x * 20.0f, y * 20.0f);
            road_samples += s.on_road;
            ++seen[s.district];
        }
    CHECK(road_samples > 0 && road_samples < 200 * 200 / 4);
    CHECK(seen.size() == static_cast<std::size_t>(city::District::Count));

    for (const auto& bld : city::generate_tile(p, 0, 0, 2000.0f)) {
        const auto s = city::sample(p, bld.x, bld.y);
        CHECK(s.border_distance - bld.footprint * 0.7072f >= p.road_half_width - 1e-3f);
    }
}

void test_building_boxes() {
    // Every building is a contiguous stack: tiers touch, shrink upwards, exactly one roof.
    city::Params p;
    p.seed = 99;
    int towers = 0;
    for (const auto& b : city::generate_tile(p, 0, 0, 1024.0f)) {
        std::vector<BuildingInstance> boxes;
        add_building_boxes(b, boxes);
        CHECK(!boxes.empty());
        int tops = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            tops += (boxes[i].flags & BuildingInstance::kTopTier) ? 1 : 0;
            CHECK(boxes[i].height > boxes[i].base_z);
            if (i == 0) CHECK(boxes[i].base_z == 0.0f);
            if (i > 0) {
                CHECK(boxes[i].base_z == boxes[i - 1].height);
                CHECK(boxes[i].footprint <= boxes[i - 1].footprint);
            }
        }
        CHECK(tops == 1);
        CHECK(near(boxes.back().height, b.height));
        towers += boxes.size() == 3;
    }
    CHECK(towers > 0);
}

void test_signs() {
    city::Params p;
    p.seed = 5;
    for (const auto& b : city::generate_tile(p, 0, 0, 512.0f)) {
        std::vector<SignInstance> a, c;
        place_signs(b, a);
        place_signs(b, c);
        CHECK(a.size() == c.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(a[i].x == c[i].x && a[i].z == c[i].z && a[i].seed == c[i].seed);
            CHECK(a[i].z - a[i].height * 0.5f >= 2.5f - 1e-4f);  // head clearance for pedestrians
        }
    }
}

void test_collision() {
    // Walking in a straight line through the city never ends up inside a building.
    Game game(2077);
    Input in;
    in.move_y = 1.0f;
    in.sprint = true;
    for (int i = 0; i < 600; ++i) {
        in.look_dx = (i % 120 == 0) ? 0.9f : 0.0f;  // turn now and then to hit walls at angles
        game.update(1.0f / 30.0f, in);
        const Vec3 pos = game.player_position();
        for (const BuildingInstance& b : game.world().snapshot()->buildings) {
            if (b.base_z > 1.0f || b.height < 1.0f) continue;
            const float half = b.footprint * 0.5f;
            const bool inside = std::fabs(pos.x - b.x) < half && std::fabs(pos.y - b.y) < half;
            CHECK(!inside);
        }
    }
    game.world().wait_ready();
}

void test_gigs() {
    Game game(31337);
    const Gig first = game.gig();
    CHECK(first.index == 1);
    const float dx = first.target.x - game.camera().position.x, dy = first.target.y - game.camera().position.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    CHECK(d > 150.0f && d < 650.0f);
    // Gig targets sit on lot-grid intersections (street crossings, never in a building).
    const float b = game.world().params().block_size;
    CHECK(near(std::fmod(std::fabs(first.target.x), b), 0.0f, 1e-2f) || near(std::fmod(std::fabs(first.target.x), b), b, 1e-2f));

    Input idle;
    game.update(0.016f, idle);
    CHECK(game.credits() == 0);
    game.set_foot_position(first.target);
    game.update(0.016f, idle);
    CHECK(game.gigs_completed() == 1);
    CHECK(game.credits() >= static_cast<std::uint32_t>(first.reward * 0.99f));  // on time: full pay
    CHECK(game.gig().index == 2);
    CHECK(game.gig().target.x != first.target.x || game.gig().target.y != first.target.y);
    game.world().wait_ready();
}

void test_jump() {
    Game game(1);
    Input in;
    in.jump = true;
    game.update(1.0f / 60.0f, in);
    in.jump = false;
    float peak = 0.0f;
    for (int i = 0; i < 120; ++i) {
        game.update(1.0f / 60.0f, in);
        peak = std::max(peak, game.camera().position.z - Game::kEyeHeight);
    }
    CHECK(peak > 0.8f && peak < 1.5f);  // v^2 / 2g = 36 / 32 = 1.125 m
    CHECK(game.on_ground());
    CHECK(near(game.camera().position.z, Game::kEyeHeight));
}

void test_driving() {
    Game game(2077);
    Input in;
    in.toggle_car = true;
    game.update(1.0f / 60.0f, in);
    CHECK(game.mode() == PlayerMode::Driving);
    CHECK(game.car().spawned);
    in.toggle_car = false;
    in.move_y = 1.0f;
    float top = 0.0f;
    for (int i = 0; i < 60 * 6; ++i) {
        in.move_x = (i / 90) % 2 ? 0.4f : -0.3f;  // weave so we hit walls at angles
        game.update(1.0f / 60.0f, in);
        top = std::max(top, game.car().speed);
        const Vec3 c = game.car().position;
        for (const BuildingInstance& b : game.world().snapshot()->buildings) {
            if (b.base_z > 1.0f) continue;
            const float half = b.footprint * 0.5f;
            CHECK(!(std::fabs(c.x - b.x) < half && std::fabs(c.y - b.y) < half));
        }
    }
    CHECK(top > 15.0f && top <= Car::kMaxSpeed);
    // Braking: full reverse input brings the car to a stop, then reverses slowly.
    in.move_x = 0.0f;
    in.move_y = -1.0f;
    for (int i = 0; i < 60 * 4; ++i) game.update(1.0f / 60.0f, in);
    CHECK(game.car().speed < 0.0f && game.car().speed >= -Car::kReverseSpeed);
    // Exit: on foot, next to the car.
    in = {};
    in.toggle_car = true;
    game.update(1.0f / 60.0f, in);
    CHECK(game.mode() == PlayerMode::OnFoot);
    const Vec3 f = game.player_position(), c = game.car().position;
    CHECK(std::hypot(f.x - c.x, f.y - c.y) < 4.0f);
    game.world().wait_ready();
}

double rms(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double e = 0;
    for (std::size_t i = from; i < to; ++i) e += double(v[i]) * v[i];
    return std::sqrt(e / double(to - from));
}

std::vector<float> render_audio(const AudioState& st, float seconds, std::uint32_t payout_at_frame = ~0u) {
    Synth synth(48000.0f);
    synth.set_state(st);
    const int frames = static_cast<int>(seconds * 48000.0f);
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);
    for (int i = 0; i < frames; i += 480) {
        if (payout_at_frame != ~0u && static_cast<std::uint32_t>(i) == payout_at_frame) {
            AudioState p = st;
            p.payouts += 1;
            synth.set_state(p);
        }
        synth.render(out.data() + std::size_t(i) * 2, std::min(480, frames - i));
    }
    return out;
}

void test_audio() {
    AudioState walking;
    walking.player_speed = 4.5f;
    const auto a = render_audio(walking, 6.0f);
    bool finite = true;
    float peak = 0;
    for (float x : a) {
        finite = finite && std::isfinite(x);
        peak = std::max(peak, std::fabs(x));
    }
    CHECK(finite);
    CHECK(peak <= 1.0f);
    const double level = rms(a, 0, a.size());
    CHECK(level > 0.01 && level < 0.5);
    CHECK(render_audio(walking, 1.0f) == render_audio(walking, 1.0f));  // deterministic

    // The chime adds energy right after a payout.
    const std::size_t at = 48000 * 2;  // 2 s in
    const auto quiet = render_audio(walking, 3.0f);
    const auto chime = render_audio(walking, 3.0f, static_cast<std::uint32_t>(at));
    CHECK(rms(chime, at * 2, (at + 24000) * 2) > rms(quiet, at * 2, (at + 24000) * 2) * 1.1);

    // Driving flat out is louder than standing still in the car.
    AudioState idle_car, fast_car;
    idle_car.driving = fast_car.driving = true;
    fast_car.player_speed = 35.0f;
    fast_car.throttle = 1.0f;
    const auto i1 = render_audio(idle_car, 4.0f), f1 = render_audio(fast_car, 4.0f);
    CHECK(rms(f1, f1.size() / 2, f1.size()) > rms(i1, i1.size() / 2, i1.size()));
}

}  // namespace

int main() {
    test_normal_codec();
    test_vmf_alpha();
    test_roughness_bake();
    test_cluster_cull();
    test_citygen_determinism();
    test_citygen_tiling();
    test_citygen_roads();
    test_building_boxes();
    test_signs();
    test_collision();
    test_gigs();
    test_jump();
    test_driving();
    test_audio();
    if (g_failures == 0) std::puts("all tests passed");
    return g_failures == 0 ? 0 : 1;
}
