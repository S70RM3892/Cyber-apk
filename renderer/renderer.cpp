#include "renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "shaders_embedded.hpp"

namespace apex {

namespace {

// std140 mirror of shaders/include/frame_ubo.glsl.
struct FrameUniforms {
    Mat4 view_proj;
    Mat4 inv_view_proj;
    Mat4 view;
    Mat4 proj;
    float camera_pos[4];
    float road_field[4];
    float viewport[4];
    float fog[4];
};
static_assert(sizeof(FrameUniforms) == 320);

constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kMaterialFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// Inverse of an invertible 4x4 (cofactor expansion). Only used once per frame.
Mat4 inverse(const Mat4& a) {
    const float* m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    const float s = det != 0.0f ? 1.0f / det : 0.0f;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * s;
    return r;
}

enum class Blend { None, Additive, Alpha };

struct PipelineDesc {
    std::span<const std::uint32_t> vs, fs;
    VkPipelineLayout layout;
    std::vector<VkFormat> color_formats;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    bool depth_test = false, depth_write = false;
    VkCompareOp depth_op = VK_COMPARE_OP_GREATER;
    Blend blend = Blend::None;
};

VkPipeline make_pipeline(const vk::Context& ctx, const PipelineDesc& d) {
    VkShaderModule vs = vk::create_shader(ctx, d.vs);
    VkShaderModule fs = vk::create_shader(ctx, d.fs);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth_test;
    ds.depthWriteEnable = d.depth_write;
    ds.depthCompareOp = d.depth_op;

    std::vector<VkPipelineColorBlendAttachmentState> att(d.color_formats.size());
    for (std::size_t i = 0; i < att.size(); ++i) {
        auto& a = att[i];
        a.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        // Identical state on every attachment: independentBlend is not required.
        if (d.blend == Blend::Additive) {
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            a.colorBlendOp = VK_BLEND_OP_ADD;
            a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (d.blend == Blend::Alpha) {
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            a.colorBlendOp = VK_BLEND_OP_ADD;
            a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            a.alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = static_cast<std::uint32_t>(att.size());
    cb.pAttachments = att.data();

    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(d.color_formats.size());
    rendering.pColorAttachmentFormats = d.color_formats.data();
    rendering.depthAttachmentFormat = d.depth_format;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = d.layout;
    VkPipeline pso;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pso));
    vkDestroyShaderModule(ctx.device(), vs, nullptr);
    vkDestroyShaderModule(ctx.device(), fs, nullptr);
    return pso;
}

void set_viewport(VkCommandBuffer cmd, VkExtent2D e) {
    VkViewport v{0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0.0f, 1.0f};
    VkRect2D s{{0, 0}, e};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &s);
}

VkExtent2D scaled(VkExtent2D e, float s) {
    return {std::max(1u, static_cast<std::uint32_t>(static_cast<float>(e.width) * s)),
            std::max(1u, static_cast<std::uint32_t>(static_cast<float>(e.height) * s))};
}

constexpr VkPipelineStageFlags2 kAnyFragmentWork = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

}  // namespace

Renderer::Renderer(vk::Context& ctx, VkFormat output_format, VkExtent2D output_extent, const RenderSettings& s)
    : ctx_(ctx), settings_(s), output_format_(output_format), output_extent_(output_extent) {
    // B10G11R11 halves bloom bandwidth; it's optional as a render target, so check.
    if (ctx_.supports_format(VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                             VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
        bloom_format_ = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    create_static();
    create_pipelines();
    create_sized();
}

Renderer::~Renderer() {
    VkDevice dev = ctx_.device();
    vkDeviceWaitIdle(dev);
    destroy_sized();
    for (VkPipeline p : {ground_pso_, buildings_pso_, signs_pso_, sky_pso_, rain_pso_, traffic_pso_, streetlife_pso_, resolve_pso_, bloom_down_pso_,
                         bloom_up_pso_, tonemap_pso_})
        vkDestroyPipeline(dev, p, nullptr);
    vkDestroyPipeline(dev, hud_pso_, nullptr);
    vkDestroyPipelineLayout(dev, hud_layout_, nullptr);
    vkDestroyDescriptorSetLayout(dev, hud_set_layout_, nullptr);
    for (auto& b : hud_buffers_) vk::destroy(ctx_, b);
    if (timestamps_) vkDestroyQueryPool(dev, timestamps_, nullptr);
    vkDestroyPipelineLayout(dev, scene_layout_, nullptr);
    vkDestroyPipelineLayout(dev, post_layout_, nullptr);
    vkDestroyDescriptorPool(dev, static_pool_, nullptr);
    vkDestroyDescriptorPool(dev, hud_pool_, nullptr);
    vkDestroyDescriptorSetLayout(dev, scene_set_layout_, nullptr);
    vkDestroyDescriptorSetLayout(dev, post_set_layout_, nullptr);
    vkDestroySampler(dev, linear_clamp_, nullptr);
    vkDestroySampler(dev, point_clamp_, nullptr);
    vk::destroy(ctx_, frame_ubo_);
    vk::destroy(ctx_, buildings_);
    vk::destroy(ctx_, signs_);
    vk::destroy(ctx_, road_field_);
    vk::destroy(ctx_, road_staging_);
}

void Renderer::create_static() {
    VkDevice dev = ctx_.device();

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &linear_clamp_));
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &point_clamp_));

    // Frame UBO: one slot per frame in flight, bound with a dynamic offset.
    const auto align = ctx_.properties().limits.minUniformBufferOffsetAlignment;
    ubo_stride_ = (sizeof(FrameUniforms) + align - 1) / align * align;
    frame_ubo_ = vk::create_buffer(ctx_, ubo_stride_ * kFramesInFlight, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);

    // Instance buffers start small and grow on demand.
    ensure_buffer(buildings_, 64 * sizeof(BuildingInstance));
    ensure_buffer(signs_, 64 * sizeof(SignInstance));

    road_field_ = vk::create_image(ctx_, {RoadField::kSize, RoadField::kSize}, VK_FORMAT_R8_UNORM,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    road_staging_ = vk::create_buffer(ctx_, RoadField::kSize * RoadField::kSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    {
        // Start with "no roads" so the first frame is valid even before upload_world.
        std::memset(road_staging_.mapped, 255, RoadField::kSize * RoadField::kSize);
        vk::OneShot os(ctx_);
        vk::transition(ctx_, os.cmd(), {road_field_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT});
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {RoadField::kSize, RoadField::kSize, 1};
        vkCmdCopyBufferToImage(os.cmd(), road_staging_.buffer, road_field_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy);
        vk::transition(ctx_, os.cmd(), {road_field_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
        os.submit_and_wait();
    }

    // Descriptor set layouts.
    {
        VkDescriptorSetLayoutBinding b[4]{};
        const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, vf, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr};
        b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr};
        b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, vf, nullptr};  // vertex: arterial culling
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 4;
        ci.pBindings = b;
        VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &scene_set_layout_));
    }
    {
        VkDescriptorSetLayoutBinding b[4]{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        for (std::uint32_t i = 1; i < 4; ++i)
            b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 4;
        ci.pBindings = b;
        VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &post_set_layout_));
    }

    // Pipeline layouts.
    {
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &scene_set_layout_;
        VK_CHECK(vkCreatePipelineLayout(dev, &ci, nullptr, &scene_layout_));
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        ci.pSetLayouts = &post_set_layout_;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pc;
        VK_CHECK(vkCreatePipelineLayout(dev, &ci, nullptr, &post_layout_));
    }

    // Scene set lives in a pool that survives resizes.
    {
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 1;
        ci.poolSizeCount = 3;
        ci.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(dev, &ci, nullptr, &static_pool_));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = static_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &scene_set_layout_;
        VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &scene_set_));
        write_scene_set();
    }

    // HUD: one quad buffer per frame in flight, rewritten every frame.
    {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &hud_set_layout_));
        VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &hud_set_layout_;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pc;
        VK_CHECK(vkCreatePipelineLayout(dev, &li, nullptr, &hud_layout_));

        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = kFramesInFlight;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &size;
        VkDescriptorPool pool;
        VK_CHECK(vkCreateDescriptorPool(dev, &pci, nullptr, &pool));
        hud_pool_ = pool;
        for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
            hud_buffers_[i] = vk::create_buffer(ctx_, kMaxHudQuads * sizeof(HudQuad), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = hud_pool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &hud_set_layout_;
            VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &hud_sets_[i]));
            VkDescriptorBufferInfo bi{hud_buffers_[i].buffer, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = hud_sets_[i];
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        }
    }

    // GPU timestamps (2 per frame slot), if the queue supports them.
    {
        std::uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx_.physical_device(), &n, nullptr);
        std::vector<VkQueueFamilyProperties> q(n);
        vkGetPhysicalDeviceQueueFamilyProperties(ctx_.physical_device(), &n, q.data());
        if (q[ctx_.queue_family()].timestampValidBits > 0 && ctx_.properties().limits.timestampPeriod > 0.0f) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2 * kFramesInFlight;
            VK_CHECK(vkCreateQueryPool(dev, &qi, nullptr, &timestamps_));
            timestamp_period_ns_ = ctx_.properties().limits.timestampPeriod;
        }
    }
}

void Renderer::write_scene_set() {
    VkDescriptorBufferInfo ubo{frame_ubo_.buffer, 0, sizeof(FrameUniforms)};
    VkDescriptorBufferInfo bld{buildings_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo sgn{signs_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo road{linear_clamp_, road_field_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[4]{};
    for (auto& x : w) {
        x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        x.dstSet = scene_set_;
        x.descriptorCount = 1;
    }
    w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w[0].pBufferInfo = &ubo;
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &bld;
    w[2].dstBinding = 2;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[2].pBufferInfo = &sgn;
    w[3].dstBinding = 3;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[3].pImageInfo = &road;
    vkUpdateDescriptorSets(ctx_.device(), 4, w, 0, nullptr);
}

void Renderer::ensure_buffer(vk::Buffer& b, VkDeviceSize size) {
    if (b.buffer && b.size >= size) return;
    vk::destroy(ctx_, b);
    // Host-visible storage: mobile GPUs share memory with the CPU, and these buffers
    // change only when the streaming window moves.
    b = vk::create_buffer(ctx_, std::max<VkDeviceSize>(size * 3 / 2, 256), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
}

void Renderer::create_pipelines() {
    namespace sh = apex::shaders;
    const std::vector<VkFormat> scene_formats{kSceneColorFormat, kMaterialFormat};

    PipelineDesc d;
    d.layout = scene_layout_;
    d.color_formats = scene_formats;
    d.depth_format = kDepthFormat;
    d.depth_test = true;
    d.depth_write = true;
    d.depth_op = VK_COMPARE_OP_GREATER;

    d.vs = sh::ground_vert;
    d.fs = sh::ground_frag;
    ground_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::buildings_vert;
    d.fs = sh::buildings_frag;
    d.cull = VK_CULL_MODE_BACK_BIT;
    buildings_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::traffic_vert;
    d.fs = sh::traffic_frag;
    traffic_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::streetlife_vert;
    d.fs = sh::streetlife_frag;
    streetlife_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::signs_vert;
    d.fs = sh::signs_frag;
    d.cull = VK_CULL_MODE_NONE;
    d.depth_op = VK_COMPARE_OP_GREATER_OR_EQUAL;
    signs_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::fullscreen_vert;
    d.fs = sh::sky_frag;
    d.depth_write = false;
    d.depth_op = VK_COMPARE_OP_GREATER_OR_EQUAL;  // only where depth is still the cleared far plane
    sky_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::rain_vert;
    d.fs = sh::rain_frag;
    d.depth_op = VK_COMPARE_OP_GREATER;
    d.blend = Blend::Additive;  // rain adds zero to the material target
    rain_pso_ = make_pipeline(ctx_, d);

    PipelineDesc p;
    p.layout = post_layout_;
    p.vs = sh::fullscreen_vert;
    p.color_formats = {kSceneColorFormat};
    p.fs = sh::resolve_frag;
    resolve_pso_ = make_pipeline(ctx_, p);

    p.color_formats = {bloom_format_};
    p.fs = sh::bloom_down_frag;
    bloom_down_pso_ = make_pipeline(ctx_, p);
    p.fs = sh::bloom_up_frag;
    p.blend = Blend::Additive;
    bloom_up_pso_ = make_pipeline(ctx_, p);

    p.color_formats = {output_format_};
    p.fs = sh::tonemap_frag;
    p.blend = Blend::None;
    tonemap_pso_ = make_pipeline(ctx_, p);

    p.layout = hud_layout_;
    p.vs = sh::hud_vert;
    p.fs = sh::hud_frag;
    p.blend = Blend::Alpha;
    hud_pso_ = make_pipeline(ctx_, p);
}

void Renderer::create_sized() {
    internal_ = scaled(output_extent_, settings_.render_scale);
    const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    scene_color_ = vk::create_image(ctx_, internal_, kSceneColorFormat, rt);
    scene_material_ = vk::create_image(ctx_, internal_, kMaterialFormat, rt);
    depth_ = vk::create_image(ctx_, internal_, kDepthFormat,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resolved_ = vk::create_image(ctx_, internal_, kSceneColorFormat, rt);
    VkExtent2D e = internal_;
    for (auto& b : bloom_) {
        e = {std::max(1u, e.width / 2), std::max(1u, e.height / 2)};
        b = vk::create_image(ctx_, e, bloom_format_, rt);
    }

    VkDevice dev = ctx_.device();
    constexpr std::uint32_t kSets = 2 + 2 * kBloomLevels;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSets},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 3}};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = kSets;
    ci.poolSizeCount = 2;
    ci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(dev, &ci, nullptr, &sized_pool_));

    auto alloc = [&](VkImageView a, VkSampler sa, VkImageView b, VkSampler sb, VkImageView c, VkSampler sc) {
        VkDescriptorSet set;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = sized_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &post_set_layout_;
        VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &set));
        VkDescriptorBufferInfo ubo{frame_ubo_.buffer, 0, sizeof(FrameUniforms)};
        // Unused slots still need a valid descriptor; point them at binding 1's image.
        VkDescriptorImageInfo imgs[3] = {{sa, a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                         {sb ? sb : sa, b ? b : a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                         {sc ? sc : sa, c ? c : a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w[4]{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            if (i == 0) {
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                w[i].pBufferInfo = &ubo;
            } else {
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &imgs[i - 1];
            }
        }
        vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
        return set;
    };

    resolve_set_ = alloc(scene_color_.view, linear_clamp_, scene_material_.view, point_clamp_, depth_.view, point_clamp_);
    for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
        const VkImageView src = i == 0 ? resolved_.view : bloom_[i - 1].view;
        bloom_down_sets_[i] = alloc(src, linear_clamp_, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
        // bloom_up_sets_[i] samples level i+1 (last one unused).
        const VkImageView up_src = bloom_[std::min(i + 1, kBloomLevels - 1)].view;
        bloom_up_sets_[i] = alloc(up_src, linear_clamp_, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    tonemap_set_ = alloc(resolved_.view, linear_clamp_, bloom_[0].view, linear_clamp_, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void Renderer::destroy_sized() {
    vk::destroy(ctx_, scene_color_);
    vk::destroy(ctx_, scene_material_);
    vk::destroy(ctx_, depth_);
    vk::destroy(ctx_, resolved_);
    for (auto& b : bloom_) vk::destroy(ctx_, b);
    if (sized_pool_) vkDestroyDescriptorPool(ctx_.device(), sized_pool_, nullptr);
    sized_pool_ = VK_NULL_HANDLE;
}

void Renderer::resize(VkExtent2D output_extent) {
    vkDeviceWaitIdle(ctx_.device());
    output_extent_ = output_extent;
    destroy_sized();
    create_sized();
}

void Renderer::upload_world(const CitySnapshot& snap) {
    vkDeviceWaitIdle(ctx_.device());
    const VkDeviceSize bsize = snap.buildings.size() * sizeof(BuildingInstance);
    const VkDeviceSize ssize = snap.signs.size() * sizeof(SignInstance);
    ensure_buffer(buildings_, bsize);
    ensure_buffer(signs_, ssize);
    if (bsize) std::memcpy(buildings_.mapped, snap.buildings.data(), bsize);
    if (ssize) std::memcpy(signs_.mapped, snap.signs.data(), ssize);
    building_count_ = static_cast<std::uint32_t>(snap.buildings.size());
    sign_count_ = static_cast<std::uint32_t>(snap.signs.size());

    std::memcpy(road_staging_.mapped, snap.roads.data.data(), snap.roads.data.size());
    {
        vk::OneShot os(ctx_);
        vk::transition(ctx_, os.cmd(), {road_field_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                                        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT});
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {RoadField::kSize, RoadField::kSize, 1};
        vkCmdCopyBufferToImage(os.cmd(), road_staging_.buffer, road_field_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy);
        vk::transition(ctx_, os.cmd(), {road_field_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
        os.submit_and_wait();
    }
    road_params_ = {snap.roads.origin_x, snap.roads.origin_y, snap.roads.texel,
                    snap.roads.texel * static_cast<float>(RoadField::kSize)};
    road_field_ready_ = true;
    write_scene_set();  // buffers may have been reallocated
}

void Renderer::update_frame_ubo(std::uint32_t slot, const Game& game) {
    const Camera& cam = game.camera();
    const float aspect = static_cast<float>(output_extent_.width) / static_cast<float>(output_extent_.height);
    FrameUniforms u{};
    u.view = cam.view();
    u.proj = cam.projection(aspect);
    u.view_proj = u.proj * u.view;
    u.inv_view_proj = inverse(u.view_proj);
    u.camera_pos[0] = cam.position.x;
    u.camera_pos[1] = cam.position.y;
    u.camera_pos[2] = cam.position.z;
    u.camera_pos[3] = game.time();
    std::memcpy(u.road_field, road_params_.data(), sizeof(u.road_field));
    u.viewport[0] = static_cast<float>(internal_.width);
    u.viewport[1] = static_cast<float>(internal_.height);
    u.viewport[2] = 1.0f / u.viewport[0];
    u.viewport[3] = 1.0f / u.viewport[1];
    u.fog[0] = settings_.fog_density;
    u.fog[1] = 0.018f;
    u.fog[2] = 0.97f;
    u.fog[3] = settings_.rain;
    std::memcpy(static_cast<char*>(frame_ubo_.mapped) + slot * ubo_stride_, &u, sizeof(u));
}

void Renderer::fullscreen_pass(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent, VkPipeline pso,
                               VkDescriptorSet set, std::uint32_t slot, const void* push, std::uint32_t push_size,
                               bool load) {
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ctx_.fns().cmd_begin_rendering(cmd, &ri);
    set_viewport(cmd, extent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    const std::uint32_t offset = static_cast<std::uint32_t>(slot * ubo_stride_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout_, 0, 1, &set, 1, &offset);
    if (push_size) vkCmdPushConstants(cmd, post_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    ctx_.fns().cmd_end_rendering(cmd);
}

void Renderer::record(VkCommandBuffer cmd, std::uint32_t slot, const Game& game, const OutputTarget& target,
                      std::span<const HudQuad> hud) {
    update_frame_ubo(slot, game);
    if (timestamps_) {
        // The caller waited for this slot's previous submission, so its queries are final.
        if (timestamps_written_[slot]) {
            std::uint64_t ts[2] = {};
            if (vkGetQueryPoolResults(ctx_.device(), timestamps_, 2 * slot, 2, sizeof(ts), ts, sizeof(std::uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0])
                gpu_ms_ = static_cast<float>(static_cast<double>(ts[1] - ts[0]) * timestamp_period_ns_ * 1e-6);
        }
        vkCmdResetQueryPool(cmd, timestamps_, 2 * slot, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps_, 2 * slot);
    }
    const auto& fns = ctx_.fns();
    using vk::ImageTransition;

    // ---- Scene pass --------------------------------------------------------------
    {
        const ImageTransition ts[] = {
            {scene_color_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kAnyFragmentWork, 0,
             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
            {scene_material_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
             kAnyFragmentWork, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
            {depth_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kAnyFragmentWork, 0,
             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
             VK_IMAGE_ASPECT_DEPTH_BIT},
        };
        vk::transition(ctx_, cmd, ts);

        VkRenderingAttachmentInfo colors[2]{};
        for (auto& c : colors) {
            c.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            c.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        colors[0].imageView = scene_color_.view;
        colors[1].imageView = scene_material_.view;
        colors[1].clearValue.color = {{0.0f, 1.0f, 0.5f, 0.5f}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {0.0f, 0};  // reversed-Z far plane

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, internal_};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 2;
        ri.pColorAttachments = colors;
        ri.pDepthAttachment = &depth;
        fns.cmd_begin_rendering(cmd, &ri);
        set_viewport(cmd, internal_);

        const std::uint32_t offset = static_cast<std::uint32_t>(slot * ubo_stride_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_layout_, 0, 1, &scene_set_, 1, &offset);

        // Front-to-back-ish: buildings occlude most of the ground.
        if (building_count_) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, buildings_pso_);
            vkCmdDraw(cmd, 30, building_count_, 0, 0);
        }
        if (settings_.traffic_count) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, traffic_pso_);
            vkCmdDraw(cmd, 36, settings_.traffic_count, 0, 0);
        }
        {
            // Lamp posts (3 boxes each) then pedestrians (6 boxes each); see streetlife.vert.
            constexpr std::uint32_t kLampCount = 2 * 9 * 2 * 20;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, streetlife_pso_);
            vkCmdDraw(cmd, 3 * 36, kLampCount, 0, 0);
            if (settings_.pedestrian_count) vkCmdDraw(cmd, 6 * 36, settings_.pedestrian_count, 0, kLampCount);
        }
        if (road_field_ready_) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ground_pso_);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
        if (sign_count_) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, signs_pso_);
            vkCmdDraw(cmd, 6, sign_count_, 0, 0);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pso_);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        if (settings_.rain > 0.0f) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rain_pso_);
            vkCmdDraw(cmd, 6, 6000, 0, 0);
        }
        fns.cmd_end_rendering(cmd);
    }

    // ---- SSR resolve ---------------------------------------------------------------
    {
        const ImageTransition ts[] = {
            {scene_color_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT},
            {scene_material_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT},
            {depth_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT},
            {resolved_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kAnyFragmentWork, 0,
             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
        };
        vk::transition(ctx_, cmd, ts);
        const std::int32_t steps[4] = {settings_.ssr_steps, 0, 0, 0};
        fullscreen_pass(cmd, resolved_.view, internal_, resolve_pso_, resolve_set_, slot, steps, sizeof(steps), false);
        vk::transition(ctx_, cmd, {resolved_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
    }

    // ---- Bloom -------------------------------------------------------------------
    {
        VkExtent2D src_extent = internal_;
        for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
            vk::transition(ctx_, cmd, {bloom_[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       kAnyFragmentWork, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
            const float push[4] = {1.0f / static_cast<float>(src_extent.width),
                                   1.0f / static_cast<float>(src_extent.height), i == 0 ? 1.0f : 0.0f, 1.0f};
            fullscreen_pass(cmd, bloom_[i].view, bloom_[i].extent, bloom_down_pso_, bloom_down_sets_[i], slot, push,
                            sizeof(push), false);
            vk::transition(ctx_, cmd, {bloom_[i].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
            src_extent = bloom_[i].extent;
        }
        for (std::int32_t i = static_cast<std::int32_t>(kBloomLevels) - 2; i >= 0; --i) {
            vk::Image& dst = bloom_[static_cast<std::uint32_t>(i)];
            const vk::Image& src = bloom_[static_cast<std::uint32_t>(i) + 1];
            vk::transition(ctx_, cmd, {dst.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
            const float push[4] = {1.0f / static_cast<float>(src.extent.width),
                                   1.0f / static_cast<float>(src.extent.height), 1.0f, 0.0f};
            fullscreen_pass(cmd, dst.view, dst.extent, bloom_up_pso_, bloom_up_sets_[static_cast<std::uint32_t>(i)],
                            slot, push, sizeof(push), true);
            vk::transition(ctx_, cmd, {dst.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
        }
    }

    // ---- Tonemap + upscale into the output -----------------------------------------
    {
        vk::transition(ctx_, cmd, {target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
        const bool srgb = target.format == VK_FORMAT_R8G8B8A8_SRGB || target.format == VK_FORMAT_B8G8R8A8_SRGB;
        struct {
            float exposure, bloom, time, srgb;
            std::int32_t rotation, pad[3];
        } push{settings_.exposure, settings_.bloom_strength, game.time(), srgb ? 1.0f : 0.0f, target.pre_rotation, {}};
        fullscreen_pass(cmd, target.view, target.extent, tonemap_pso_, tonemap_set_, slot, &push, sizeof(push), false);

        if (!hud.empty()) {
            const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::size_t>(hud.size(), kMaxHudQuads));
            std::memcpy(hud_buffers_[slot].mapped, hud.data(), count * sizeof(HudQuad));
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = target.view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, target.extent};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &color;
            // The tonemap pass wrote the same attachment: order its writes before our blend reads.
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            fns.cmd_pipeline_barrier2(cmd, &dep);
            fns.cmd_begin_rendering(cmd, &ri);
            set_viewport(cmd, target.extent);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hud_pso_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hud_layout_, 0, 1, &hud_sets_[slot], 0,
                                    nullptr);
            struct {
                float w, h;
                std::int32_t rotation, srgb;
            } hp{static_cast<float>(output_extent_.width), static_cast<float>(output_extent_.height),
                 target.pre_rotation, srgb ? 1 : 0};
            vkCmdPushConstants(cmd, hud_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(hp), &hp);
            vkCmdDraw(cmd, 6, count, 0, 0);
            fns.cmd_end_rendering(cmd);
        }

        const bool to_present = target.final_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vk::transition(ctx_, cmd, {target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, target.final_layout,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                   to_present ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT,
                                   to_present ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_READ_BIT});
    }
    if (timestamps_) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_, 2 * slot + 1);
        timestamps_written_[slot] = true;
    }
}

void Renderer::update_dynamic_resolution() {
    if (!settings_.dynamic_resolution || gpu_ms_ <= 0.0f) return;
    gpu_ms_avg_ = gpu_ms_avg_ <= 0.0f ? gpu_ms_ : gpu_ms_avg_ * 0.95f + gpu_ms_ * 0.05f;
    // Resizing idles the GPU and reallocates targets: at most every ~2 s, in 5% steps,
    // with a dead band so it doesn't oscillate.
    if (++frames_since_resize_ < 120) return;
    float scale = settings_.render_scale;
    if (gpu_ms_avg_ > settings_.gpu_budget_ms * 1.05f) scale -= 0.05f;
    else if (gpu_ms_avg_ < settings_.gpu_budget_ms * 0.75f) scale += 0.05f;
    scale = std::clamp(scale, settings_.min_scale, settings_.max_scale);
    if (std::fabs(scale - settings_.render_scale) < 1e-3f) return;
    APEX_LOGI("DRS: gpu %.1f ms -> render scale %.2f", static_cast<double>(gpu_ms_avg_), static_cast<double>(scale));
    settings_.render_scale = scale;
    frames_since_resize_ = 0;
    gpu_ms_avg_ = 0.0f;
    resize(output_extent_);
}

}  // namespace apex
