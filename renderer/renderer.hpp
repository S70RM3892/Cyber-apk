// Frame renderer: scene pass (G-buffer-lite: HDR + material + depth), wet-street SSR
// resolve, bloom chain, tonemap/upscale into the platform's output image.
//
// The platform layer owns command buffers, synchronization and the output image
// (swapchain on Android, offscreen on the host); this class only records.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "apex/game.hpp"
#include "apex/hud.hpp"
#include "vk_resources.hpp"

namespace apex {

struct RenderSettings {
    float render_scale = 0.67f;  // internal resolution relative to output (spec §4.3 DRS range)
    // Dynamic resolution: steer render_scale within [min, max] to keep GPU time under
    // the budget. 0.5 of 1080p = 540p, the floor of the spec's DRS range.
    bool dynamic_resolution = true;
    float min_scale = 0.5f, max_scale = 0.75f;
    float gpu_budget_ms = 13.0f;  // leaves headroom inside a 16.6 ms vsync interval
    int ssr_steps = 24;
    float exposure = 1.5f;
    float bloom_strength = 0.9f;
    float rain = 1.0f;
    float fog_density = 0.0045f;
    std::uint32_t traffic_count = 580;     // first 320 fly (traffic.vert kAirCount), the rest drive
    std::uint32_t pedestrian_count = 700;
};

struct OutputTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageLayout final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    // Surface pre-rotation in degrees (0/90/180/270). `extent` is the physical
    // (identity-orientation) swapchain size; the logical view is rotated from it.
    int pre_rotation = 0;
};

class Renderer {
public:
    static constexpr std::uint32_t kFramesInFlight = 2;
    static constexpr std::uint32_t kBloomLevels = 6;

    Renderer(vk::Context& ctx, VkFormat output_format, VkExtent2D output_extent, const RenderSettings& s);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // `output_extent` is the logical (upright) size the player sees.
    void resize(VkExtent2D output_extent);
    // Re-upload streamed city data. Waits for the GPU to go idle (only happens when
    // the player crosses a tile boundary).
    void upload_world(const CitySnapshot& snap);

    void record(VkCommandBuffer cmd, std::uint32_t frame_slot, const Game& game, const OutputTarget& target,
                std::span<const HudQuad> hud = {});

    // GPU time of the most recently completed frame (0 if timestamps are unsupported).
    float gpu_ms() const { return gpu_ms_; }
    // Feed after each frame; may change the internal resolution (with a GPU idle).
    void update_dynamic_resolution();

    RenderSettings& settings() { return settings_; }
    VkExtent2D internal_extent() const { return internal_; }

private:
    vk::Context& ctx_;
    RenderSettings settings_;
    VkFormat output_format_;
    VkExtent2D output_extent_{};
    VkExtent2D internal_{};
    VkFormat bloom_format_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;

    // Samplers
    VkSampler linear_clamp_ = VK_NULL_HANDLE;
    VkSampler point_clamp_ = VK_NULL_HANDLE;

    // Size-dependent targets
    vk::Image scene_color_, scene_material_, depth_, resolved_;
    std::array<vk::Image, kBloomLevels> bloom_{};

    // Data
    vk::Buffer frame_ubo_;  // kFramesInFlight slots, dynamic offset
    VkDeviceSize ubo_stride_ = 0;
    vk::Buffer buildings_, signs_;
    std::uint32_t building_count_ = 0, sign_count_ = 0;
    vk::Image road_field_;
    vk::Buffer road_staging_;
    std::array<float, 4> road_params_{};
    bool road_field_ready_ = false;

    // Descriptors
    VkDescriptorSetLayout scene_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout post_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool static_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool sized_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet scene_set_ = VK_NULL_HANDLE;
    VkDescriptorSet resolve_set_ = VK_NULL_HANDLE;
    VkDescriptorSet tonemap_set_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kBloomLevels> bloom_down_sets_{};
    std::array<VkDescriptorSet, kBloomLevels> bloom_up_sets_{};

    // Pipelines
    VkPipelineLayout scene_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout post_layout_ = VK_NULL_HANDLE;
    VkPipeline ground_pso_ = VK_NULL_HANDLE, buildings_pso_ = VK_NULL_HANDLE, signs_pso_ = VK_NULL_HANDLE,
               sky_pso_ = VK_NULL_HANDLE, rain_pso_ = VK_NULL_HANDLE, traffic_pso_ = VK_NULL_HANDLE,
               streetlife_pso_ = VK_NULL_HANDLE, beacon_pso_ = VK_NULL_HANDLE;
    VkPipeline resolve_pso_ = VK_NULL_HANDLE, bloom_down_pso_ = VK_NULL_HANDLE, bloom_up_pso_ = VK_NULL_HANDLE,
               tonemap_pso_ = VK_NULL_HANDLE;

    // HUD
    static constexpr std::uint32_t kMaxHudQuads = 4096;
    VkDescriptorSetLayout hud_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool hud_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout hud_layout_ = VK_NULL_HANDLE;
    VkPipeline hud_pso_ = VK_NULL_HANDLE;
    std::array<vk::Buffer, kFramesInFlight> hud_buffers_{};
    std::array<VkDescriptorSet, kFramesInFlight> hud_sets_{};

    // GPU timing + dynamic resolution
    VkQueryPool timestamps_ = VK_NULL_HANDLE;
    std::array<bool, kFramesInFlight> timestamps_written_{};
    float timestamp_period_ns_ = 0.0f;
    float gpu_ms_ = 0.0f;
    float gpu_ms_avg_ = 0.0f;
    int frames_since_resize_ = 0;

    void create_static();
    void create_pipelines();
    void create_sized();
    void destroy_sized();
    void write_scene_set();
    void ensure_buffer(vk::Buffer& b, VkDeviceSize size);
    void update_frame_ubo(std::uint32_t slot, const Game& game);
    void fullscreen_pass(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent, VkPipeline pso,
                         VkDescriptorSet set, std::uint32_t slot, const void* push, std::uint32_t push_size,
                         bool load);
};

}  // namespace apex
