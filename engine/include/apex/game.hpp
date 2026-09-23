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
    bool jump = false;  // edge-triggered: true for the frame the button was pressed
};

// A courier job: reach the beacon before the clock runs down. Reward decays with time.
struct Gig {
    Vec3 target;
    std::uint32_t index = 0;   // how many gigs have been issued
    float reward = 0;          // credits paid if completed now
    float elapsed = 0;         // seconds since the gig was issued
    float par_time = 0;        // seconds for full reward
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
    const Gig& gig() const { return gig_; }
    std::uint32_t credits() const { return credits_; }
    std::uint32_t gigs_completed() const { return completed_; }
    // Seconds since the last gig was completed (for the HUD's payout flash); large if never.
    float since_payout() const { return time_ - last_payout_time_; }
    std::uint32_t last_payout() const { return last_payout_; }
    bool on_ground() const { return on_ground_; }

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
    static constexpr float kJumpSpeed = 6.0f;
    static constexpr float kGravity = 16.0f;  // snappier than 9.81 for a game feel
    static constexpr float kGigRadius = 5.0f;

private:
    std::uint64_t seed_;
    World world_;
    Camera camera_;
    float time_ = 0;
    bool world_dirty_ = true;
    Gig gig_;
    std::uint32_t credits_ = 0, completed_ = 0, last_payout_ = 0;
    float last_payout_time_ = -1000.0f;
    float height_ = 0.0f, vz_ = 0.0f;  // feet above the ground, vertical velocity
    bool on_ground_ = true;

    void issue_gig();
};

}  // namespace apex
