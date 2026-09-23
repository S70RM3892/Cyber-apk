#include "vk_resources.hpp"

#include <vector>

namespace apex::vk {

Buffer create_buffer(const Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(ctx.device(), &ci, nullptr, &b.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx.device(), b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx.find_memory_type(
        req.memoryTypeBits, host_visible
                                ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device(), &ai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(ctx.device(), b.buffer, b.memory, 0));
    if (host_visible) VK_CHECK(vkMapMemory(ctx.device(), b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return b;
}

void destroy(const Context& ctx, Buffer& b) {
    if (b.buffer) vkDestroyBuffer(ctx.device(), b.buffer, nullptr);
    if (b.memory) vkFreeMemory(ctx.device(), b.memory, nullptr);
    b = {};
}

bool is_depth_format(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

Image create_image(const Context& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                   std::uint32_t mip_levels) {
    Image img;
    img.format = format;
    img.extent = extent;
    img.mip_levels = mip_levels;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = mip_levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.device(), &ci, nullptr, &img.image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx.device(), img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx.find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device(), &ai, nullptr, &img.memory));
    VK_CHECK(vkBindImageMemory(ctx.device(), img.image, img.memory, 0));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {static_cast<VkImageAspectFlags>(is_depth_format(format) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                                     : VK_IMAGE_ASPECT_COLOR_BIT),
                           0, mip_levels, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &img.view));
    return img;
}

VkImageView create_mip_view(const Context& ctx, const Image& img, std::uint32_t mip) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = img.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
    VkImageView view;
    VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &view));
    return view;
}

void destroy(const Context& ctx, Image& img) {
    if (img.view) vkDestroyImageView(ctx.device(), img.view, nullptr);
    if (img.image) vkDestroyImage(ctx.device(), img.image, nullptr);
    if (img.memory) vkFreeMemory(ctx.device(), img.memory, nullptr);
    img = {};
}

void transition(const Context& ctx, VkCommandBuffer cmd, std::span<const ImageTransition> ts) {
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(ts.size());
    for (const auto& t : ts) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = t.src_stage;
        b.srcAccessMask = t.src_access;
        b.dstStageMask = t.dst_stage;
        b.dstAccessMask = t.dst_access;
        b.oldLayout = t.old_layout;
        b.newLayout = t.new_layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.image;
        b.subresourceRange = {t.aspect, t.base_mip, t.mip_count, 0, 1};
        barriers.push_back(b);
    }
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();
    ctx.fns().cmd_pipeline_barrier2(cmd, &dep);
}

OneShot::OneShot(const Context& ctx) : ctx_(ctx) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = ctx.queue_family();
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pci, nullptr, &pool_));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &ai, &cmd_));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd_, &bi));
}

void OneShot::submit_and_wait() {
    VK_CHECK(vkEndCommandBuffer(cmd_));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    VK_CHECK(vkQueueSubmit(ctx_.queue(), 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx_.queue()));
}

OneShot::~OneShot() { vkDestroyCommandPool(ctx_.device(), pool_, nullptr); }

VkShaderModule create_shader(const Context& ctx, std::span<const std::uint32_t> spirv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size_bytes();
    ci.pCode = spirv.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(ctx.device(), &ci, nullptr, &m));
    return m;
}

}  // namespace apex::vk
