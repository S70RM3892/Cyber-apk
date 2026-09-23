#include "presenter.hpp"

#include <algorithm>

namespace apex {

Presenter::Presenter(vk::Context& ctx) : ctx_(ctx) {
    for (Slot& s : slots_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = ctx_.queue_family();
        VK_CHECK(vkCreateCommandPool(ctx_.device(), &pci, nullptr, &s.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx_.device(), &ai, &s.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(ctx_.device(), &fi, nullptr, &s.fence));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(ctx_.device(), &si, nullptr, &s.acquired));
    }
}

Presenter::~Presenter() {
    vkDeviceWaitIdle(ctx_.device());
    detach();
    for (Slot& s : slots_) {
        vkDestroySemaphore(ctx_.device(), s.acquired, nullptr);
        vkDestroyFence(ctx_.device(), s.fence, nullptr);
        vkDestroyCommandPool(ctx_.device(), s.pool, nullptr);
    }
}

VkFormat Presenter::choose_format(VkPhysicalDevice gpu, VkSurfaceKHR surface) {
    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, formats.data());
    for (VkFormat want : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_FORMAT_B8G8R8A8_UNORM})
        for (const auto& f : formats)
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return want;
    return formats.empty() ? VK_FORMAT_R8G8B8A8_UNORM : formats[0].format;
}

void Presenter::attach(VkSurfaceKHR surface, VkFormat format, VkExtent2D fallback_extent) {
    surface_ = surface;
    format_ = format;
    fallback_ = fallback_extent;
    create_swapchain();
}

void Presenter::detach() {
    if (!surface_) return;
    vkDeviceWaitIdle(ctx_.device());
    destroy_swapchain_images();
    if (swapchain_) vkDestroySwapchainKHR(ctx_.device(), swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    vkDestroySurfaceKHR(ctx_.instance(), surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
}

void Presenter::destroy_swapchain_images() {
    for (VkImageView v : views_) vkDestroyImageView(ctx_.device(), v, nullptr);
    for (VkSemaphore s : render_done_) vkDestroySemaphore(ctx_.device(), s, nullptr);
    views_.clear();
    render_done_.clear();
    images_.clear();
}

void Presenter::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical_device(), surface_, &caps));

    // Pre-rotation (developer.android.com/games/optimize/vulkan-prerotation): keep the
    // swapchain in the display's identity orientation, set preTransform to the current
    // transform, and rotate in the final pass so the compositor doesn't have to.
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(fallback_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(fallback_.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    rotation_ = 0;
    switch (transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: rotation_ = 90; break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: rotation_ = 180; break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: rotation_ = 270; break;
        case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR: break;
        default:
            // Mirrored/inherit transforms: let the compositor handle them.
            transform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                            : caps.currentTransform;
            break;
    }
    if (rotation_ == 90 || rotation_ == 270) std::swap(extent.width, extent.height);
    physical_ = extent;
    logical_ = (rotation_ == 90 || rotation_ == 270) ? VkExtent2D{extent.height, extent.width} : extent;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (auto a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                   VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
        if (caps.supportedCompositeAlpha & a) {
            alpha = a;
            break;
        }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = format_;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = physical_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = transform;
    ci.compositeAlpha = alpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync: always available and the most power-efficient
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;
    VkSwapchainKHR fresh;
    VK_CHECK(vkCreateSwapchainKHR(ctx_.device(), &ci, nullptr, &fresh));
    destroy_swapchain_images();
    if (swapchain_) vkDestroySwapchainKHR(ctx_.device(), swapchain_, nullptr);
    swapchain_ = fresh;

    std::uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &n, images_.data());
    for (VkImage img : images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        VK_CHECK(vkCreateImageView(ctx_.device(), &vi, nullptr, &view));
        views_.push_back(view);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem;
        VK_CHECK(vkCreateSemaphore(ctx_.device(), &si, nullptr, &sem));
        render_done_.push_back(sem);
    }
    APEX_LOGI("swapchain %ux%u (logical %ux%u, rotation %d), %u images, format %d", physical_.width,
              physical_.height, logical_.width, logical_.height, rotation_, n, int(format_));
}

void Presenter::reset_acquire_semaphores() {
    // A failed acquire can leave its semaphore in an undefined state: replace them.
    for (Slot& s : slots_) {
        vkDestroySemaphore(ctx_.device(), s.acquired, nullptr);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(ctx_.device(), &si, nullptr, &s.acquired));
    }
}

void Presenter::recreate(Renderer& renderer) {
    vkDeviceWaitIdle(ctx_.device());
    reset_acquire_semaphores();
    const VkExtent2D old = logical_;
    create_swapchain();
    if (old.width != logical_.width || old.height != logical_.height) renderer.resize(logical_);
}

bool Presenter::frame(const Game& game, Renderer& renderer, bool world_dirty, std::span<const HudQuad> hud) {
    Slot& slot = slots_[slot_index_];
    VK_CHECK(vkWaitForFences(ctx_.device(), 1, &slot.fence, VK_TRUE, UINT64_MAX));
    if (world_dirty) renderer.upload_world(*game.world().snapshot());

    std::uint32_t image = 0;
    const VkResult acq =
        vkAcquireNextImageKHR(ctx_.device(), swapchain_, UINT64_MAX, slot.acquired, VK_NULL_HANDLE, &image);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate(renderer);
        return false;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) VK_CHECK(acq);
    VK_CHECK(vkResetFences(ctx_.device(), 1, &slot.fence));

    VK_CHECK(vkResetCommandPool(ctx_.device(), slot.pool, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(slot.cmd, &bi));
    OutputTarget target;
    target.image = images_[image];
    target.view = views_[image];
    target.format = format_;
    target.extent = physical_;
    target.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    target.pre_rotation = rotation_;
    renderer.record(slot.cmd, slot_index_, game, target, hud);
    VK_CHECK(vkEndCommandBuffer(slot.cmd));

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = slot.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = render_done_[image];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = slot.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(ctx_.fns().queue_submit2(ctx_.queue(), 1, &si, slot.fence));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_done_[image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image;
    const VkResult pres = vkQueuePresentKHR(ctx_.queue(), &pi);
    slot_index_ = (slot_index_ + 1) % Renderer::kFramesInFlight;
    // SUBOPTIMAL signals an orientation change on Android 10+.
    if (pres == VK_SUBOPTIMAL_KHR || pres == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate(renderer);
    } else {
        VK_CHECK(pres);
    }
    ++presented_;
    return true;
}

}  // namespace apex
