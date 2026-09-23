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

Game::Game(std::uint64_t seed) : world_([seed] {
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
}

void Game::update(float dt, const Input& in) {
    dt = std::min(dt, 0.1f);  // don't tunnel through walls after a hitch
    time_ += dt;

    camera_.yaw += in.look_dx;
    camera_.pitch = std::clamp(camera_.pitch + in.look_dy, -1.45f, 1.45f);

    const float speed = in.sprint ? kSprintSpeed : kWalkSpeed;
    const float fx = std::cos(camera_.yaw), fy = std::sin(camera_.yaw);
    const float rx = fy, ry = -fx;  // right of forward in a Z-up world
    Vec3 target = camera_.position;
    target.x += (fx * in.move_y + rx * in.move_x) * speed * dt;
    target.y += (fy * in.move_y + ry * in.move_x) * speed * dt;
    camera_.position = world_.move_with_collision(camera_.position, target, kRadius);
    camera_.position.z = kEyeHeight;

    if (world_.update(camera_.position)) world_dirty_ = true;
}

}  // namespace apex
