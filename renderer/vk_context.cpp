#include "vk_context.hpp"

#include <algorithm>
#include <cstring>

namespace apex::vk {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        APEX_LOGE("vulkan: %s", data->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        APEX_LOGW("vulkan: %s", data->pMessage);
    return VK_FALSE;
}

bool has_layer(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

template <typename T>
T load_device_fn(VkDevice device, const char* core, const char* khr) {
    auto fn = reinterpret_cast<T>(vkGetDeviceProcAddr(device, core));
    if (!fn) fn = reinterpret_cast<T>(vkGetDeviceProcAddr(device, khr));
    if (!fn) {
        APEX_LOGE("missing Vulkan entry point %s", core);
        std::abort();
    }
    return fn;
}

}  // namespace

Context::Context(const ContextDesc& desc) {
    std::uint32_t api = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_3) {
        APEX_LOGE("Vulkan 1.3 loader required, found %u.%u", VK_API_VERSION_MAJOR(api),
                  VK_API_VERSION_MINOR(api));
        std::abort();
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "CYBER-APEX";
    app.pEngineName = "apex";
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> exts = desc.instance_extensions;
    std::vector<const char*> layers;
    const bool validation = desc.enable_validation && has_layer("VK_LAYER_KHRONOS_validation");
    if (validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));

    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = debug_callback;
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create) create(instance_, &mi, nullptr, &messenger_);
        APEX_LOGI("validation layer enabled");
    }
}

void Context::create_device(const ContextDesc& desc, VkSurfaceKHR present_surface) {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance_, &count, gpus.data());

    // Prefer a discrete/integrated GPU over a CPU implementation; require 1.3 and a
    // queue that can do graphics + compute (+ present when a surface is given).
    int best_score = -1;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpu, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;

        std::uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qcount, qprops.data());
        for (std::uint32_t q = 0; q < qcount; ++q) {
            const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((qprops[q].queueFlags & need) != need) continue;
            if (present_surface) {
                VkBool32 ok = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(gpu, q, present_surface, &ok);
                if (!ok) continue;
            }
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                 : 1;
            if (score > best_score) {
                best_score = score;
                physical_ = gpu;
                queue_family_ = q;
            }
            break;
        }
    }
    if (!physical_) {
        APEX_LOGE("no Vulkan 1.3 device with a graphics+compute queue found");
        std::abort();
    }
    vkGetPhysicalDeviceProperties(physical_, &props_);
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props_);
    APEX_LOGI("GPU: %s (Vulkan %u.%u.%u)", props_.deviceName, VK_API_VERSION_MAJOR(props_.apiVersion),
              VK_API_VERSION_MINOR(props_.apiVersion), VK_API_VERSION_PATCH(props_.apiVersion));

    // Hardware ray tracing (spec: ray query is a feature flag; everything has a raster
    // fallback). Needs the extensions plus accelerationStructure, rayQuery and buffer
    // device addresses.
    std::uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ext_count, exts.data());
    auto has_ext = [&](const char* name) {
        for (const auto& e : exts)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    const bool rt_exts = has_ext(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                         has_ext(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                         has_ext(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

    VkPhysicalDeviceRayQueryFeaturesKHR frq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fas{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &frq};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                         rt_exts ? static_cast<void*>(&fas) : nullptr};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    vkGetPhysicalDeviceFeatures2(physical_, &supported);
    if (!f13.dynamicRendering || !f13.synchronization2) {
        APEX_LOGE("device lacks dynamicRendering/synchronization2");
        std::abort();
    }

    ray_query_ = desc.allow_ray_query && rt_exts && fas.accelerationStructure && frq.rayQuery &&
                 f12.bufferDeviceAddress;
    VkPhysicalDeviceRayQueryFeaturesKHR enrq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    enrq.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enas{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &enrq};
    enas.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceVulkan12Features en12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                          ray_query_ ? static_cast<void*>(&enas) : nullptr};
    en12.bufferDeviceAddress = ray_query_ ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceVulkan13Features en13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &en12};
    en13.dynamicRendering = VK_TRUE;
    en13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &en13};
    std::vector<const char*> dev_exts = desc.device_extensions;
    if (ray_query_) {
        dev_exts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        dev_exts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        dev_exts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &asp};
        vkGetPhysicalDeviceProperties2(physical_, &p2);
        as_scratch_align_ = std::max(asp.minAccelerationStructureScratchOffsetAlignment, 1u);
    }
    APEX_LOGI("ray query: %s", ray_query_ ? "on" : (rt_exts ? "off (disabled)" : "unsupported"));
    // Anisotropic filtering keeps material textures sharp on walls and streets seen at a
    // grazing angle (optional: used only when the device has it).
    enabled.features.samplerAnisotropy = supported.features.samplerAnisotropy;
    anisotropy_ = supported.features.samplerAnisotropy == VK_TRUE;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.data();
    VK_CHECK(vkCreateDevice(physical_, &dci, nullptr, &device_));
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    fns_.cmd_begin_rendering =
        load_device_fn<PFN_vkCmdBeginRendering>(device_, "vkCmdBeginRendering", "vkCmdBeginRenderingKHR");
    fns_.cmd_end_rendering =
        load_device_fn<PFN_vkCmdEndRendering>(device_, "vkCmdEndRendering", "vkCmdEndRenderingKHR");
    fns_.cmd_pipeline_barrier2 = load_device_fn<PFN_vkCmdPipelineBarrier2>(
        device_, "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR");
    fns_.queue_submit2 =
        load_device_fn<PFN_vkQueueSubmit2>(device_, "vkQueueSubmit2", "vkQueueSubmit2KHR");
    if (ray_query_) {
        auto get = [this](const char* n) { return vkGetDeviceProcAddr(device_, n); };
        fns_.create_as = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(get("vkCreateAccelerationStructureKHR"));
        fns_.destroy_as = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(get("vkDestroyAccelerationStructureKHR"));
        fns_.as_build_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            get("vkGetAccelerationStructureBuildSizesKHR"));
        fns_.cmd_build_as = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            get("vkCmdBuildAccelerationStructuresKHR"));
        fns_.as_address = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            get("vkGetAccelerationStructureDeviceAddressKHR"));
        fns_.buffer_address = load_device_fn<PFN_vkGetBufferDeviceAddress>(device_, "vkGetBufferDeviceAddress",
                                                                             "vkGetBufferDeviceAddressKHR");
        if (!fns_.create_as || !fns_.cmd_build_as || !fns_.buffer_address) ray_query_ = false;
    }
}

Context::~Context() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
    }
    if (messenger_) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance_, messenger_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

std::uint32_t Context::find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const {
    for (std::uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    APEX_LOGE("no memory type for bits 0x%x flags 0x%x", type_bits, flags);
    std::abort();
}

bool Context::supports_format(VkFormat format, VkFormatFeatureFlags features) const {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(physical_, format, &p);
    return (p.optimalTilingFeatures & features) == features;
}

}  // namespace apex::vk
