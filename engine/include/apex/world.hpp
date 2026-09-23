// Streams the procedural city around the player (spec §5) and turns it into the
// flat instance arrays the renderer uploads. Expansion runs on a worker thread so
// crossing a tile boundary never stalls the frame.
#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <vector>

#include "apex/citygen.hpp"
#include "apex/math.hpp"

namespace apex {

// GPU layouts (std430-compatible, 16-byte multiples). Keep in sync with shaders/city_common.glsl.
// One box of a building. Towers are stacked from several boxes (podium, shaft,
// crown) that share the building's seed so their facades match.
struct BuildingInstance {
    float x, y, footprint, height;  // height = top of this box (metres)
    std::uint32_t seed, district;
    std::uint32_t flags;             // kTopTier: this box carries the roof of the building
    float base_z;                    // bottom of this box (metres)
    static constexpr std::uint32_t kTopTier = 1;
};
static_assert(sizeof(BuildingInstance) == 32);

enum class SignStyle : std::uint32_t { WallPanel = 0, Blade = 1, Rooftop = 2 };

struct SignInstance {
    float x, y, z, yaw;       // centre (metres), facing direction around +Z
    float width, height;      // metres
    std::uint32_t seed, style;
};
static_assert(sizeof(SignInstance) == 32);

// Distance-to-arterial-road field around the streaming centre. Stored as R8 where
// value/255 * kRoadFieldRange is the distance in metres (saturating), so bilinear
// filtering reconstructs smooth road edges at a coarse texel size.
struct RoadField {
    static constexpr std::uint32_t kSize = 512;
    static constexpr float kRange = 32.0f;
    float origin_x = 0, origin_y = 0;  // world position of texel (0,0)'s corner
    float texel = 1.0f;                // metres per texel
    std::vector<std::uint8_t> data;    // kSize * kSize
};

struct CitySnapshot {
    std::int32_t center_tx = 0, center_ty = 0;
    std::vector<BuildingInstance> buildings;
    std::vector<SignInstance> signs;
    RoadField roads;
};

// Stack of boxes approximating one building's massing (exposed for tests).
void add_building_boxes(const city::Building& b, std::vector<BuildingInstance>& out);

// Deterministic sign placement for one building (exposed for tests).
void place_signs(const city::Building& b, std::vector<SignInstance>& out);

// Build a snapshot synchronously (used by the worker and by tests).
std::shared_ptr<const CitySnapshot> build_snapshot(const city::Params& p, std::int32_t center_tx,
                                                   std::int32_t center_ty, float tile_size,
                                                   std::int32_t radius_tiles);

class World {
public:
    explicit World(const city::Params& params, float tile_size = 256.0f, std::int32_t radius_tiles = 4);
    ~World();

    // Call once per frame. Starts a background rebuild when the player crosses into a
    // new tile and swaps in the result when it's ready. Returns true when a new
    // snapshot became current (renderer must re-upload).
    bool update(Vec3 player_pos);

    // Blocks until any in-flight rebuild has landed (startup / teleports).
    void wait_ready();

    const CitySnapshot* snapshot() const { return current_.get(); }
    const city::Params& params() const { return params_; }

    // Slide a circle of `radius` from `from` towards `to` against building footprints.
    Vec3 move_with_collision(Vec3 from, Vec3 to, float radius) const;

    // A point on an arterial road near `near`, used as a spawn position.
    Vec3 find_spawn(Vec3 near) const;

private:
    city::Params params_;
    float tile_size_;
    std::int32_t radius_tiles_;
    std::shared_ptr<const CitySnapshot> current_;
    std::future<std::shared_ptr<const CitySnapshot>> pending_;
    std::int32_t pending_tx_ = 0, pending_ty_ = 0;
    bool has_pending_ = false;

    void request(std::int32_t tx, std::int32_t ty);
};

}  // namespace apex
