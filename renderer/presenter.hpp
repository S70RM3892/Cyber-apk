// Swapchain presentation + frame pacing, shared by the Android runtime and the host's
// headless-surface test so the acquire/submit/present path is exercised off-device.
#pragma once

#include <array>
#include <vector>

#include "renderer.hpp"

namespace apex {

class Presenter {
public:
    explicit Presenter(vk::Context& ctx);
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    static VkFormat choose_format(VkPhysicalDevice gpu, VkSurfaceKHR surface);

    // Takes ownership of `surface`. `fallback_extent` is used when the surface doesn't
    // dictate a size (headless / some desktop WSI).
    void attach(VkSurfaceKHR surface, VkFormat format, VkExtent2D fallback_extent = {1280, 720});
    // Destroys swapchain and surface (Android: window went away).
    void detach();
    bool attached() const { return swapchain_ != VK_NULL_HANDLE; }

    VkFormat format() const { return format_; }
    VkExtent2D logical_extent() const { return logical_; }
    int rotation() const { return rotation_; }

    // Acquire -> record -> submit -> present. Handles out-of-date / suboptimal
    // swapchains (Android rotation) by recreating and resizing the renderer.
    // Returns false if the frame was skipped.
    bool frame(const Game& game, Renderer& renderer, bool world_dirty);

    std::uint64_t frames_presented() const { return presented_; }

private:
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
    };

    vk::Context& ctx_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D physical_{}, logical_{}, fallback_{};
    int rotation_ = 0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> render_done_;  // per image: safe to reuse once the image is re-acquired
    std::array<Slot, Renderer::kFramesInFlight> slots_{};
    std::uint32_t slot_index_ = 0;
    std::uint64_t presented_ = 0;

    void create_swapchain();
    void destroy_swapchain_images();
    void recreate(Renderer& renderer);
    void reset_acquire_semaphores();
};

}  // namespace apex
