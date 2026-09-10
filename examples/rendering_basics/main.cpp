/*
 * @file main.cpp
 * @brief Example of the Catalyst rendering API's per-frame flow: window → device → swapchain → record → submit → present.
 * @details Opens a window with the platform module, creates a rendering device and a swapchain bound to the window's
 * native handle, uploads a triangle into a structured vertex buffer, builds a graphics pipeline from the embedded
 * SPIR-V in shaders.hpp (compiled from shaders/triangle.vert and .frag by scripts/embed_spirv.py), and then for a fixed
 * number of frames acquires a back buffer, records a render pass that clears it to a slowly changing colour and draws
 * the triangle, submits the command list and presents. Backends that do not consume SPIR-V skip the pipeline and only
 * clear. If the swapchain reports it is out of date (window resized or minimised) it is recreated at the current
 * client size.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "shaders.hpp"

#include <catalyst/catalyst.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <span>
#include <thread>

namespace logging = catalyst::logging;

namespace
{
    /** @brief Names this example in the log's category column. */
    struct example_log
    {
        static constexpr const char *name = "rendering_basics";
    };

    struct vertex
    {
        float x, y, z;
        float r, g, b;
    };

    constexpr vertex triangle[] = {
        {0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {-0.5f, -0.5f, 0.0f, 0.0f, 1.0f, 0.0f},
        {0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f},
    };

    /** Builds the triangle pipeline; returns an invalid handle when the backend cannot consume SPIR-V. */
    catalyst::rendering::pipeline make_triangle_pipeline(const catalyst::rendering::device &dev,
                                                         catalyst::rendering::format color_format)
    {
        using namespace catalyst::rendering;

        if (native_bytecode_format(backend()) != shader_bytecode_format::spirv)
        {
            logging::warn<example_log>("Backend {} does not consume SPIR-V; drawing only the clear colour",
                                       module_name());
            return {};
        }

        shader_desc vs_desc;
        vs_desc.stage = shader_stage::vertex;
        vs_desc.bytecode = rendering_basics::spirv::triangle_vertex_bytes();
        vs_desc.debug_name = "triangle.vert";
        shader vs = create_shader(dev, vs_desc);

        shader_desc fs_desc;
        fs_desc.stage = shader_stage::fragment;
        fs_desc.bytecode = rendering_basics::spirv::triangle_fragment_bytes();
        fs_desc.debug_name = "triangle.frag";
        shader fs = create_shader(dev, fs_desc);

        const std::array<vertex_binding, 1> bindings = {vertex_binding{0, sizeof(vertex), vertex_input_rate::per_vertex}};
        const std::array<vertex_attribute, 2> attributes = {
            vertex_attribute{0, 0, format::rgb32_float, offsetof(vertex, x)},
            vertex_attribute{1, 0, format::rgb32_float, offsetof(vertex, r)},
        };
        const std::array<format, 1> color_formats = {color_format};

        graphics_pipeline_desc desc;
        desc.vertex_shader = vs;
        desc.fragment_shader = fs;
        desc.vertex_input = {bindings, attributes};
        desc.color_formats = color_formats;
        desc.rasterizer.cull = cull_mode::none;
        desc.debug_name = "triangle";
        pipeline p = create_graphics_pipeline(dev, desc);

        // Pipelines keep no reference to their shader modules.
        destroy_shader(vs);
        destroy_shader(fs);

        if (!p)
            logging::error<example_log>("Failed to create the triangle pipeline; drawing only the clear colour");
        return p;
    }
} // namespace

int main()
{
    using namespace catalyst;
    catalyst_version_anchor();

    // One console sink, and every line below reaches the terminal, coloured when the terminal
    // understands colour. Sending the same log to a file is one more add_sink, and no change here.
    logging::default_logger().add_sink(logging::console_sink{});

    platform::window_desc wd;
    wd.title = "Catalyst rendering basics";
    platform::window w = platform::create_window(wd);
    if (!w)
    {
        logging::critical<example_log>("Failed to create window");
        return 1;
    }

    rendering::device_desc dd;
    dd.application_name = "catalyst_rendering_basics";
    dd.enable_validation = true;
    rendering::device dev = rendering::create_device(dd);
    if (!dev)
    {
        logging::critical<example_log>("Failed to create rendering device");
        return 1;
    }

    const rendering::device_info info = rendering::get_device_info(dev);
    logging::info<example_log>("Rendering backend: {}", rendering::to_string(info.backend));
    logging::info<example_log>("Adapter: {} ({} MiB device-local)", info.adapter_name,
                               info.dedicated_video_memory_bytes >> 20);

    auto client_extent = [&]() -> rendering::extent2d {
        const auto client = platform::client_rect_px(w);
        return {static_cast<std::uint32_t>(client.size().x()), static_cast<std::uint32_t>(client.size().y())};
    };

    rendering::swapchain_desc sd;
    sd.window = platform::get_native_handle(w);
    sd.extent = client_extent();
    sd.debug_name = "main swapchain";
    rendering::swapchain sc = rendering::create_swapchain(dev, sd);
    if (!sc)
    {
        logging::critical<example_log>("Failed to create swapchain");
        return 1;
    }
    sd = rendering::get_swapchain_desc(sc); // The backend may have adjusted extent, format or image count.
    logging::info<example_log>("Swapchain: {}x{}, {} images", sd.extent.width, sd.extent.height, sd.image_count);

    auto vb = rendering::create_structured_buffer<vertex>(dev, std::size(triangle), rendering::buffer_usage::vertex,
                                                          rendering::memory_access::gpu_only,
                                                          std::span<const vertex>{triangle}, "triangle vertices");
    logging::info<example_log>("Vertex buffer: {} vertices, {} bytes", vb.count(), vb.size_bytes());

    rendering::pipeline pipeline = make_triangle_pipeline(dev, sd.pixel_format);
    rendering::command_list cl = rendering::create_command_list(dev, {.debug_name = "frame"});

    constexpr int frame_count = 120;
    int rendered = 0;
    for (int frame = 0; frame < frame_count; ++frame)
    {
        platform::pump_events();

        rendering::texture back_buffer = rendering::acquire_next_image(sc);
        if (!back_buffer)
        {
            // Window minimised or swapchain out of date: try to match the current client size and skip the frame.
            const rendering::extent2d extent = client_extent();
            if (extent.width != 0 && extent.height != 0 && rendering::resize_swapchain(sc, extent))
                sd = rendering::get_swapchain_desc(sc);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        const float t = static_cast<float>(frame) / frame_count;
        const rendering::color_attachment color{
            .target = back_buffer,
            .load = rendering::load_op::clear,
            .store = rendering::store_op::store,
            .clear = {0.1f, 0.2f + 0.3f * std::sin(t * 6.28318f) * 0.5f, 0.35f, 1.0f},
        };

        rendering::begin_recording(cl);
        rendering::begin_render_pass(cl, {.color_attachments = std::span{&color, 1}, .debug_name = "clear"});
        rendering::set_viewport(cl, {0.0f, 0.0f, static_cast<float>(sd.extent.width),
                                     static_cast<float>(sd.extent.height)});
        rendering::set_scissor(cl, {0, 0, sd.extent.width, sd.extent.height});

        if (pipeline)
        {
            rendering::set_pipeline(cl, pipeline);
            rendering::set_vertex_buffer(cl, 0, vb.handle());
            rendering::draw(cl, static_cast<std::uint32_t>(vb.count()));
        }

        rendering::end_render_pass(cl);
        rendering::end_recording(cl);

        rendering::submit(dev, cl);
        rendering::present(sc);
        ++rendered;

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    rendering::wait_idle(dev);
    rendering::destroy_command_list(cl);
    rendering::destroy_pipeline(pipeline);
    vb.destroy();
    rendering::destroy_swapchain(sc);
    rendering::destroy_device(dev);
    platform::destroy_window(w);

    logging::info<example_log>("Rendered {} of {} frames", rendered, frame_count);
    return 0;
}
