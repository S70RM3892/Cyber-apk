#include "apex/game.hpp"
#include "apex/hero.hpp"

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
    // Start on the rooftop of the hand-built spawn set, facing its view.
    const Vec3 spawn = hero::to_world(0.0f, 0.0f, hero::kRoofZ);
    foot_position_ = spawn;
    camera_.position = {spawn.x, spawn.y, spawn.z + kEyeHeight};
    camera_.yaw = hero::kHeading;
    camera_.pitch = hero::kPitch;
    world_.update(camera_.position);
    world_.wait_ready();
    issue_gig();
}

Vec3 Game::player_position() const { return mode_ == PlayerMode::Driving ? car_.position : foot_position_; }

void Game::issue_gig() {
    // Deterministic from (seed, index): 250-550 m away, snapped to a side-street
    // intersection (lot-grid crossings are never inside a building).
    const Vec3 from = player_position();
    const std::uint64_t h = city::hash64(seed_ ^ (0x6167ull << 32) ^ gig_.index);
    const float angle = static_cast<float>(h >> 40) * (6.2831853f / 16777216.0f);
    const float dist = 250.0f + static_cast<float>((h >> 16) & 0xFFFF) * (300.0f / 65535.0f);
    const float b = world_.params().block_size;
    const float x = std::round((from.x + std::cos(angle) * dist) / b) * b;
    const float y = std::round((from.y + std::sin(angle) * dist) / b) * b;
    gig_.target = {x, y, 0.0f};
    gig_.index += 1;
    gig_.elapsed = 0.0f;
    const float dx = x - from.x, dy = y - from.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    gig_.par_time = d / 7.0f + 20.0f;  // a jogging pace plus slack
    gig_.reward = 100.0f + d * 0.8f;
}

void Game::toggle_car() {
    if (mode_ == PlayerMode::Driving) {
        // Step out to the driver's side (left of the heading), stay where the car is.
        const float lx = -std::sin(car_.yaw), ly = std::cos(car_.yaw);
        const Vec3 door{car_.position.x + lx * 2.2f, car_.position.y + ly * 2.2f, 0.0f};
        foot_position_ = world_.move_with_collision(car_.position, door, kRadius);
        car_.speed = 0.0f;
        camera_.yaw = car_.yaw;
        camera_.pitch = 0.0f;
        mode_ = PlayerMode::OnFoot;
        return;
    }
    // Summon: the car pulls up a few metres ahead (pushed out of any building), or
    // re-enter the parked car if it's close by.
    const float fx = std::cos(camera_.yaw), fy = std::sin(camera_.yaw);
    const float dx = car_.position.x - foot_position_.x, dy = car_.position.y - foot_position_.y;
    if (!car_.spawned || dx * dx + dy * dy > 30.0f * 30.0f) {
        const Vec3 want{foot_position_.x + fx * 5.0f, foot_position_.y + fy * 5.0f, 0.0f};
        car_.position = world_.move_with_collision(foot_position_, want, Car::kRadius);
        car_.yaw = camera_.yaw;
        car_.spawned = true;
    }
    car_.speed = 0.0f;
    chase_yaw_offset_ = 0.0f;
    vz_ = 0.0f;
    on_ground_ = true;
    jump_charge_ = 0.0f;
    foot_position_.z = 0.0f;
    mode_ = PlayerMode::Driving;
}

void Game::update_on_foot(float dt, const Input& in) {
    camera_.yaw += in.look_dx;
    camera_.pitch = std::clamp(camera_.pitch + in.look_dy, -1.45f, 1.45f);

    // Jump: hold to charge (reinforced-tendon style), release to jump; a tap is a hop.
    if (in.jump && on_ground_) jump_charge_ += dt;
    if (!in.jump && jump_was_held_ && on_ground_) {
        const float c = std::min(1.0f, std::max(0.0f, jump_charge_ - 0.12f) / (kChargeTime - 0.12f));
        vz_ = kJumpSpeed + (kChargedJumpSpeed - kJumpSpeed) * c * c;
        on_ground_ = false;
    }
    if (!in.jump) jump_charge_ = 0.0f;
    jump_was_held_ = in.jump;

    const float speed = in.sprint ? kSprintSpeed : kWalkSpeed;
    const float fx = std::cos(camera_.yaw), fy = std::sin(camera_.yaw);
    const float rx = fy, ry = -fx;  // right of forward in a Z-up world
    Vec3 target = foot_position_;
    target.x += (fx * in.move_y + rx * in.move_x) * speed * dt;
    target.y += (fy * in.move_y + ry * in.move_x) * speed * dt;
    const Vec3 before = foot_position_;
    foot_position_ = world_.move_with_collision(foot_position_, target, kRadius);
    const float mdx = foot_position_.x - before.x, mdy = foot_position_.y - before.y;
    player_speed_ = std::sqrt(mdx * mdx + mdy * mdy) / std::max(dt, 1e-4f);

    // Vertical: gravity, land on the street or any roof below; walking off an edge falls.
    const float ground = world_.ground_height(foot_position_.x, foot_position_.y, foot_position_.z);
    if (!on_ground_ || foot_position_.z > ground + 0.01f) {
        on_ground_ = false;
        vz_ -= kGravity * dt;
        foot_position_.z += vz_ * dt;
        const float below = world_.ground_height(foot_position_.x, foot_position_.y, foot_position_.z - vz_ * dt);
        if (foot_position_.z <= below && vz_ <= 0.0f) {
            foot_position_.z = below;
            vz_ = 0.0f;
            on_ground_ = true;
        }
    } else {
        foot_position_.z = ground;  // step up small ledges
    }
    camera_.position = {foot_position_.x, foot_position_.y, foot_position_.z + kEyeHeight};
}

void Game::update_driving(float dt, const Input& in) {
    // Throttle / brake / reverse from the stick's y, steering from its x.
    const float throttle = in.move_y;
    if (throttle > 0.05f) {
        car_.speed += (car_.speed < 0.0f ? Car::kBrake : Car::kAccel) * throttle * dt;
    } else if (throttle < -0.05f) {
        car_.speed += (car_.speed > 0.0f ? -Car::kBrake : -Car::kAccel) * -throttle * dt;
    }
    // Drag grows with speed; rolling resistance brings the car to rest.
    car_.speed -= car_.speed * std::fabs(car_.speed) / Car::kMaxSpeed * Car::kDrag * dt;
    if (std::fabs(throttle) < 0.05f) car_.speed -= std::clamp(car_.speed, -2.0f * dt, 2.0f * dt);
    car_.speed = std::clamp(car_.speed, -Car::kReverseSpeed, Car::kMaxSpeed);

    // Steering: yaw rate scales with speed, softer at high speed; reversed when backing up.
    const float speed_factor = std::clamp(std::fabs(car_.speed) / 8.0f, 0.0f, 1.0f) *
                               (1.0f - 0.45f * std::clamp(std::fabs(car_.speed) / Car::kMaxSpeed, 0.0f, 1.0f));
    const float dir = car_.speed >= 0.0f ? 1.0f : -1.0f;
    car_.yaw -= in.move_x * 1.9f * speed_factor * dir * dt;

    const float fx = std::cos(car_.yaw), fy = std::sin(car_.yaw);
    const Vec3 want{car_.position.x + fx * car_.speed * dt, car_.position.y + fy * car_.speed * dt, 0.0f};
    const Vec3 got = world_.move_with_collision(car_.position, want, Car::kRadius);
    // Hitting a wall scrubs speed in proportion to how much of the move was blocked.
    const float wx = want.x - car_.position.x, wy = want.y - car_.position.y;
    const float gx = got.x - car_.position.x, gy = got.y - car_.position.y;
    const float wanted = std::sqrt(wx * wx + wy * wy);
    if (wanted > 1e-4f) {
        const float along = (gx * wx + gy * wy) / (wanted * wanted);
        if (along < 0.98f) car_.speed *= std::clamp(along, 0.0f, 1.0f) * 0.9f;
    }
    car_.position = got;
    player_speed_ = std::fabs(car_.speed);

    // Chase camera: orbit offset from look drags, easing back behind the car.
    chase_yaw_offset_ += in.look_dx;
    if (in.look_dx == 0.0f) chase_yaw_offset_ *= std::exp(-1.5f * dt);
    const float cam_yaw = car_.yaw + chase_yaw_offset_;
    const float back = 7.5f + std::fabs(car_.speed) * 0.05f;
    const Vec3 eye_target{car_.position.x - std::cos(cam_yaw) * back, car_.position.y - std::sin(cam_yaw) * back, 3.0f};
    // Keep the camera out of buildings by sliding it towards the car.
    const Vec3 eye = world_.move_with_collision(car_.position, eye_target, 0.3f);
    camera_.position = {eye.x, eye.y, 3.0f};
    const Vec3 look_at{car_.position.x, car_.position.y, 1.3f};
    const float dx = look_at.x - eye.x, dy = look_at.y - eye.y, dz = look_at.z - camera_.position.z;
    camera_.yaw = std::atan2(dy, dx);
    camera_.pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) + 0.08f;
}

void Game::update(float dt, const Input& in) {
    dt = std::min(dt, 0.1f);  // don't tunnel through walls after a hitch
    time_ += dt;

    if (in.toggle_car) toggle_car();
    if (mode_ == PlayerMode::Driving) update_driving(dt, in);
    else update_on_foot(dt, in);

    // Gig progress: full pay within par time, then decaying to 25%.
    gig_.elapsed += dt;
    const Vec3 p = player_position();
    const float dx = gig_.target.x - p.x, dy = gig_.target.y - p.y;
    const float r = mode_ == PlayerMode::Driving ? kGigRadiusDriving : kGigRadius;
    if (dx * dx + dy * dy < r * r) {
        const float late = std::max(0.0f, gig_.elapsed - gig_.par_time) / gig_.par_time;
        last_payout_ = static_cast<std::uint32_t>(gig_.reward * std::max(0.25f, 1.0f - late));
        credits_ += last_payout_;
        completed_ += 1;
        last_payout_time_ = time_;
        issue_gig();
    }

    if (world_.update(p)) world_dirty_ = true;
}

}  // namespace apex
