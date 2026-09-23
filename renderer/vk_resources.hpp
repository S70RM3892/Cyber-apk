// Thin RAII-free resource helpers. The renderer owns a small, fixed set of resources,
// so one VkDeviceMemory per resource is fine and keeps the code auditable.
#pragma once

#include <cstdint>
#include <span>

#include "vk_context.hpp"

namespace apex::vk {

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;  // non-null for host-visible buffers
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::uint32_t mip_levels = 1;
    std::uint32_t array_layers = 1;
};

Buffer create_buffer(const Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
void destroy(const Context& ctx, Buffer& b);

Image create_image(const Context& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                   std::uint32_t mip_levels = 1, std::uint32_t array_layers = 1);  // >1: 2D-array view
void destroy(const Context& ctx, Image& img);

// Extra single-mip view (for rendering into / sampling one level of a mip chain).
VkImageView create_mip_view(const Context& ctx, const Image& img, std::uint32_t mip);

bool is_depth_format(VkFormat f);

// Image layout transition with synchronization2 semantics.
struct ImageTransition {
    VkImage image;
    VkImageLayout old_layout;
    VkImageLayout new_layout;
    VkPipelineStageFlags2 src_stage;
    VkAccessFlags2 src_access;
    VkPipelineStageFlags2 dst_stage;
    VkAccessFlags2 dst_access;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    std::uint32_t base_mip = 0;
    std::uint32_t mip_count = VK_REMAINING_MIP_LEVELS;
};
void transition(const Context& ctx, VkCommandBuffer cmd, std::span<const ImageTransition> ts);
inline void transition(const Context& ctx, VkCommandBuffer cmd, const ImageTransition& t) {
    transition(ctx, cmd, std::span<const ImageTransition>(&t, 1));
}

// Record-and-wait helper for uploads during setup.
class OneShot {
public:
    explicit OneShot(const Context& ctx);
    ~OneShot();
    VkCommandBuffer cmd() const { return cmd_; }
    void submit_and_wait();

private:
    const Context& ctx_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
};

VkShaderModule create_shader(const Context& ctx, std::span<const std::uint32_t> spirv);

}  // namespace apex::vk
