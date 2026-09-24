// Host-side unit tests for the CPU reference paths. No framework dependency on purpose:
// the tools must build in a bare CI container.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
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
#include "apex/massing.hpp"
#include "apex/materials.hpp"
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

void test_building_meshes() {
    // Detailed meshes: valid indices, unit normals, sane extents, every building meshed,
    // and attachments never cutting through the signs hung on the building.
    city::Params p;
    p.seed = 5;
    const auto snap = build_snapshot(p, 0, 0, 256.0f, 1);
    const CityMesh& m = snap->mesh;
    CHECK(!m.vertices.empty());
    CHECK(m.indices.size() % 3 == 0);
    std::uint32_t max_building = 0;
    for (std::uint32_t i : m.indices) CHECK(i < m.vertices.size());
    for (const MeshVertex& v : m.vertices) {
        const float n2 = static_cast<float>(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz) / (127.0f * 127.0f);
        CHECK(n2 > 0.9f && n2 < 1.1f);
        CHECK(std::isfinite(v.x) && std::isfinite(v.y) && v.z >= -0.01f && v.z < 700.0f);
        CHECK((v.building_material >> 24) <= static_cast<std::uint32_t>(SurfaceMaterial::Lantern));
        max_building = std::max(max_building, v.building_material & 0xFFFFFFu);
    }
    CHECK(max_building < snap->buildings.size());
    for (const BuildingInstance& b : snap->buildings) CHECK(b.flags & BuildingInstance::kMeshed);
    std::uint32_t covered = 0;
    for (const MeshChunk& c : m.chunks) {
        covered += c.index_count;
        CHECK(c.min[0] <= c.max[0] && c.min[1] <= c.max[1]);
        for (std::uint32_t i = c.first_index; i < c.first_index + c.index_count; i += 97) {
            const MeshVertex& v = m.vertices[m.indices[i]];
            CHECK(v.x >= c.min[0] - 1e-3f && v.x <= c.max[0] + 1e-3f && v.y >= c.min[1] - 1e-3f &&
                  v.y <= c.max[1] + 1e-3f && v.z <= c.max[2] + 1e-3f);
        }
    }
    CHECK(covered == m.indices.size());
    // Instanced box parts: valid building, materials and extents, all covered by chunks.
    std::uint32_t boxes_covered = 0;
    for (const MeshChunk& c : m.chunks) boxes_covered += c.box_count;
    CHECK(boxes_covered == m.boxes.size());
    CHECK(!m.boxes.empty());
    for (const BoxInstance& b : m.boxes) {
        CHECK(b.building < snap->buildings.size());
        for (int k = 0; k < 3; ++k)
            CHECK(((b.materials >> (8 * k)) & 0xFFu) <= static_cast<std::uint32_t>(SurfaceMaterial::Lantern));
        CHECK(b.hx > 0.0f && b.hy > 0.0f && b.height > 0.0f && std::isfinite(b.x + b.y + b.z0 + b.yaw));
    }

    // Same inputs, same mesh (tiles must rebuild identically when streamed back in).
    const auto again = build_snapshot(p, 0, 0, 256.0f, 1);
    CHECK(again->mesh.vertices.size() == m.vertices.size() && again->mesh.indices == m.indices);
}

void test_mesh_no_degenerate_normals() {
    // A full streaming window (chamfered towers with recessed waists included): every
    // vertex finite with a unit normal. A zero normal turns into NaN in the shader and
    // the bloom chain spreads it into black blocks.
    city::Params p;
    p.seed = 2077;
    const auto snap = build_snapshot(p, -1, -1, 256.0f, 4);
    std::size_t bad = 0;
    for (const MeshVertex& v : snap->mesh.vertices) {
        const int n2 = v.nx * v.nx + v.ny * v.ny + v.nz * v.nz;
        if (n2 < 110 * 110 || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.u) ||
            std::fabs(v.x) > 1e5f || std::fabs(v.y) > 1e5f)
            ++bad;
    }
    CHECK(bad == 0);
}

void test_material_textures() {
    // Every shipped layer decodes to its array's square size with a full mip chain, and the
    // packed normals are centred (flat on average), so shading isn't tilted.
    const auto t = load_material_textures([](const std::string& name) -> std::optional<std::vector<std::uint8_t>> {
        std::ifstream f(std::string(APEX_ASSET_DIR) + "/" + name, std::ios::binary);
        if (!f) return std::nullopt;
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
    });
    CHECK(t.has_value());
    if (!t) return;
    CHECK(t->size == 1024 && t->mips == 11);
    CHECK(t->nrm_size == 512 && t->nrm_mips == 10);
    CHECK(t->albedo.size() == t->layer_bytes() * MaterialTextures::kLayers);
    CHECK(t->nrm.size() == t->nrm_layer_bytes() * MaterialTextures::kLayers);
    for (std::uint32_t l = 0; l < MaterialTextures::kLayers; ++l) {
        const std::uint8_t* last = t->nrm.data() + (l + 1) * t->nrm_layer_bytes() - 4;  // 1x1 mip
        CHECK(std::abs(static_cast<int>(last[0]) - 128) < 20 && std::abs(static_cast<int>(last[1]) - 128) < 20);
    }
}

void test_light_grid() {
    city::Params p;
    p.seed = 5;
    const auto snap = build_snapshot(p, 0, 0, 256.0f, 1);
    CHECK(snap->point_lights.size() >= snap->signs.size());
    const auto& g = snap->light_grid;
    CHECK(g.size() >= std::size_t{kLightGridCells} * 2);
    const float extent = 3.0f * 256.0f, cell = extent / static_cast<float>(kLightGridSize);
    std::size_t referenced = 0;
    for (std::uint32_t c = 0; c < kLightGridCells; ++c) {
        const std::uint32_t off = g[2 * c], count = g[2 * c + 1];
        CHECK(count <= kMaxLightsPerCell);
        CHECK(off + count <= g.size());
        referenced += count;
        const float cx = -256.0f + (static_cast<float>(c % kLightGridSize) + 0.5f) * cell;
        const float cy = -256.0f + (static_cast<float>(c / kLightGridSize) + 0.5f) * cell;
        for (std::uint32_t k = 0; k < count; ++k) {
            const PointLight& l = snap->point_lights[g[off + k]];
            CHECK(l.radius > 0.0f && l.r >= 0.0f && l.g >= 0.0f && l.b >= 0.0f);
            // The light reaches the cell (plan distance to its centre within radius + half diagonal).
            CHECK(std::hypot(l.x - cx, l.y - cy) <= l.radius + cell * 0.7072f + 1e-3f);
        }
    }
    CHECK(referenced > snap->signs.size());
}

void test_signs_attached() {
    // Every wall-mounted sign must touch a box of its own building: its centre lies within
    // 0.7 m of some box's side (screens hang in front of ledges and fins) (or roof for neon lettering), inside that box's height range.
    city::Params p;
    p.seed = 11;
    int screens = 0, neon_text = 0, checked = 0;
    for (const auto& b : city::generate_tile(p, 0, 0, 1024.0f)) {
        std::vector<BuildingInstance> boxes;
        add_building_boxes(b, boxes);
        std::vector<SignInstance> signs;
        place_signs(b, signs);
        for (const SignInstance& s : signs) {
            screens += s.kind() == SignStyle::Screen;
            neon_text += s.kind() == SignStyle::NeonText;
            if (s.kind() == SignStyle::Blade) continue;  // sticks out perpendicular by design
            bool attached = false;
            // Rooftop billboards stand on a frame above the roof, within its footprint.
            if (s.kind() == SignStyle::Screen && s.z - s.height * 0.5f >= b.height) {
                const Massing m = massing_of(b);
                attached = std::fabs(s.x - b.x) <= m.top_footprint * 0.5f && std::fabs(s.y - b.y) <= m.top_footprint * 0.5f;
                CHECK(attached);
                ++checked;
                continue;
            }
            for (const auto& bx : boxes) {
                const float half = bx.footprint * 0.5f;
                const float dx = std::fabs(s.x - bx.x), dy = std::fabs(s.y - bx.y);
                if (s.kind() == SignStyle::NeonText) {
                    attached |= (bx.flags & BuildingInstance::kTopTier) && dx <= half && dy <= half &&
                                s.z - s.height * 0.5f >= bx.height - 0.01f;
                } else {
                    const float face = std::max(dx, dy);
                    attached |= std::fabs(face - half) < 0.7f && s.z >= bx.base_z && s.z <= bx.height;
                }
            }
            if (!attached)
                std::fprintf(stderr, "  unattached sign kind=%u z=%.1f h=%.1f bld h=%.1f district=%u boxes=%zu\n",
                             static_cast<unsigned>(s.kind()), static_cast<double>(s.z), static_cast<double>(s.height),
                             static_cast<double>(b.height), static_cast<unsigned>(b.district), boxes.size());
            CHECK(attached);
            ++checked;
        }
    }
    CHECK(checked > 100);
    CHECK(screens > 0);
    CHECK(neon_text > 0);
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
            if (pos.z >= b.height - 0.01f) continue;  // standing on its roof (the spawn is on one)
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
    const float floor_z = game.feet_height();  // the spawn roof
    Input in;
    in.jump = true;
    game.update(1.0f / 60.0f, in);
    in.jump = false;
    float peak = 0.0f;
    for (int i = 0; i < 120; ++i) {
        game.update(1.0f / 60.0f, in);
        peak = std::max(peak, game.camera().position.z - Game::kEyeHeight - floor_z);
    }
    CHECK(peak > 0.8f && peak < 1.5f);  // v^2 / 2g = 36 / 32 = 1.125 m
    CHECK(game.on_ground());
    CHECK(near(game.camera().position.z, floor_z + Game::kEyeHeight));
}

void test_rooftops() {
    Game game(2077);
    // Find a shack roof in the streamed snapshot.
    const BuildingInstance* roof = nullptr;
    for (const auto& b : game.world().snapshot()->buildings)
        if ((b.flags & BuildingInstance::kShanty) && b.height > 6.0f && b.footprint > 6.0f) {
            roof = &b;
            break;
        }
    CHECK(roof != nullptr);
    if (!roof) return;
    const BuildingInstance r = *roof;

    // Standing on the roof: stays up there.
    game.set_foot_position({r.x, r.y, r.height});
    Input idle;
    for (int i = 0; i < 30; ++i) game.update(1.0f / 30.0f, idle);
    CHECK(near(game.feet_height(), r.height, 1e-3f));
    CHECK(game.on_ground());

    // Walk off the edge: falls back to the street (or a lower roof).
    Input walk;
    walk.move_y = 1.0f;
    game.camera().yaw = 0.0f;  // +x
    for (int i = 0; i < 90; ++i) game.update(1.0f / 30.0f, walk);
    CHECK(game.feet_height() < r.height - 1.0f);

    // A full charge clears a two-storey roof; a tap does not.
    game.set_foot_position({game.player_position().x, game.player_position().y, 0.0f});
    game.update(1.0f / 60.0f, idle);
    Input hold;
    hold.jump = true;
    for (int i = 0; i < 60; ++i) game.update(1.0f / 60.0f, hold);
    CHECK(game.jump_charge() >= 1.0f);
    const float start = game.feet_height();
    float peak = start;
    for (int i = 0; i < 180; ++i) {
        game.update(1.0f / 60.0f, idle);
        peak = std::max(peak, game.feet_height());
    }
    CHECK(peak - start > 9.0f && peak - start < 13.0f);  // v^2/2g = 361/32 = 11.3 m
    game.world().wait_ready();
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
    test_signs_attached();
    test_building_meshes();
    test_light_grid();
    test_mesh_no_degenerate_normals();
    test_material_textures();
    test_collision();
    test_gigs();
    test_jump();
    test_driving();
    test_rooftops();
    test_audio();
    if (g_failures == 0) std::puts("all tests passed");
    return g_failures == 0 ? 0 : 1;
}
