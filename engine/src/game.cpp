#include "apex/game.hpp"

#include <algorithm>
#include <cmath>

namespace apex {

Vec3 Camera::forward() const {
    const float cp = std::cos(pitch);
    return {std::cos(yaw) * cp, std::sin(yaw) * cp, std::sin(pitch)};
}

Mat4 Camera::view() const { return look_to(position, forward(), Vec3{0, 0, 1}); }

Mat4 Camera::projection(float aspect) const { return perspective_reversed_infinite(fov_y, aspect, znear); }

Game::Game(std::uint64_t seed) : seed_(seed), world_([seed] {
    city::Params p;
    p.seed = seed;
    return p;
}()) {
    world_.update({0, 0, 0});
    world_.wait_ready();
    const Vec3 spawn = world_.find_spawn({0, 0, kEyeHeight});
    camera_.position = {spawn.x, spawn.y, kEyeHeight};
    world_.update(camera_.position);
    world_.wait_ready();
    issue_gig();
}

void Game::issue_gig() {
    // Deterministic from (seed, index): 250-550 m away, snapped to a side-street
    // intersection (lot-grid crossings are never inside a building).
    const std::uint64_t h = city::hash64(seed_ ^ (0x6167ull << 32) ^ gig_.index);
    const float angle = static_cast<float>(h >> 40) * (6.2831853f / 16777216.0f);
    const float dist = 250.0f + static_cast<float>((h >> 16) & 0xFFFF) * (300.0f / 65535.0f);
    const float b = world_.params().block_size;
    const float x = std::round((camera_.position.x + std::cos(angle) * dist) / b) * b;
    const float y = std::round((camera_.position.y + std::sin(angle) * dist) / b) * b;
    gig_.target = {x, y, 0.0f};
    gig_.index += 1;
    gig_.elapsed = 0.0f;
    const float dx = x - camera_.position.x, dy = y - camera_.position.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    gig_.par_time = d / 7.0f + 20.0f;  // a jogging pace plus slack
    gig_.reward = 100.0f + d * 0.8f;
}

void Game::update(float dt, const Input& in) {
    dt = std::min(dt, 0.1f);  // don't tunnel through walls after a hitch
    time_ += dt;

    camera_.yaw += in.look_dx;
    camera_.pitch = std::clamp(camera_.pitch + in.look_dy, -1.45f, 1.45f);

    // Vertical: jump + gravity, feet clamp at street level.
    if (in.jump && on_ground_) {
        vz_ = kJumpSpeed;
        on_ground_ = false;
    }
    if (!on_ground_) {
        vz_ -= kGravity * dt;
        height_ += vz_ * dt;
        if (height_ <= 0.0f) {
            height_ = 0.0f;
            vz_ = 0.0f;
            on_ground_ = true;
        }
    }

    const float speed = in.sprint ? kSprintSpeed : kWalkSpeed;
    const float fx = std::cos(camera_.yaw), fy = std::sin(camera_.yaw);
    const float rx = fy, ry = -fx;  // right of forward in a Z-up world
    Vec3 target = camera_.position;
    target.x += (fx * in.move_y + rx * in.move_x) * speed * dt;
    target.y += (fy * in.move_y + ry * in.move_x) * speed * dt;
    camera_.position = world_.move_with_collision(camera_.position, target, kRadius);
    camera_.position.z = kEyeHeight + height_;

    // Gig progress: full pay within par time, then decaying to 25%.
    gig_.elapsed += dt;
    const float dx = gig_.target.x - camera_.position.x, dy = gig_.target.y - camera_.position.y;
    if (dx * dx + dy * dy < kGigRadius * kGigRadius) {
        const float late = std::max(0.0f, gig_.elapsed - gig_.par_time) / gig_.par_time;
        last_payout_ = static_cast<std::uint32_t>(gig_.reward * std::max(0.25f, 1.0f - late));
        credits_ += last_payout_;
        completed_ += 1;
        last_payout_time_ = time_;
        issue_gig();
    }

    if (world_.update(camera_.position)) world_dirty_ = true;
}

}  // namespace apex
