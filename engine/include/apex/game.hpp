// Platform-independent game state: player controller (on foot / driving), camera,
// gigs, clock.
#pragma once

#include <algorithm>

#include "apex/mat.hpp"
#include "apex/world.hpp"

namespace apex {

// Per-frame input, already mapped from touch / keyboard by the platform layer.
struct Input {
    float move_x = 0, move_y = 0;    // stick, [-1, 1]; +y = forward/throttle, +x = right/steer
    float look_dx = 0, look_dy = 0;  // radians this frame
    bool sprint = false;
    bool jump = false;        // jump button is held (charge while held, jump on release)
    bool toggle_car = false;  // edge-triggered: summon + enter, or exit
};

// A courier job: reach the beacon before the clock runs down. Reward decays with time.
struct Gig {
    Vec3 target;
    std::uint32_t index = 0;   // how many gigs have been issued
    float reward = 0;          // credits paid if completed now
    float elapsed = 0;         // seconds since the gig was issued
    float par_time = 0;        // seconds for full reward
};

// The player's car: arcade handling, collides with buildings as a circle.
struct Car {
    Vec3 position;       // ground contact point (z = 0)
    float yaw = 0;       // heading, same convention as Camera::yaw
    float speed = 0;     // m/s along the heading (negative = reversing)
    bool spawned = false;

    static constexpr float kMaxSpeed = 38.0f;    // ~137 km/h
    static constexpr float kReverseSpeed = 9.0f;
    static constexpr float kAccel = 11.0f;
    static constexpr float kBrake = 22.0f;
    static constexpr float kDrag = 0.35f;        // 1/s at full speed (quadratic-ish feel)
    static constexpr float kRadius = 1.3f;
};

enum class PlayerMode { OnFoot, Driving };

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
    // Seconds since the last gig was completed (for the HUD's payout flash).
    float since_payout() const { return time_ - last_payout_time_; }
    std::uint32_t last_payout() const { return last_payout_; }
    bool on_ground() const { return on_ground_; }
    // 0..1 while the jump button is held on the ground (for the HUD).
    float jump_charge() const { return std::min(1.0f, jump_charge_ / kChargeTime); }
    float feet_height() const { return foot_position_.z; }
    PlayerMode mode() const { return mode_; }
    const Car& car() const { return car_; }
    // The player's ground position (feet), whichever mode they're in.
    Vec3 player_position() const;
    // Move the player on foot (tests / debug teleports).
    void set_foot_position(Vec3 p) {
        foot_position_ = {p.x, p.y, std::max(p.z, 0.0f)};
        camera_.position = {p.x, p.y, foot_position_.z + kEyeHeight};
    }
    // Horizontal speed of the player (on foot or in the car), m/s.
    float player_speed() const { return player_speed_; }

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
    static constexpr float kJumpSpeed = 6.0f;         // tap
    static constexpr float kChargedJumpSpeed = 19.0f; // full charge: ~11 m, enough for low roofs
    static constexpr float kChargeTime = 0.9f;
    static constexpr float kGravity = 16.0f;  // snappier than 9.81 for a game feel
    static constexpr float kGigRadius = 5.0f;
    static constexpr float kGigRadiusDriving = 9.0f;

private:
    std::uint64_t seed_;
    World world_;
    Camera camera_;
    float time_ = 0;
    bool world_dirty_ = true;
    Gig gig_;
    std::uint32_t credits_ = 0, completed_ = 0, last_payout_ = 0;
    float last_payout_time_ = -1000.0f;
    float vz_ = 0.0f;                  // vertical velocity (feet altitude lives in foot_position_.z)
    bool on_ground_ = true;
    bool jump_was_held_ = false;
    float jump_charge_ = 0.0f;
    PlayerMode mode_ = PlayerMode::OnFoot;
    Car car_;
    Vec3 foot_position_;           // feet position while on foot
    float chase_yaw_offset_ = 0;   // driving: camera orbit relative to the car heading
    float player_speed_ = 0;

    void issue_gig();
    void update_on_foot(float dt, const Input& in);
    void update_driving(float dt, const Input& in);
    void toggle_car();
};

}  // namespace apex
