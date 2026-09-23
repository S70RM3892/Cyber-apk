// Host-side unit tests for the CPU reference paths. No framework dependency on purpose:
// the tools must build in a bare CI container.
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "apex/citygen.hpp"
#include "apex/cluster_cull.hpp"
#include "apex/normal_codec.hpp"
#include "apex/specular_aa.hpp"

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

}  // namespace

int main() {
    test_normal_codec();
    test_vmf_alpha();
    test_roughness_bake();
    test_cluster_cull();
    test_citygen_determinism();
    test_citygen_tiling();
    test_citygen_roads();
    if (g_failures == 0) std::puts("all tests passed");
    return g_failures == 0 ? 0 : 1;
}
