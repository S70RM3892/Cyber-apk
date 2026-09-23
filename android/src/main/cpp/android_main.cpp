// Android runtime: NativeActivity + Vulkan swapchain + touch controls.
//
// Controls (landscape):
//   left half   floating virtual stick: walk; push to the rim to sprint
//   right half  drag to look around
//   keyboard    WASD + Shift, arrow keys to look (emulators / Chromebooks)
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>

#include "apex/game.hpp"
#include "presenter.hpp"
#include "renderer.hpp"

namespace {

using namespace apex;

// ---- Touch / key input ------------------------------------------------------------

class TouchControls {
public:
    void set_screen(float w, float h) {
        width_ = w;
        height_ = h;
    }

    bool on_motion(const AInputEvent* e) {
        const int32_t action = AMotionEvent_getAction(e);
        const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
        const auto index = static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                               AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        switch (masked) {
            case AMOTION_EVENT_ACTION_DOWN:
            case AMOTION_EVENT_ACTION_POINTER_DOWN: {
                const int32_t id = AMotionEvent_getPointerId(e, index);
                const float x = AMotionEvent_getX(e, index), y = AMotionEvent_getY(e, index);
                if (x < width_ * 0.5f && stick_id_ < 0) {
                    stick_id_ = id;
                    stick_origin_x_ = x;
                    stick_origin_y_ = y;
                    stick_x_ = stick_y_ = 0.0f;
                } else if (look_id_ < 0) {
                    look_id_ = id;
                    look_last_x_ = x;
                    look_last_y_ = y;
                }
                return true;
            }
            case AMOTION_EVENT_ACTION_MOVE: {
                const size_t count = AMotionEvent_getPointerCount(e);
                for (size_t i = 0; i < count; ++i) {
                    const int32_t id = AMotionEvent_getPointerId(e, i);
                    const float x = AMotionEvent_getX(e, i), y = AMotionEvent_getY(e, i);
                    if (id == stick_id_) {
                        const float radius = height_ * 0.12f;
                        float dx = (x - stick_origin_x_) / radius, dy = (stick_origin_y_ - y) / radius;
                        const float len = std::sqrt(dx * dx + dy * dy);
                        if (len > 1.0f) {
                            dx /= len;
                            dy /= len;
                        }
                        stick_x_ = dx;
                        stick_y_ = dy;
                        sprint_ = len > 1.15f;
                    } else if (id == look_id_) {
                        const float sens = 3.0f / width_;  // a full-width swipe turns ~170 degrees
                        look_dx_ -= (x - look_last_x_) * sens;
                        look_dy_ -= (y - look_last_y_) * sens;
                        look_last_x_ = x;
                        look_last_y_ = y;
                    }
                }
                return true;
            }
            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_POINTER_UP:
            case AMOTION_EVENT_ACTION_CANCEL: {
                const int32_t id = masked == AMOTION_EVENT_ACTION_CANCEL ? -2 : AMotionEvent_getPointerId(e, index);
                if (id == stick_id_ || id == -2) {
                    stick_id_ = -1;
                    stick_x_ = stick_y_ = 0.0f;
                    sprint_ = false;
                }
                if (id == look_id_ || id == -2) look_id_ = -1;
                return true;
            }
            default:
                return false;
        }
    }

    bool on_key(const AInputEvent* e) {
        const bool down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
        switch (AKeyEvent_getKeyCode(e)) {
            case AKEYCODE_W: keys_[0] = down; return true;
            case AKEYCODE_S: keys_[1] = down; return true;
            case AKEYCODE_A: keys_[2] = down; return true;
            case AKEYCODE_D: keys_[3] = down; return true;
            case AKEYCODE_DPAD_LEFT: keys_[4] = down; return true;
            case AKEYCODE_DPAD_RIGHT: keys_[5] = down; return true;
            case AKEYCODE_DPAD_UP: keys_[6] = down; return true;
            case AKEYCODE_DPAD_DOWN: keys_[7] = down; return true;
            case AKEYCODE_SHIFT_LEFT:
            case AKEYCODE_SHIFT_RIGHT: keys_[8] = down; return true;
            default: return false;
        }
    }

    Input consume(float dt) {
        Input in;
        in.move_x = stick_x_ + (keys_[3] ? 1.0f : 0.0f) - (keys_[2] ? 1.0f : 0.0f);
        in.move_y = stick_y_ + (keys_[0] ? 1.0f : 0.0f) - (keys_[1] ? 1.0f : 0.0f);
        in.move_x = std::clamp(in.move_x, -1.0f, 1.0f);
        in.move_y = std::clamp(in.move_y, -1.0f, 1.0f);
        in.sprint = sprint_ || keys_[8];
        const float key_turn = 1.8f * dt;
        in.look_dx = look_dx_ + (keys_[4] ? key_turn : 0.0f) - (keys_[5] ? key_turn : 0.0f);
        in.look_dy = look_dy_ + (keys_[6] ? key_turn : 0.0f) - (keys_[7] ? key_turn : 0.0f);
        look_dx_ = look_dy_ = 0.0f;
        return in;
    }

private:
    float width_ = 1, height_ = 1;
    int32_t stick_id_ = -1, look_id_ = -1;
    float stick_origin_x_ = 0, stick_origin_y_ = 0, stick_x_ = 0, stick_y_ = 0;
    float look_last_x_ = 0, look_last_y_ = 0, look_dx_ = 0, look_dy_ = 0;
    bool sprint_ = false;
    std::array<bool, 9> keys_{};
};

// ---- App ----------------------------------------------------------------------------

class App {
public:
    explicit App(android_app* app) : app_(app) {}
    ~App() {
        if (!ctx_) return;
        vkDeviceWaitIdle(ctx_->device());
        renderer_.reset();
        presenter_.reset();
        ctx_.reset();
    }

    void on_window_created() {
        vk::ContextDesc desc;
        desc.instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        desc.device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
#ifndef NDEBUG
        desc.enable_validation = true;
#endif
        if (!ctx_) ctx_ = std::make_unique<vk::Context>(desc);
        VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        ci.window = app_->window;
        VkSurfaceKHR surface;
        VK_CHECK(vkCreateAndroidSurfaceKHR(ctx_->instance(), &ci, nullptr, &surface));
        if (!ctx_->device()) {
            ctx_->create_device(desc, surface);
            presenter_ = std::make_unique<Presenter>(*ctx_);
        }
        const VkFormat format = presenter_->format() != VK_FORMAT_UNDEFINED
                                    ? presenter_->format()
                                    : Presenter::choose_format(ctx_->physical_device(), surface);
        presenter_->attach(surface, format);
        const VkExtent2D extent = presenter_->logical_extent();
        controls_.set_screen(static_cast<float>(extent.width), static_cast<float>(extent.height));

        if (!game_) game_ = std::make_unique<Game>(2077);
        if (!renderer_) {
            renderer_ = std::make_unique<Renderer>(*ctx_, format, extent, RenderSettings{});
            world_dirty_ = true;
        } else {
            renderer_->resize(extent);
        }
        last_frame_ = std::chrono::steady_clock::now();
    }

    void on_window_destroyed() {
        if (presenter_) presenter_->detach();
    }

    bool can_render() const { return presenter_ && presenter_->attached() && focused_; }
    void set_focused(bool f) {
        focused_ = f;
        last_frame_ = std::chrono::steady_clock::now();  // no giant dt after resume
    }

    int32_t on_input(AInputEvent* e) {
        switch (AInputEvent_getType(e)) {
            case AINPUT_EVENT_TYPE_MOTION: return controls_.on_motion(e) ? 1 : 0;
            case AINPUT_EVENT_TYPE_KEY: return controls_.on_key(e) ? 1 : 0;
            default: return 0;
        }
    }

    void frame() {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last_frame_).count();
        last_frame_ = now;
        game_->update(dt, controls_.consume(dt));
        world_dirty_ |= game_->take_world_dirty();
        if (presenter_->frame(*game_, *renderer_, world_dirty_)) world_dirty_ = false;
        const VkExtent2D extent = presenter_->logical_extent();
        controls_.set_screen(static_cast<float>(extent.width), static_cast<float>(extent.height));
    }

private:
    android_app* app_;
    std::unique_ptr<vk::Context> ctx_;
    std::unique_ptr<Presenter> presenter_;
    std::unique_ptr<Game> game_;
    std::unique_ptr<Renderer> renderer_;
    TouchControls controls_;
    bool focused_ = false;
    bool world_dirty_ = true;
    std::chrono::steady_clock::time_point last_frame_;
};

void handle_cmd(android_app* app, int32_t cmd) {
    auto* a = static_cast<App*>(app->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) a->on_window_created();
            break;
        case APP_CMD_TERM_WINDOW: a->on_window_destroyed(); break;
        case APP_CMD_GAINED_FOCUS: a->set_focused(true); break;
        case APP_CMD_LOST_FOCUS: a->set_focused(false); break;
        default: break;
    }
}

int32_t handle_input(android_app* app, AInputEvent* e) { return static_cast<App*>(app->userData)->on_input(e); }

}  // namespace

void android_main(android_app* app) {
    App a(app);
    app->userData = &a;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;

    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        // Drain events without blocking while rendering; block while paused.
        while (ALooper_pollOnce(a.can_render() ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) return;
        }
        if (a.can_render()) a.frame();
    }
}
