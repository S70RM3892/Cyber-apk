// Platform-independent game state: player controller, camera, clock.
#pragma once

#include "apex/mat.hpp"
#include "apex/world.hpp"

namespace apex {

// Per-frame input, already mapped from touch / keyboard by the platform layer.
struct Input {
    float move_x = 0, move_y = 0;  // stick, [-1, 1]; +y = forward, +x = strafe right
    float look_dx = 0, look_dy = 0;  // radians this frame
    bool sprint = false;
};

struct Camera {
    Vec3 position;
    float yaw = 0, pitch = 0;  // yaw 0 = +X (east), counter-clockwise; pitch + = up
    float fov_y = 1.13f;       // ~65 degrees
    float znear = 0.1f;

    Vec3 forward() const;
    Mat4 view() const;
    Mat4 projection(float aspect) const;
};

class Game {
public:
    explicit Game(std::uint64_t seed);

    void update(float dt, const Input& in);

    World& world() { return world_; }
    const World& world() const { return world_; }
    const Camera& camera() const { return camera_; }
    Camera& camera() { return camera_; }
    float time() const { return time_; }

    // True once when the streamed city changed and GPU buffers must be refreshed.
    bool take_world_dirty() {
        const bool d = world_dirty_;
        world_dirty_ = false;
        return d;
    }

    static constexpr float kEyeHeight = 1.7f;
    static constexpr float kWalkSpeed = 4.5f;
    static constexpr float kSprintSpeed = 11.0f;
    static constexpr float kRadius = 0.35f;

private:
    World world_;
    Camera camera_;
    float time_ = 0;
    bool world_dirty_ = true;
};

}  // namespace apex
