#include "renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "apex/sign_text_data.hpp"
#include "shaders_embedded.hpp"
#include "sign_font_sdf.hpp"

namespace apex {

namespace {

static_assert(signtext::kCols == 16 && signtext::kRows == 9 && signtext::kCell == 48 && signtext::kSpread == 6.0f,
              "shaders/include/city_common.glsl hard-codes the sign atlas layout");
static_assert(signtext::kGlyphChoonpu == 44, "city_common.glsl kGlyphChoonpu");
static_assert(signtext::kJapaneseCount == 52, "signs_common.glsl kJapaneseStrings");

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
    float objective[4];
    float player_car[4];
    float sun[4];
    Mat4 prev_view_proj;
    float taa[4];
};
static_assert(sizeof(FrameUniforms) == 448);

constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kMaterialFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kLightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // RT albedo, irradiance, specular
constexpr VkFormat kMetaFormat = VK_FORMAT_R16G16_SFLOAT;         // RT history: linear depth, samples
constexpr std::uint32_t kPlayerCarInstance = 1u << 20;  // traffic.vert kPlayerCar

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

// Clip-space planes of a view-projection matrix (reversed-Z infinite: no far plane).
struct Frustum {
    float planes[5][4];
    bool visible(const float mn[3], const float mx[3]) const {
        for (const auto& p : planes) {
            // Corner furthest along the plane normal.
            const float x = p[0] >= 0 ? mx[0] : mn[0], y = p[1] >= 0 ? mx[1] : mn[1], z = p[2] >= 0 ? mx[2] : mn[2];
            if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f) return false;
        }
        return true;
    }
};
Frustum frustum_of(const Mat4& m) {
    auto row = [&](int r, int c) { return m.at(c, r); };
    Frustum f{};
    const int rows[5][2] = {{0, 1}, {0, -1}, {1, 1}, {1, -1}, {2, -1}};  // w+x, w-x, w+y, w-y, w-z (near)
    for (int i = 0; i < 5; ++i)
        for (int c = 0; c < 4; ++c)
            f.planes[i][c] = row(3, c) + static_cast<float>(rows[i][1]) * row(rows[i][0], c);
    return f;
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
    std::span<const VkVertexInputBindingDescription> vertex_bindings;
    std::span<const VkVertexInputAttributeDescription> vertex_attributes;
};

VkPipeline make_pipeline(const vk::Context& ctx, const PipelineDesc& d) {
    VkShaderModule vs = vk::create_shader(ctx, d.vs);
    VkShaderModule fs = vk::create_shader(ctx, d.fs);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = static_cast<std::uint32_t>(d.vertex_bindings.size());
    vi.pVertexBindingDescriptions = d.vertex_bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(d.vertex_attributes.size());
    vi.pVertexAttributeDescriptions = d.vertex_attributes.data();
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
    // Depth must be sampled by the SSR pass. D32_SFLOAT is near-universal on mobile but
    // not guaranteed; X8_D24 or D16 are the spec's fallbacks.
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM}) {
        if (ctx_.supports_format(f, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            depth_format_ = f;
            break;
        }
    }
    if (ctx_.ray_query()) rt_ = std::make_unique<RtScene>(ctx_);
    create_static();
    create_pipelines();
    create_sized();
}

Renderer::~Renderer() {
    VkDevice dev = ctx_.device();
    vkDeviceWaitIdle(dev);
    destroy_sized();
    for (VkPipeline p : {ground_pso_, buildings_pso_, signs_pso_, sky_pso_, rain_pso_, traffic_pso_, streetlife_pso_,
                         beacon_pso_, signs_glow_pso_, props_pso_, lights_pso_, infra_pso_, detail_pso_, halo_pso_, box_pso_, resolve_pso_,
                         bloom_down_pso_,
                         bloom_up_pso_, tonemap_pso_, taa_pso_, rt_light_pso_, rt_accum_pso_, rt_atrous_pso_,
                         rt_composite_pso_})
        vkDestroyPipeline(dev, p, nullptr);
    vkDestroyPipeline(dev, hud_pso_, nullptr);
    vkDestroyPipelineLayout(dev, hud_layout_, nullptr);
    vkDestroyDescriptorSetLayout(dev, hud_set_layout_, nullptr);
    for (auto& b : hud_buffers_) vk::destroy(ctx_, b);
    if (timestamps_) vkDestroyQueryPool(dev, timestamps_, nullptr);
    vkDestroyPipelineLayout(dev, scene_layout_, nullptr);
    vkDestroyPipelineLayout(dev, post_layout_, nullptr);
    if (resolve_rt_layout_) vkDestroyPipelineLayout(dev, resolve_rt_layout_, nullptr);
    rt_.reset();
    vkDestroyDescriptorPool(dev, static_pool_, nullptr);
    vkDestroyDescriptorPool(dev, hud_pool_, nullptr);
    vkDestroyDescriptorSetLayout(dev, scene_set_layout_, nullptr);
    vkDestroyDescriptorSetLayout(dev, post_set_layout_, nullptr);
    vkDestroySampler(dev, linear_clamp_, nullptr);
    vkDestroySampler(dev, point_clamp_, nullptr);
    vk::destroy(ctx_, frame_ubo_);
    vk::destroy(ctx_, buildings_);
    vk::destroy(ctx_, signs_);
    vk::destroy(ctx_, props_);
    vk::destroy(ctx_, lights_);
    vk::destroy(ctx_, mesh_vertices_);
    vk::destroy(ctx_, mesh_indices_);
    vk::destroy(ctx_, point_lights_);
    vk::destroy(ctx_, light_grid_);
    vk::destroy(ctx_, halos_);
    vk::destroy(ctx_, boxes_);
    vk::destroy(ctx_, road_field_);
    vk::destroy(ctx_, sign_atlas_);
    vk::destroy(ctx_, mat_albedo_);
    vk::destroy(ctx_, mat_nrm_);
    vkDestroySampler(dev, material_sampler_, nullptr);
    vk::destroy(ctx_, sign_strings_);
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
    {
        VkSamplerCreateInfo ms{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ms.magFilter = ms.minFilter = VK_FILTER_LINEAR;
        ms.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ms.addressModeU = ms.addressModeV = ms.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ms.maxLod = VK_LOD_CLAMP_NONE;
        ms.anisotropyEnable = ctx_.anisotropy() ? VK_TRUE : VK_FALSE;
        ms.maxAnisotropy = ctx_.anisotropy() ? std::min(8.0f, ctx_.properties().limits.maxSamplerAnisotropy) : 1.0f;
        VK_CHECK(vkCreateSampler(dev, &ms, nullptr, &material_sampler_));
    }
    {
        // Neutral stand-in until upload_materials: mid-grey albedo (x2 in the shader = 1),
        // flat normal, medium roughness.
        MaterialTextures neutral;
        neutral.size = neutral.nrm_size = 1;
        neutral.mips = neutral.nrm_mips = 1;
        for (std::uint32_t i = 0; i < MaterialTextures::kLayers; ++i) {
            neutral.albedo.insert(neutral.albedo.end(), {188, 188, 188, 255});
            neutral.nrm.insert(neutral.nrm.end(), {128, 128, 128, 255});
        }
        upload_materials_images(neutral);
    }

    // Frame UBO: one slot per frame in flight, bound with a dynamic offset.
    const auto align = ctx_.properties().limits.minUniformBufferOffsetAlignment;
    ubo_stride_ = (sizeof(FrameUniforms) + align - 1) / align * align;
    frame_ubo_ = vk::create_buffer(ctx_, ubo_stride_ * kFramesInFlight, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);

    // Instance buffers start small and grow on demand.
    ensure_buffer(buildings_, 64 * sizeof(BuildingInstance));
    ensure_buffer(signs_, 64 * sizeof(SignInstance));
    ensure_buffer(props_, 64 * sizeof(PropInstance));
    ensure_buffer(lights_, 64 * sizeof(LightSprite));
    ensure_buffer(point_lights_, 64 * sizeof(PointLight));
    ensure_buffer(halos_, 64 * sizeof(PointLight));
    ensure_buffer(boxes_, 64 * sizeof(BoxInstance));
    // An empty grid (all cells zero lights) until the first snapshot arrives.
    ensure_buffer(light_grid_, kLightGridCells * 2 * sizeof(std::uint32_t));
    std::memset(light_grid_.mapped, 0, kLightGridCells * 2 * sizeof(std::uint32_t));

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

    // Sign text: SDF glyph atlas + string table (static for the app's lifetime).
    {
        namespace st = signtext;
        sign_atlas_ = vk::create_image(ctx_, {st::kAtlasWidth, st::kAtlasHeight}, VK_FORMAT_R8_UNORM,
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        vk::Buffer staging = vk::create_buffer(ctx_, assets::sign_font_sdf.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, assets::sign_font_sdf.data(), assets::sign_font_sdf.size());
        vk::OneShot os(ctx_);
        vk::transition(ctx_, os.cmd(), {sign_atlas_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT});
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {st::kAtlasWidth, st::kAtlasHeight, 1};
        vkCmdCopyBufferToImage(os.cmd(), staging.buffer, sign_atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        vk::transition(ctx_, os.cmd(), {sign_atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
        os.submit_and_wait();
        vk::destroy(ctx_, staging);

        // GPU layout per string (uvec4): x,y,z = 12 glyph bytes, w = length | japanese << 8.
        constexpr std::size_t kCount = std::size(st::kStrings);
        sign_strings_ = vk::create_buffer(ctx_, kCount * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        auto* words = static_cast<std::uint32_t*>(sign_strings_.mapped);
        for (std::size_t i = 0; i < kCount; ++i) {
            const auto& e = st::kStrings[i];
            for (int w = 0; w < 3; ++w) {
                std::uint32_t v = 0;
                for (int k = 0; k < 4; ++k) v |= std::uint32_t{e.glyphs[w * 4 + k]} << (8 * k);
                words[i * 4 + static_cast<std::size_t>(w)] = v;
            }
            words[i * 4 + 3] = e.length | (e.japanese ? 0x100u : 0u);
        }
    }

    // Descriptor set layouts.
    {
        VkDescriptorSetLayoutBinding b[16]{};
        const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, vf, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr};
        b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr};
        b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, vf, nullptr};  // vertex: arterial culling
        b[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};  // glyph atlas
        b[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};          // sign strings
        b[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr};  // rooftop props
        b[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr};  // light sprites
        b[8] = {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};  // point lights
        b[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};  // light grid
        b[10] = {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};  // depth (halos)
        b[11] = {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};  // halos
        b[12] = {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};  // material albedo
        b[13] = {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};  // material normal/rough
        b[14] = {14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};  // box instances
        b[1].stageFlags = b[2].stageFlags = vf;  // signs: also read by ray-traced reflections
        b[15] = {15, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};  // TLAS
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = rt_ ? 16 : 15;
        ci.pBindings = b;
        VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &scene_set_layout_));
    }
    {
        VkDescriptorSetLayoutBinding b[6]{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        for (std::uint32_t i = 1; i < 6; ++i)
            b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 6;
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
        if (rt_) {
            const VkDescriptorSetLayout sets[2] = {post_set_layout_, scene_set_layout_};
            ci.setLayoutCount = 2;
            ci.pSetLayouts = sets;
            VK_CHECK(vkCreatePipelineLayout(dev, &ci, nullptr, &resolve_rt_layout_));
        }
    }

    // Scene set lives in a pool that survives resizes.
    {
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},
                                        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 1;
        ci.poolSizeCount = rt_ ? 4 : 3;
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
    VkDescriptorImageInfo atlas{linear_clamp_, sign_atlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo strings{sign_strings_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo prp{props_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo lts{lights_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo pls{point_lights_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo grid{light_grid_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo hal{halos_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo depth{point_clamp_, depth_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo malb{material_sampler_, mat_albedo_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo mnrm{material_sampler_, mat_nrm_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bxs{boxes_.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[15]{};
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
    w[4].dstBinding = 4;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[4].pImageInfo = &atlas;
    w[5].dstBinding = 5;
    w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[5].pBufferInfo = &strings;
    w[6].dstBinding = 6;
    w[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[6].pBufferInfo = &prp;
    w[7].dstBinding = 7;
    w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[7].pBufferInfo = &lts;
    w[8].dstBinding = 8;
    w[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[8].pBufferInfo = &pls;
    w[9].dstBinding = 9;
    w[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[9].pBufferInfo = &grid;
    w[10].dstBinding = 11;
    w[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[10].pBufferInfo = &hal;
    // The depth target is size-dependent: written once it exists (create_sized rewrites it).
    w[11].dstBinding = 10;
    w[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[11].pImageInfo = &depth;
    w[12].dstBinding = 12;
    w[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[12].pImageInfo = &malb;
    w[13].dstBinding = 13;
    w[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[13].pImageInfo = &mnrm;
    w[14].dstBinding = 14;
    w[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[14].pBufferInfo = &bxs;
    if (rt_ && rt_->tlas()) {
        const VkAccelerationStructureKHR tlas = rt_->tlas();
        VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        as.accelerationStructureCount = 1;
        as.pAccelerationStructures = &tlas;
        VkWriteDescriptorSet wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &as};
        wa.dstSet = scene_set_;
        wa.dstBinding = 15;
        wa.descriptorCount = 1;
        wa.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(ctx_.device(), 1, &wa, 0, nullptr);
    }
    // Depth (w[11]) is written once the size-dependent target exists.
    if (depth_.view) {
        vkUpdateDescriptorSets(ctx_.device(), 15, w, 0, nullptr);
    } else {
        std::swap(w[11], w[14]);
        vkUpdateDescriptorSets(ctx_.device(), 14, w, 0, nullptr);
    }
}

void Renderer::ensure_buffer(vk::Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage) {
    if (b.buffer && b.size >= size) return;
    vk::destroy(ctx_, b);
    // Host-visible: mobile GPUs share memory with the CPU, and these buffers change only
    // when the streaming window moves.
    b = vk::create_buffer(ctx_, std::max<VkDeviceSize>(size * 5 / 4, 256), usage, true);
}

void Renderer::create_pipelines() {
    namespace sh = apex::shaders;
    std::vector<VkFormat> scene_formats{kSceneColorFormat, kMaterialFormat};
    if (rt_) scene_formats.push_back(kLightFormat);  // G-buffer albedo for ray-traced lighting

    PipelineDesc d;
    d.layout = scene_layout_;
    d.color_formats = scene_formats;
    d.depth_format = depth_format_;
    d.depth_test = true;
    d.depth_write = true;
    d.depth_op = VK_COMPARE_OP_GREATER;

    d.vs = sh::ground_vert;
    d.fs = rt_ ? sh::ground_rt_frag : sh::ground_frag;
    ground_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::buildings_vert;
    d.fs = rt_ ? sh::buildings_rt_frag : sh::buildings_frag;
    d.cull = VK_CULL_MODE_BACK_BIT;
    buildings_pso_ = make_pipeline(ctx_, d);

    {
        // Detailed building meshes: MeshVertex (city_mesh.hpp).
        const VkVertexInputBindingDescription binding{0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, x)},
            {1, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(MeshVertex, nx)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, u)},
            {3, 0, VK_FORMAT_R32_UINT, offsetof(MeshVertex, building_material)},
        };
        PipelineDesc m = d;
        m.vs = sh::detail_vert;
        m.fs = rt_ ? sh::detail_rt_frag : sh::detail_frag;
        m.vertex_bindings = std::span(&binding, 1);
        m.vertex_attributes = attrs;
        detail_pso_ = make_pipeline(ctx_, m);
        // Instanced boxes: same fragment shader, faces generated in the vertex shader.
        PipelineDesc bd = d;
        bd.vs = sh::box_detail_vert;
        bd.fs = rt_ ? sh::detail_rt_frag : sh::detail_frag;
        box_pso_ = make_pipeline(ctx_, bd);
    }
    {
        // Smog halos: additive into the resolved image, no depth attachment (the shader
        // reads depth to fade softly into geometry).
        PipelineDesc hd;
        hd.vs = sh::halo_vert;
        hd.fs = sh::halo_frag;
        hd.layout = scene_layout_;
        hd.color_formats = {kSceneColorFormat};
        hd.blend = Blend::Additive;
        halo_pso_ = make_pipeline(ctx_, hd);
    }

    d.vs = sh::traffic_vert;
    d.fs = sh::traffic_frag;
    traffic_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::infra_vert;
    d.fs = sh::infra_frag;
    infra_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::props_vert;
    d.fs = sh::props_frag;
    d.cull = VK_CULL_MODE_NONE;  // lattice frames are seen through
    props_pso_ = make_pipeline(ctx_, d);
    d.cull = VK_CULL_MODE_BACK_BIT;

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

    d.vs = sh::beacon_vert;
    d.fs = sh::beacon_frag;
    beacon_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::signs_vert;
    d.fs = sh::signs_glow_frag;
    d.depth_op = VK_COMPARE_OP_GREATER_OR_EQUAL;
    signs_glow_pso_ = make_pipeline(ctx_, d);

    d.vs = sh::lights_vert;
    d.fs = sh::lights_frag;
    d.depth_op = VK_COMPARE_OP_GREATER;
    lights_pso_ = make_pipeline(ctx_, d);

    PipelineDesc p;
    p.layout = post_layout_;
    p.vs = sh::fullscreen_vert;
    p.color_formats = {kSceneColorFormat};
    p.fs = rt_ ? sh::resolve_rt_frag : sh::resolve_frag;
    if (rt_) p.layout = resolve_rt_layout_;
    resolve_pso_ = make_pipeline(ctx_, p);
    p.layout = post_layout_;

    p.fs = sh::taa_frag;
    taa_pso_ = make_pipeline(ctx_, p);

    if (rt_) {
        PipelineDesc r = p;
        r.layout = resolve_rt_layout_;
        r.fs = sh::rt_light_rt_frag;
        r.color_formats = {kLightFormat, kLightFormat};
        rt_light_pso_ = make_pipeline(ctx_, r);
        r.layout = post_layout_;
        r.fs = sh::rt_accum_frag;
        r.color_formats = {kLightFormat, kMetaFormat};
        rt_accum_pso_ = make_pipeline(ctx_, r);
        r.fs = sh::rt_atrous_frag;
        r.color_formats = {kLightFormat};
        rt_atrous_pso_ = make_pipeline(ctx_, r);
        r.fs = sh::rt_composite_frag;
        r.color_formats = {kSceneColorFormat};
        r.blend = Blend::Additive;
        rt_composite_pso_ = make_pipeline(ctx_, r);
    }

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
    depth_ = vk::create_image(ctx_, internal_, depth_format_,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resolved_ = vk::create_image(ctx_, internal_, kSceneColorFormat, rt);
    for (auto& t : taa_) t = vk::create_image(ctx_, internal_, kSceneColorFormat, rt);
    taa_valid_ = false;
    if (rt_) {
        albedo_ = vk::create_image(ctx_, internal_, kLightFormat, rt);
        rt_irr_ = vk::create_image(ctx_, internal_, kLightFormat, rt);
        rt_spec_ = vk::create_image(ctx_, internal_, kLightFormat, rt);
        for (auto& i : irr_hist_) i = vk::create_image(ctx_, internal_, kLightFormat, rt);
        for (auto& i : meta_hist_) i = vk::create_image(ctx_, internal_, kMetaFormat, rt);
        for (auto& i : atrous_) i = vk::create_image(ctx_, internal_, kLightFormat, rt);
        rt_hist_valid_ = false;
    }
    VkExtent2D e = internal_;
    for (auto& b : bloom_) {
        e = {std::max(1u, e.width / 2), std::max(1u, e.height / 2)};
        b = vk::create_image(ctx_, e, bloom_format_, rt);
    }

    VkDevice dev = ctx_.device();
    constexpr std::uint32_t kSets = 2 + 2 * kBloomLevels + 6 + 10;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSets},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 5}};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = kSets;
    ci.poolSizeCount = 2;
    ci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(dev, &ci, nullptr, &sized_pool_));

    // Post sets: UBO + up to 5 images (bindings 1-5). Unused slots still need a valid
    // descriptor; they point at the first image.
    auto alloc_n = [&](std::initializer_list<std::pair<VkImageView, VkSampler>> images) {
        VkDescriptorSet set;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = sized_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &post_set_layout_;
        VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &set));
        VkDescriptorBufferInfo ubo{frame_ubo_.buffer, 0, sizeof(FrameUniforms)};
        VkDescriptorImageInfo imgs[5];
        const auto first = *images.begin();
        std::size_t i = 0;
        for (const auto& [view, sampler] : images) {
            imgs[i++] = {sampler ? sampler : first.second, view ? view : first.first,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        for (; i < 5; ++i) imgs[i] = {first.second, first.first, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[6]{};
        for (std::uint32_t k = 0; k < 6; ++k) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = set;
            w[k].dstBinding = k;
            w[k].descriptorCount = 1;
            if (k == 0) {
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                w[k].pBufferInfo = &ubo;
            } else {
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[k].pImageInfo = &imgs[k - 1];
            }
        }
        vkUpdateDescriptorSets(dev, 6, w, 0, nullptr);
        return set;
    };
    auto alloc = [&](VkImageView a, VkSampler sa, VkImageView b, VkSampler sb, VkImageView c, VkSampler sc) {
        return alloc_n({{a, sa}, {b, sb}, {c, sc}});
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
    // TAA writes taa_[parity] from resolved_ and taa_[1 - parity]; bloom and tonemap read it.
    for (std::uint32_t i = 0; i < 2; ++i) {
        taa_sets_[i] = alloc(resolved_.view, point_clamp_, taa_[1 - i].view, linear_clamp_, depth_.view, point_clamp_);
        bloom0_sets_[i] = alloc(taa_[i].view, linear_clamp_, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
        tonemap_sets_[i] = alloc(taa_[i].view, linear_clamp_, bloom_[0].view, linear_clamp_, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    if (rt_) {
        const VkSampler pc = point_clamp_;
        rt_light_set_ = alloc_n({{scene_material_.view, pc}, {albedo_.view, pc}, {depth_.view, pc}});
        for (std::uint32_t par = 0; par < 2; ++par) {
            rt_accum_sets_[par] = alloc_n({{rt_irr_.view, pc}, {irr_hist_[1 - par].view, pc},
                                           {meta_hist_[1 - par].view, pc}, {depth_.view, pc}});
            // Filter passes: history -> atrous 0 -> atrous 1 -> atrous 0.
            const VkImageView inputs[3] = {irr_hist_[par].view, atrous_[0].view, atrous_[1].view};
            for (int k = 0; k < 3; ++k)
                rt_atrous_sets_[par][static_cast<std::size_t>(k)] =
                    alloc_n({{inputs[k], pc}, {depth_.view, pc}, {scene_material_.view, pc}, {meta_hist_[par].view, pc}});
        }
        rt_composite_set_ = alloc_n({{atrous_[0].view, pc}, {rt_spec_.view, pc}, {albedo_.view, pc}, {depth_.view, pc}});
    }
    if (scene_set_) write_scene_set();  // the halo pass samples the new depth target
}

void Renderer::destroy_sized() {
    vk::destroy(ctx_, scene_color_);
    vk::destroy(ctx_, scene_material_);
    vk::destroy(ctx_, depth_);
    vk::destroy(ctx_, resolved_);
    for (auto& t : taa_) vk::destroy(ctx_, t);
    for (vk::Image* i : {&albedo_, &rt_irr_, &rt_spec_, &irr_hist_[0], &irr_hist_[1], &meta_hist_[0], &meta_hist_[1],
                         &atrous_[0], &atrous_[1]})
        vk::destroy(ctx_, *i);
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

void Renderer::upload_materials_images(const MaterialTextures& t) {
    vk::destroy(ctx_, mat_albedo_);
    vk::destroy(ctx_, mat_nrm_);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const std::uint32_t layers = MaterialTextures::kLayers;
    mat_albedo_ = vk::create_image(ctx_, {t.size, t.size}, VK_FORMAT_R8G8B8A8_SRGB, usage, t.mips, layers);
    mat_nrm_ = vk::create_image(ctx_, {t.nrm_size, t.nrm_size}, VK_FORMAT_R8G8B8A8_UNORM, usage, t.nrm_mips, layers);
    const std::size_t bytes = t.albedo.size() + t.nrm.size();
    vk::Buffer staging = vk::create_buffer(ctx_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, t.albedo.data(), t.albedo.size());
    std::memcpy(static_cast<char*>(staging.mapped) + t.albedo.size(), t.nrm.data(), t.nrm.size());
    {
        vk::OneShot os(ctx_);
        for (const vk::Image* img : {&mat_albedo_, &mat_nrm_}) {
            vk::transition(ctx_, os.cmd(), {img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                                            VK_ACCESS_2_TRANSFER_WRITE_BIT});
            // Data is layer-major, each layer a full mip chain.
            std::vector<VkBufferImageCopy> copies;
            VkDeviceSize offset = img == &mat_albedo_ ? 0 : t.albedo.size();
            const bool alb = img == &mat_albedo_;
            for (std::uint32_t l = 0; l < layers; ++l)
                for (std::uint32_t m = 0, s = alb ? t.size : t.nrm_size; m < (alb ? t.mips : t.nrm_mips);
                     ++m, s = std::max(1u, s / 2)) {
                    VkBufferImageCopy c{};
                    c.bufferOffset = offset;
                    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, l, 1};
                    c.imageExtent = {s, s, 1};
                    copies.push_back(c);
                    offset += VkDeviceSize{s} * s * 4;
                }
            vkCmdCopyBufferToImage(os.cmd(), staging.buffer, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(copies.size()), copies.data());
            vk::transition(ctx_, os.cmd(), {img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
        }
        os.submit_and_wait();
    }
    vk::destroy(ctx_, staging);
}

void Renderer::upload_materials(const MaterialTextures& t) {
    vkDeviceWaitIdle(ctx_.device());
    upload_materials_images(t);
    write_scene_set();
}

void Renderer::upload_world(const CitySnapshot& snap) {
    vkDeviceWaitIdle(ctx_.device());
    const VkDeviceSize bsize = snap.buildings.size() * sizeof(BuildingInstance);
    const VkDeviceSize ssize = snap.signs.size() * sizeof(SignInstance);
    ensure_buffer(buildings_, bsize);
    ensure_buffer(signs_, ssize);
    const VkDeviceSize psize = snap.props.size() * sizeof(PropInstance);
    const VkDeviceSize lsize = snap.lights.size() * sizeof(LightSprite);
    ensure_buffer(props_, psize);
    ensure_buffer(lights_, lsize);
    if (bsize) std::memcpy(buildings_.mapped, snap.buildings.data(), bsize);
    if (ssize) std::memcpy(signs_.mapped, snap.signs.data(), ssize);
    if (psize) std::memcpy(props_.mapped, snap.props.data(), psize);
    if (lsize) std::memcpy(lights_.mapped, snap.lights.data(), lsize);
    const VkDeviceSize vsize = snap.mesh.vertices.size() * sizeof(MeshVertex);
    const VkDeviceSize isize = snap.mesh.indices.size() * sizeof(std::uint32_t);
    ensure_buffer(mesh_vertices_, vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensure_buffer(mesh_indices_, isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (vsize) std::memcpy(mesh_vertices_.mapped, snap.mesh.vertices.data(), vsize);
    if (isize) std::memcpy(mesh_indices_.mapped, snap.mesh.indices.data(), isize);
    mesh_chunks_ = snap.mesh.chunks;
    const VkDeviceSize plsize = snap.point_lights.size() * sizeof(PointLight);
    const VkDeviceSize gsize = snap.light_grid.size() * sizeof(std::uint32_t);
    ensure_buffer(point_lights_, plsize);
    ensure_buffer(light_grid_, gsize);
    if (plsize) std::memcpy(point_lights_.mapped, snap.point_lights.data(), plsize);
    if (gsize) std::memcpy(light_grid_.mapped, snap.light_grid.data(), gsize);
    const VkDeviceSize hsize = snap.halos.size() * sizeof(PointLight);
    ensure_buffer(halos_, hsize);
    if (hsize) std::memcpy(halos_.mapped, snap.halos.data(), hsize);
    halo_count_ = static_cast<std::uint32_t>(snap.halos.size());
    const VkDeviceSize bxsize = snap.mesh.boxes.size() * sizeof(BoxInstance);
    ensure_buffer(boxes_, bxsize);
    if (bxsize) std::memcpy(boxes_.mapped, snap.mesh.boxes.data(), bxsize);
    building_count_ = static_cast<std::uint32_t>(snap.buildings.size());
    sign_count_ = static_cast<std::uint32_t>(snap.signs.size());
    prop_count_ = static_cast<std::uint32_t>(snap.props.size());
    light_count_ = static_cast<std::uint32_t>(snap.lights.size());

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
    if (rt_) rt_->build(snap);
    write_scene_set();  // buffers may have been reallocated
}

void Renderer::update_frame_ubo(std::uint32_t slot, const Game& game) {
    const Camera& cam = game.camera();
    const float aspect = static_cast<float>(output_extent_.width) / static_cast<float>(output_extent_.height);
    FrameUniforms u{};
    u.view = cam.view();
    u.proj = cam.projection(aspect);
    view_proj_ = u.proj * u.view;  // unjittered: CPU culling and next frame's reprojection
    // TAA: sub-pixel Halton(2,3) jitter, 8-frame cycle.
    auto halton = [](std::uint32_t i, std::uint32_t b) {
        float f = 1.0f, r = 0.0f;
        for (; i > 0; i /= b) {
            f /= static_cast<float>(b);
            r += f * static_cast<float>(i % b);
        }
        return r;
    };
    const std::uint32_t k = frame_index_ % 8u + 1u;
    const float jx = (halton(k, 2) - 0.5f) * 2.0f / static_cast<float>(internal_.width);
    const float jy = (halton(k, 3) - 0.5f) * 2.0f / static_cast<float>(internal_.height);
    Mat4 jittered = u.proj;
    for (int c = 0; c < 4; ++c) {
        jittered.at(c, 0) += jx * jittered.at(c, 3);
        jittered.at(c, 1) += jy * jittered.at(c, 3);
    }
    u.proj = jittered;
    u.view_proj = jittered * u.view;
    u.inv_view_proj = inverse(u.view_proj);
    u.prev_view_proj = have_prev_ ? prev_view_proj_ : view_proj_;
    prev_view_proj_ = view_proj_;
    have_prev_ = true;
    u.taa[0] = jx;
    u.taa[1] = jy;
    u.taa[2] = static_cast<float>(frame_index_ % 1024u);
    u.camera_pos[0] = cam.position.x;
    u.camera_pos[1] = cam.position.y;
    u.camera_pos[2] = cam.position.z;
    u.camera_pos[3] = game.time();
    std::memcpy(u.road_field, road_params_.data(), sizeof(u.road_field));
    u.viewport[0] = static_cast<float>(internal_.width);
    u.viewport[1] = static_cast<float>(internal_.height);
    u.viewport[2] = 1.0f / u.viewport[0];
    u.viewport[3] = 1.0f / u.viewport[1];
    u.fog[0] = settings_.fog_density * (1.0f - 0.45f * settings_.daylight);  // clearer air at dusk
    u.fog[1] = 0.018f;
    u.fog[2] = 0.97f;
    // Dusk is dry and clear; night keeps its rain.
    u.fog[3] = settings_.rain * (1.0f - settings_.daylight);
    {
        // Low sun, ~9 degrees up, in the south-west.
        const Vec3 sun = normalize(Vec3{-0.62f, -0.76f, 0.16f});
        u.sun[0] = sun.x;
        u.sun[1] = sun.y;
        u.sun[2] = sun.z;
        u.sun[3] = settings_.daylight;
    }
    const Gig& gig = game.gig();
    u.objective[0] = gig.target.x;
    u.objective[1] = gig.target.y;
    u.objective[2] = gig.elapsed;
    u.objective[3] = 1.0f;
    const Car& car = game.car();
    u.player_car[0] = car.position.x;
    u.player_car[1] = car.position.y;
    u.player_car[2] = car.yaw;
    u.player_car[3] = car.spawned ? 1.0f : 0.0f;
    std::memcpy(static_cast<char*>(frame_ubo_.mapped) + slot * ubo_stride_, &u, sizeof(u));
}

void Renderer::fullscreen_pass(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent, VkPipeline pso,
                               VkDescriptorSet set, std::uint32_t slot, const void* push, std::uint32_t push_size,
                               bool load, bool with_scene_set) {
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
    // Ray-traced resolve: scene set (TLAS, signs) at set 1, same frame slot.
    const VkPipelineLayout layout = with_scene_set ? resolve_rt_layout_ : post_layout_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 1, &offset);
    if (with_scene_set)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &scene_set_, 1, &offset);
    if (push_size) vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size, push);
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

        if (rt_)
            vk::transition(ctx_, cmd, {albedo_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       kAnyFragmentWork, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
        VkRenderingAttachmentInfo colors[3]{};
        for (auto& c : colors) {
            c.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            c.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        colors[0].imageView = scene_color_.view;
        colors[1].imageView = scene_material_.view;
        colors[1].clearValue.color = {{0.0f, 1.0f, 0.5f, 0.5f}};
        colors[2].imageView = albedo_.view;  // rt_ only (colorAttachmentCount below)
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {0.0f, 0};  // reversed-Z far plane

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, internal_};
        ri.layerCount = 1;
        ri.colorAttachmentCount = rt_ ? 3 : 2;
        ri.pColorAttachments = colors;
        ri.pDepthAttachment = &depth;
        fns.cmd_begin_rendering(cmd, &ri);
        set_viewport(cmd, internal_);

        const std::uint32_t offset = static_cast<std::uint32_t>(slot * ubo_stride_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_layout_, 0, 1, &scene_set_, 1, &offset);

        // Front-to-back-ish: buildings occlude most of the ground.
        if (!mesh_chunks_.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, detail_pso_);
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh_vertices_.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, mesh_indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
            const Frustum fr = frustum_of(view_proj_);
            for (const MeshChunk& c : mesh_chunks_)
                if (c.index_count && fr.visible(c.min, c.max)) vkCmdDrawIndexed(cmd, c.index_count, 1, c.first_index, 0, 0);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, box_pso_);
            // Box parts are centimetre-to-metre relief: past ~400 m they are sub-pixel, so
            // whole tiles skip them there.
            const Vec3 cam = game.camera().position;
            for (const MeshChunk& c : mesh_chunks_) {
                if (!c.box_count || !fr.visible(c.min, c.max)) continue;
                const float dx = std::max({c.min[0] - cam.x, 0.0f, cam.x - c.max[0]});
                const float dy = std::max({c.min[1] - cam.y, 0.0f, cam.y - c.max[1]});
                if (dx * dx + dy * dy > 400.0f * 400.0f) continue;
                vkCmdDraw(cmd, 36, c.box_count, 0, c.first_box);
            }
        }
        if (building_count_) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, buildings_pso_);
            vkCmdDraw(cmd, 30, building_count_, 0, 0);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, traffic_pso_);
        {
            // Expressways: 5 boxes per segment, 2 families x 7 lines x 61 segments (infra.vert).
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, infra_pso_);
            vkCmdDraw(cmd, 5 * 36, 2 * 7 * 61, 0, 0);
        }
        if (prop_count_) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, props_pso_);
            vkCmdDraw(cmd, 36, prop_count_, 0, 0);
        }
        // Two boxes per vehicle: body + cabin.
        if (settings_.traffic_count) vkCmdDraw(cmd, 72, settings_.traffic_count, 0, 0);
        if (game.car().spawned) vkCmdDraw(cmd, 72, 1, 0, kPlayerCarInstance);
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
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, beacon_pso_);
        vkCmdDraw(cmd, 12, 1, 0, 0);
        if (sign_count_) {
            // Free-standing neon lettering, additive over whatever is behind it.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, signs_glow_pso_);
            vkCmdDraw(cmd, 6, sign_count_, 0, 0);
        }
        if (light_count_) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lights_pso_);
            vkCmdDraw(cmd, 6, light_count_, 0, 0);
        }
        if (settings_.rain * (1.0f - settings_.daylight) > 0.0f) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rain_pso_);
            vkCmdDraw(cmd, 6, 6000, 0, 0);
        }
        fns.cmd_end_rendering(cmd);
    }

    // ---- Ray-traced lighting (G-buffer -> denoised light added into scene colour) --
    const bool rt_lit = rt_ && rt_->tlas();
    if (rt_lit) record_rt_lighting(cmd, slot);
    // After ray-traced lighting the material and depth targets are already readable.
    const VkImageLayout mat_old = rt_lit ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkImageLayout depth_old = rt_lit ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

    // ---- SSR resolve ---------------------------------------------------------------
    {
        const ImageTransition ts[] = {
            {scene_color_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT},
            {scene_material_.image, mat_old, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT},
            {depth_.image, depth_old, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT},
            {resolved_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kAnyFragmentWork, 0,
             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
        };
        vk::transition(ctx_, cmd, ts);
        const std::int32_t steps[4] = {settings_.ssr_steps, 0, 0, 0};
        fullscreen_pass(cmd, resolved_.view, internal_, resolve_pso_, resolve_set_, slot, steps, sizeof(steps), false,
                        rt_ != nullptr);
        if (halo_count_) {
            // Resolve wrote this attachment: order it before the additive halos.
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            fns.cmd_pipeline_barrier2(cmd, &dep);
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = resolved_.view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, internal_};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &color;
            fns.cmd_begin_rendering(cmd, &ri);
            set_viewport(cmd, internal_);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, halo_pso_);
            const std::uint32_t offset = static_cast<std::uint32_t>(slot * ubo_stride_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_layout_, 0, 1, &scene_set_, 1, &offset);
            vkCmdDraw(cmd, 6, halo_count_, 0, 0);
            fns.cmd_end_rendering(cmd);
        }
        vk::transition(ctx_, cmd, {resolved_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
    }

    // ---- TAA ---------------------------------------------------------------------
    const std::uint32_t parity = frame_index_ & 1u;
    {
        vk::Image& out = taa_[parity];
        vk::Image& hist = taa_[1 - parity];
        if (!taa_valid_)  // history never written at this size: give it a defined layout
            vk::transition(ctx_, cmd, {hist.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       kAnyFragmentWork, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
        vk::transition(ctx_, cmd, {out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   kAnyFragmentWork, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
        const std::int32_t reset[4] = {taa_valid_ ? 0 : 1, 0, 0, 0};
        fullscreen_pass(cmd, out.view, internal_, taa_pso_, taa_sets_[parity], slot, reset, sizeof(reset), false);
        vk::transition(ctx_, cmd, {out.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
        taa_valid_ = true;
        bloom_down_sets_[0] = bloom0_sets_[parity];
        tonemap_set_ = tonemap_sets_[parity];
    }
    frame_index_ += 1;

    // ---- Bloom -------------------------------------------------------------------
    {
        VkExtent2D src_extent = internal_;
        for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
            vk::transition(ctx_, cmd, {bloom_[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       kAnyFragmentWork, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
            const float push[4] = {1.0f / static_cast<float>(src_extent.width),
                                   1.0f / static_cast<float>(src_extent.height), i == 0 ? 1.0f : 0.0f,
                                   // Threshold: neon / screens bloom, lit windows don't; by day
                                   // sunlit walls are brighter, so the bar rises.
                                   1.2f * (1.0f + 3.0f * settings_.daylight)};
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
                                   1.0f / static_cast<float>(src.extent.height), 1.3f, 0.0f};  // wider tent: hazier glow
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
        } push{settings_.exposure * (1.0f - 0.55f * settings_.daylight), settings_.bloom_strength, game.time(),
               srgb ? 1.0f : 0.0f, target.pre_rotation, {}};
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

void Renderer::record_rt_lighting(VkCommandBuffer cmd, std::uint32_t slot) {
    const auto& fns = ctx_.fns();
    const std::uint32_t par = frame_index_ & 1u;  // same parity as TAA (frame_index_ advances after it)
    const std::uint32_t offset = static_cast<std::uint32_t>(slot * ubo_stride_);
    auto to_read = [&](VkImage img, VkImageLayout from, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        const bool depth = aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
        vk::transition(ctx_, cmd, {img, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   depth ? VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                                         : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   depth ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, aspect});
    };
    auto to_write = [&](VkImage img) {
        vk::transition(ctx_, cmd, {img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   kAnyFragmentWork, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
    };
    // Fullscreen triangle into 1-2 targets.
    auto pass = [&](std::initializer_list<VkImageView> targets, VkPipeline pso, VkPipelineLayout layout,
                    VkDescriptorSet set, bool scene_set, const void* push, std::uint32_t push_size, bool load) {
        VkRenderingAttachmentInfo att[2]{};
        std::uint32_t n = 0;
        for (VkImageView v : targets) {
            att[n].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            att[n].imageView = v;
            att[n].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att[n].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ++n;
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, internal_};
        ri.layerCount = 1;
        ri.colorAttachmentCount = n;
        ri.pColorAttachments = att;
        fns.cmd_begin_rendering(cmd, &ri);
        set_viewport(cmd, internal_);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 1, &offset);
        if (scene_set) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &scene_set_, 1, &offset);
        if (push_size) vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        fns.cmd_end_rendering(cmd);
    };

    // G-buffer readable; scene colour stays an attachment for the composite.
    to_read(scene_material_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    to_read(albedo_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    to_read(depth_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!rt_hist_valid_)  // first frame at this size: give the history a defined layout
        for (vk::Image* h : {&irr_hist_[1 - par], &meta_hist_[1 - par]})
            vk::transition(ctx_, cmd, {h->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       kAnyFragmentWork, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});

    // 1. Noisy estimate.
    to_write(rt_irr_.image);
    to_write(rt_spec_.image);
    pass({rt_irr_.view, rt_spec_.view}, rt_light_pso_, resolve_rt_layout_, rt_light_set_, true, nullptr, 0, false);
    to_read(rt_irr_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    to_read(rt_spec_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // 2. Temporal accumulation into this parity's history.
    to_write(irr_hist_[par].image);
    to_write(meta_hist_[par].image);
    const std::int32_t reset[4] = {rt_hist_valid_ ? 0 : 1, 0, 0, 0};
    pass({irr_hist_[par].view, meta_hist_[par].view}, rt_accum_pso_, post_layout_, rt_accum_sets_[par], false, reset,
         sizeof(reset), false);
    to_read(irr_hist_[par].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    to_read(meta_hist_[par].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rt_hist_valid_ = true;

    // 3. A-trous filter: history -> atrous 0 -> atrous 1 -> atrous 0 (steps 1, 2, 4).
    const std::uint32_t outs[3] = {0, 1, 0};
    for (int k = 0; k < 3; ++k) {
        vk::Image& out = atrous_[outs[k]];
        to_write(out.image);
        const std::int32_t step[4] = {1 << k, 0, 0, 0};
        pass({out.view}, rt_atrous_pso_, post_layout_, rt_atrous_sets_[par][static_cast<std::size_t>(k)], false, step,
             sizeof(step), false);
        to_read(out.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    // 4. Composite: additive into the scene colour (still an attachment).
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        fns.cmd_pipeline_barrier2(cmd, &di);
    }
    pass({scene_color_.view}, rt_composite_pso_, post_layout_, rt_composite_set_, false, nullptr, 0, true);
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
