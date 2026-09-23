// Vulkan instance/device bootstrap shared by the Android runtime and the headless host runner.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <vector>

#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#include "apex/log.hpp"

#define VK_CHECK(expr)                                                                \
    do {                                                                              \
        const VkResult vk_check_result_ = (expr);                                     \
        if (vk_check_result_ != VK_SUCCESS) {                                         \
            APEX_LOGE("%s failed: VkResult %d (%s:%d)", #expr, int(vk_check_result_), \
                      __FILE__, __LINE__);                                            \
            std::abort();                                                             \
        }                                                                             \
    } while (0)

namespace apex::vk {

// Vulkan 1.3 entry points resolved through vkGetDeviceProcAddr: Android's loader
// stub does not export every core 1.3 symbol on all OS versions.
struct DeviceFns {
    PFN_vkCmdBeginRendering cmd_begin_rendering = nullptr;
    PFN_vkCmdEndRendering cmd_end_rendering = nullptr;
    PFN_vkCmdPipelineBarrier2 cmd_pipeline_barrier2 = nullptr;
    PFN_vkQueueSubmit2 queue_submit2 = nullptr;
};

struct ContextDesc {
    std::vector<const char*> instance_extensions;
    std::vector<const char*> device_extensions;
    bool enable_validation = false;
};

// Two-phase setup: the constructor creates the instance; create_device() picks the
// GPU and queue. The Android runtime creates its window surface in between so the
// chosen queue family is guaranteed to be able to present to it.
class Context {
public:
    explicit Context(const ContextDesc& desc);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    void create_device(const ContextDesc& desc, VkSurfaceKHR present_surface = VK_NULL_HANDLE);

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical_device() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    std::uint32_t queue_family() const { return queue_family_; }
    const DeviceFns& fns() const { return fns_; }
    const VkPhysicalDeviceProperties& properties() const { return props_; }
    bool anisotropy() const { return anisotropy_; }  // samplerAnisotropy enabled

    std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    bool supports_format(VkFormat format, VkFormatFeatureFlags features) const;

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkPhysicalDeviceProperties props_{};
    bool anisotropy_ = false;
    VkPhysicalDeviceMemoryProperties mem_props_{};
    DeviceFns fns_;
};

}  // namespace apex::vk
