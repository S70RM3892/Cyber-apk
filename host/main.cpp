// Headless host runner: drives the same Game + Renderer as the APK on any Vulkan 1.3
// device (lavapipe works), then writes the last frame to a PNG. Used for visual
// checks and CI screenshots without a phone.
//
//   apex_host --out city.png [--width 1920 --height 1080] [--frames 60] [--seed 2077]
//             [--x X --y Y --yaw RAD --pitch RAD] [--walk SECONDS] [--scale 0.67]
//   apex_host --present N   run N frames through the swapchain Presenter (the APK's
//                           acquire/submit/present path) on a VK_EXT_headless_surface
#include <zlib.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "apex/game.hpp"
#include "apex/hud.hpp"
#include "presenter.hpp"
#include "renderer.hpp"

namespace {

bool write_png(const char* path, const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> raw;
    raw.reserve(std::size_t{h} * (w * 4 + 1));
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), rgba + std::size_t{y} * w * 4, rgba + std::size_t{y + 1} * w * 4);
    }
    uLongf zsize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> z(zsize);
    if (compress2(z.data(), &zsize, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return false;
    z.resize(zsize);

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    auto be32 = [](std::uint32_t v, std::uint8_t* o) {
        o[0] = std::uint8_t(v >> 24); o[1] = std::uint8_t(v >> 16); o[2] = std::uint8_t(v >> 8); o[3] = std::uint8_t(v);
    };
    auto chunk = [&](const char* type, const std::uint8_t* data, std::uint32_t len) {
        std::uint8_t hdr[8];
        be32(len, hdr);
        std::memcpy(hdr + 4, type, 4);
        std::fwrite(hdr, 1, 8, f);
        if (len) std::fwrite(data, 1, len, f);
        uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
        if (len) crc = crc32(crc, data, len);
        std::uint8_t c[4];
        be32(static_cast<std::uint32_t>(crc), c);
        std::fwrite(c, 1, 4, f);
    };
    static const std::uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);
    std::uint8_t ihdr[13];
    be32(w, ihdr);
    be32(h, ihdr + 4);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;  // 8-bit RGBA
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), static_cast<std::uint32_t>(z.size()));
    chunk("IEND", nullptr, 0);
    std::fclose(f);
    return true;
}

struct Args {
    std::string out = "apex.png";
    std::uint32_t width = 1920, height = 1080;
    int frames = 30;
    std::uint64_t seed = 2077;
    bool has_pos = false;
    float x = 0, y = 0, yaw = 0, pitch = 0.08f;
    float walk_seconds = 0;
    float scale = 0.67f;
    bool validation = true;
    int present_frames = 0;
    bool hud = true;
    bool hud_stick = false;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (k == "--out") a.out = next();
        else if (k == "--width") a.width = static_cast<std::uint32_t>(std::stoul(next()));
        else if (k == "--height") a.height = static_cast<std::uint32_t>(std::stoul(next()));
        else if (k == "--frames") a.frames = std::stoi(next());
        else if (k == "--seed") a.seed = std::stoull(next());
        else if (k == "--x") { a.x = std::stof(next()); a.has_pos = true; }
        else if (k == "--y") { a.y = std::stof(next()); a.has_pos = true; }
        else if (k == "--yaw") a.yaw = std::stof(next());
        else if (k == "--pitch") a.pitch = std::stof(next());
        else if (k == "--walk") a.walk_seconds = std::stof(next());
        else if (k == "--scale") a.scale = std::stof(next());
        else if (k == "--no-validation") a.validation = false;
        else if (k == "--present") a.present_frames = std::stoi(next());
        else if (k == "--no-hud") a.hud = false;
        else if (k == "--hud-stick") a.hud_stick = true;
        else std::fprintf(stderr, "unknown argument %s\n", k.c_str());
    }
    return a;
}

}  // namespace

int run_present(const Args& args) {
    using namespace apex;
    vk::ContextDesc desc;
    desc.enable_validation = args.validation;
    desc.instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    desc.device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    vk::Context ctx(desc);

    auto create_headless = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
        vkGetInstanceProcAddr(ctx.instance(), "vkCreateHeadlessSurfaceEXT"));
    VkHeadlessSurfaceCreateInfoEXT hci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface;
    VK_CHECK(create_headless(ctx.instance(), &hci, nullptr, &surface));
    ctx.create_device(desc, surface);

    Game game(args.seed);
    Presenter presenter(ctx);
    const VkFormat format = Presenter::choose_format(ctx.physical_device(), surface);
    presenter.attach(surface, format, {args.width, args.height});
    RenderSettings settings;
    settings.render_scale = args.scale;
    Renderer renderer(ctx, format, presenter.logical_extent(), settings);

    bool dirty = true;
    HudBuilder hud;
    for (int i = 0; i < args.present_frames; ++i) {
        Input in;
        in.move_y = 1.0f;
        in.sprint = true;  // cross tile boundaries to exercise streaming uploads mid-flight
        in.look_dx = 0.01f;
        game.update(1.0f / 30.0f, in);
        dirty |= game.take_world_dirty();
        HudInput hi;
        hi.width = static_cast<float>(presenter.logical_extent().width);
        hi.height = static_cast<float>(presenter.logical_extent().height);
        hi.render_scale = renderer.settings().render_scale;
        hi.gpu_ms = renderer.gpu_ms();
        build_hud(hud, game, hi);
        if (presenter.frame(game, renderer, dirty, hud.quads())) dirty = false;
        renderer.update_dynamic_resolution();
    }
    vkDeviceWaitIdle(ctx.device());
    const Vec3 p = game.camera().position;
    std::printf("presented=%llu of %d frames, format=%d, final pos=(%.1f, %.1f), render scale %.2f\n",
                static_cast<unsigned long long>(presenter.frames_presented()), args.present_frames, int(format), p.x,
                p.y, static_cast<double>(renderer.settings().render_scale));
    return presenter.frames_presented() > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    using namespace apex;
    const Args args = parse(argc, argv);
    if (args.present_frames > 0) return run_present(args);

    vk::ContextDesc desc;
    desc.enable_validation = args.validation;
    vk::Context ctx(desc);
    ctx.create_device(desc);

    Game game(args.seed);
    game.camera().yaw = args.yaw;
    game.camera().pitch = args.pitch;
    if (args.has_pos) game.camera().position = {args.x, args.y, Game::kEyeHeight};

    const VkExtent2D extent{args.width, args.height};
    RenderSettings settings;
    settings.render_scale = args.scale;
    settings.dynamic_resolution = false;  // deterministic screenshots
    Renderer renderer(ctx, VK_FORMAT_R8G8B8A8_UNORM, extent, settings);
    HudBuilder hud;

    vk::Image output = vk::create_image(ctx, extent, VK_FORMAT_R8G8B8A8_UNORM,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    vk::Buffer readback = vk::create_buffer(ctx, VkDeviceSize{args.width} * args.height * 4,
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);

    VkCommandPool pool;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.queue_family();
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pci, nullptr, &pool));
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cai, &cmd));

    const OutputTarget target{output.image, output.view, output.format, extent, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    const float dt = 1.0f / 30.0f;
    double gpu_ms = 0;
    for (int frame = 0; frame < args.frames; ++frame) {
        Input in;
        if (game.time() < args.walk_seconds) in.move_y = 1.0f;
        game.update(dt, in);
        if (game.take_world_dirty()) renderer.upload_world(*game.world().snapshot());

        const auto t0 = std::chrono::steady_clock::now();
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        HudInput hi;
        hi.width = static_cast<float>(extent.width);
        hi.height = static_cast<float>(extent.height);
        hi.fps = 60.0f;
        hi.render_scale = settings.render_scale;
        if (args.hud_stick) {
            hi.stick = {true, hi.width * 0.15f, hi.height * 0.75f, 0.3f, 0.8f, hi.height * 0.12f};
        }
        if (args.hud) build_hud(hud, game, hi);
        renderer.record(cmd, static_cast<std::uint32_t>(frame) % Renderer::kFramesInFlight, game, target,
                        args.hud ? std::span<const HudQuad>(hud.quads()) : std::span<const HudQuad>{});
        if (frame == args.frames - 1) {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        }
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(ctx.queue(), 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.queue()));
        gpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    const auto* px = static_cast<const std::uint8_t*>(readback.mapped);
    double sum = 0;
    std::size_t bright = 0;
    const std::size_t n = std::size_t{extent.width} * extent.height;
    for (std::size_t i = 0; i < n; ++i) {
        const double l = 0.2126 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.0722 * px[i * 4 + 2];
        sum += l;
        bright += l > 200.0;
    }
    const Vec3 p = game.camera().position;
    std::printf("frames=%d avg_frame_ms=%.1f (cpu-emulated GPU) buildings=%zu signs=%zu pos=(%.1f, %.1f) "
                "mean_luma=%.1f bright_px=%.2f%%\n",
                args.frames, gpu_ms / args.frames, game.world().snapshot()->buildings.size(),
                game.world().snapshot()->signs.size(), p.x, p.y, sum / static_cast<double>(n),
                100.0 * static_cast<double>(bright) / static_cast<double>(n));

    const bool ok = write_png(args.out.c_str(), px, extent.width, extent.height);
    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    vk::destroy(ctx, readback);
    vk::destroy(ctx, output);
    return ok ? 0 : 1;
}
