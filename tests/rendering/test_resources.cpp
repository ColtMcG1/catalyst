/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Exercises the public rendering API against whichever backend is compiled in: handle semantics, argument
 * validation, buffer round-trips, pipeline validation, swapchain image cycling and command-list state rules. Backends
 * that execute real GPU work additionally run a compute dispatch and read the result back.
 * @details Shaders are real SPIR-V compiled from tests/rendering/shaders (see test_shaders.hpp) so the same test binary
 * is meaningful on the bookkeeping null backend and on a real Vulkan device.
 */

#include "../test_common.hpp"
#include "test_shaders.hpp"

#include <catalyst/rendering/rendering.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using namespace catalyst::rendering;

namespace
{
    namespace spirv = catalyst::tests::spirv;

    /** Real SPIR-V for each stage; the bookkeeping backend copies it, a real backend builds pipelines from it. */
    std::span<const std::byte> bytecode_for(shader_stage stage) noexcept
    {
        switch (stage)
        {
        case shader_stage::vertex:   return spirv::minimal_vertex_bytes();
        case shader_stage::fragment: return spirv::minimal_fragment_bytes();
        case shader_stage::compute:  return spirv::fill_compute_bytes();
        }
        return {};
    }

    shader make_shader(const device &dev, shader_stage stage)
    {
        shader_desc d;
        d.stage = stage;
        d.bytecode_format = shader_bytecode_format::spirv;
        d.bytecode = bytecode_for(stage);
        return create_shader(dev, d);
    }

    /** True for backends that execute submitted work on a GPU (as opposed to bookkeeping only). */
    bool backend_executes_gpu_work() noexcept
    {
        return backend() == backend_kind::vulkan;
    }

    swapchain make_swapchain(const device &dev, std::uint32_t width, std::uint32_t height, std::uint32_t images = 2)
    {
        swapchain_desc d;
        d.extent = {width, height};
        d.image_count = images;
        return create_swapchain(dev, d);
    }

    void test_handles()
    {
        device none;
        CT_REQUIRE(!none);
        CT_REQUIRE(none.id() == 0);
        CT_REQUIRE(!is_valid(none));

        const device some{42};
        CT_REQUIRE(some);
        CT_REQUIRE(some.id() == 42);
        CT_REQUIRE(some != none);
        CT_REQUIRE(some == device{42});
        CT_REQUIRE(!is_valid(some)); // Non-zero but never created.

        // Type safety: different resource kinds are different types.
        static_assert(!std::is_same_v<device, buffer>);
        static_assert(std::is_trivially_copyable_v<buffer>);
    }

    void test_flags()
    {
        buffer_usage u = buffer_usage::vertex | buffer_usage::transfer_dst;
        CT_REQUIRE(has_flag(u, buffer_usage::vertex));
        CT_REQUIRE(has_flag(u, buffer_usage::transfer_dst));
        CT_REQUIRE(!has_flag(u, buffer_usage::index));
        CT_REQUIRE(has_any(u, buffer_usage::index | buffer_usage::transfer_dst));
        CT_REQUIRE(!has_any(u, buffer_usage::index | buffer_usage::uniform));
        CT_REQUIRE(!has_flag(u, buffer_usage::none));

        u &= ~buffer_usage::transfer_dst;
        CT_REQUIRE(!has_flag(u, buffer_usage::transfer_dst));
        u |= buffer_usage::index;
        CT_REQUIRE(has_flag(u, buffer_usage::vertex | buffer_usage::index));

        CT_REQUIRE(has_flag(color_write_mask::all, color_write_mask::r | color_write_mask::a));
    }

    void test_formats()
    {
        CT_REQUIRE(format_size_bytes(format::unknown) == 0);
        CT_REQUIRE(format_size_bytes(format::rgba8_unorm) == 4);
        CT_REQUIRE(format_size_bytes(format::rgb32_float) == 12);
        CT_REQUIRE(format_size_bytes(format::rgba32_float) == 16);
        CT_REQUIRE(is_depth_format(format::d32_float));
        CT_REQUIRE(!is_depth_format(format::bgra8_unorm_srgb));
        CT_REQUIRE(is_stencil_format(format::d24_unorm_s8_uint));
        CT_REQUIRE(!is_stencil_format(format::d32_float));
        CT_REQUIRE(is_srgb_format(format::bgra8_unorm_srgb));
        CT_REQUIRE(!is_srgb_format(format::bgra8_unorm));
        CT_REQUIRE(index_size_bytes(index_type::uint16) == 2);
        CT_REQUIRE(index_size_bytes(index_type::uint32) == 4);
        CT_REQUIRE(std::string_view{to_string(backend_kind::vulkan)} == "vulkan");
    }

    void test_device()
    {
        device dev = create_device();
        CT_REQUIRE(is_valid(dev));

        const device_info info = get_device_info(dev);
        CT_REQUIRE(info.backend == backend());
        CT_REQUIRE(!std::string_view{info.adapter_name}.empty());
        CT_REQUIRE(std::string_view{module_name()} == to_string(backend()));

        wait_idle(dev);

        destroy_device(dev);
        CT_REQUIRE(!dev);
        CT_REQUIRE(!is_valid(dev));
        destroy_device(dev); // Idempotent on an invalid handle.
    }

    void test_buffers()
    {
        device dev = create_device();

        // Argument validation.
        buffer_desc bad;
        CT_REQUIRE(!create_buffer(dev, bad)); // size 0
        bad.size_bytes = 4;
        const std::array<std::byte, 8> too_much{};
        CT_REQUIRE(!create_buffer(dev, bad, too_much));
        CT_REQUIRE(!create_buffer(device{}, bad));

        // Readback buffer: initial data round-trips.
        const std::array<std::uint32_t, 4> values = {1, 2, 3, 4};
        buffer_desc desc;
        desc.size_bytes = sizeof(values);
        desc.usage = buffer_usage::storage | buffer_usage::transfer_dst;
        desc.access = memory_access::gpu_to_cpu;
        desc.debug_name = "readback";

        buffer b = create_buffer(dev, desc, std::as_bytes(std::span{values}));
        CT_REQUIRE(is_valid(b));
        CT_REQUIRE(buffer_size(b) == sizeof(values));
        const buffer_desc got = get_buffer_desc(b);
        CT_REQUIRE(got.access == memory_access::gpu_to_cpu);
        CT_REQUIRE(has_flag(got.usage, buffer_usage::storage));
        CT_REQUIRE(got.debug_name != nullptr && std::string_view{got.debug_name} == "readback");

        std::array<std::uint32_t, 4> out{};
        CT_REQUIRE(read_buffer(b, 0, std::as_writable_bytes(std::span{out})));
        CT_REQUIRE(out == values);

        std::array<std::uint32_t, 2> tail{};
        CT_REQUIRE(read_buffer(b, 8, std::as_writable_bytes(std::span{tail})));
        CT_REQUIRE(tail[0] == 3 && tail[1] == 4);

        // Out of range and wrong-direction access are rejected.
        CT_REQUIRE(!read_buffer(b, 12, std::as_writable_bytes(std::span{tail})));
        CT_REQUIRE(!write_buffer(b, 0, std::as_bytes(std::span{values}))); // gpu_to_cpu is read-only for the CPU.

        // Upload buffer: writable, not readable.
        desc.access = memory_access::cpu_to_gpu;
        buffer up = create_buffer(dev, desc);
        CT_REQUIRE(is_valid(up));
        CT_REQUIRE(write_buffer(up, 4, std::as_bytes(std::span{tail})));
        CT_REQUIRE(!write_buffer(up, 12, std::as_bytes(std::span{tail})));
        CT_REQUIRE(!read_buffer(up, 0, std::as_writable_bytes(std::span{out})));

        destroy_buffer(up);
        CT_REQUIRE(!up);
        destroy_buffer(b);
        CT_REQUIRE(!is_valid(b));
        CT_REQUIRE(buffer_size(b) == 0);

        destroy_device(dev);
    }

    void test_structured_buffer()
    {
        struct vertex
        {
            float x, y, z;
        };

        device dev = create_device();

        const std::array<vertex, 3> tri = {vertex{0.0f, 0.5f, 0.0f}, vertex{-0.5f, -0.5f, 0.0f},
                                           vertex{0.5f, -0.5f, 0.0f}};

        auto vb = create_structured_buffer<vertex>(dev, tri.size(), buffer_usage::vertex, memory_access::gpu_to_cpu,
                                                   std::span<const vertex>{tri}, "triangle");
        CT_REQUIRE(vb);
        CT_REQUIRE(vb.count() == 3);
        CT_REQUIRE(vb.size_bytes() == 3 * sizeof(vertex));
        CT_REQUIRE(structured_buffer<vertex>::stride_bytes() == sizeof(vertex));
        CT_REQUIRE(get_buffer_desc(vb.handle()).stride_bytes == sizeof(vertex));

        std::array<vertex, 3> back{};
        CT_REQUIRE(vb.read(back));
        CT_REQUIRE(std::memcmp(back.data(), tri.data(), sizeof(tri)) == 0);

        std::array<vertex, 1> one{};
        CT_REQUIRE(vb.read(one, 2));
        CT_REQUIRE(one[0].x == 0.5f);
        CT_REQUIRE(!vb.read(back, 1)); // 3 elements from index 1 overruns.

        // Too much initial data / zero count are rejected.
        CT_REQUIRE(!create_structured_buffer<vertex>(dev, 2, buffer_usage::vertex, memory_access::gpu_only,
                                                     std::span<const vertex>{tri}));
        CT_REQUIRE(!create_structured_buffer<vertex>(dev, 0, buffer_usage::vertex));

        // Partial initial data is fine; writes respect element bounds.
        auto dyn = create_structured_buffer<vertex>(dev, 8, buffer_usage::vertex, memory_access::cpu_to_gpu,
                                                    std::span<const vertex>{tri});
        CT_REQUIRE(dyn && dyn.count() == 8);
        CT_REQUIRE(dyn.write(tri, 5));
        CT_REQUIRE(!dyn.write(tri, 6));

        vb.destroy();
        CT_REQUIRE(!vb);
        CT_REQUIRE(vb.count() == 0);
        dyn.destroy();

        destroy_device(dev);
    }

    void test_shaders()
    {
        device dev = create_device();

        shader_desc empty;
        CT_REQUIRE(!create_shader(dev, empty));

        shader vs = make_shader(dev, shader_stage::vertex);
        CT_REQUIRE(is_valid(vs));
        CT_REQUIRE(get_shader_stage(vs) == shader_stage::vertex);

        const std::span<const std::byte> expected = bytecode_for(shader_stage::vertex);
        const std::span<const std::byte> code = get_bytecode(vs);
        CT_REQUIRE(code.size() == expected.size());
        CT_REQUIRE(std::memcmp(code.data(), expected.data(), code.size()) == 0);
        CT_REQUIRE(code.data() != expected.data()); // The backend owns its own copy.

        // Garbage is rejected by backends that parse the blob; the bookkeeping backend only checks emptiness.
        const std::array<std::byte, 6> garbage = {std::byte{1}, std::byte{2}, std::byte{3},
                                                  std::byte{4}, std::byte{5}, std::byte{6}};
        shader_desc bad;
        bad.stage = shader_stage::vertex;
        bad.bytecode = garbage;
        shader junk = create_shader(dev, bad);
        CT_REQUIRE(backend_executes_gpu_work() ? !junk : is_valid(junk));
        destroy_shader(junk);

        CT_REQUIRE(get_bytecode(shader{}).empty());

        destroy_shader(vs);
        CT_REQUIRE(!vs);
        CT_REQUIRE(get_bytecode(vs).empty());

        destroy_device(dev);
    }

    void test_textures_and_samplers()
    {
        device dev = create_device();

        texture_desc bad;
        CT_REQUIRE(!create_texture(dev, bad)); // zero extent
        bad.extent = {4, 4, 1};
        bad.pixel_format = format::unknown;
        CT_REQUIRE(!create_texture(dev, bad));

        texture_desc desc;
        desc.extent = {4, 4, 1};
        desc.pixel_format = format::rgba8_unorm;
        desc.usage = texture_usage::sampled | texture_usage::transfer_dst;
        desc.debug_name = "checker";

        const std::vector<std::byte> pixels(4 * 4 * 4);
        CT_REQUIRE(!create_texture(dev, desc, std::span{pixels}.first(8))); // wrong size
        texture t = create_texture(dev, desc, pixels);
        CT_REQUIRE(is_valid(t));

        const texture_desc got = get_texture_desc(t);
        CT_REQUIRE((got.extent == extent3d{4, 4, 1}));
        CT_REQUIRE(got.pixel_format == format::rgba8_unorm);
        CT_REQUIRE(has_flag(got.usage, texture_usage::sampled));
        CT_REQUIRE(got.debug_name != nullptr && std::string_view{got.debug_name} == "checker");

        sampler_desc sd;
        sd.max_anisotropy = 0.5f;
        CT_REQUIRE(!create_sampler(dev, sd));
        sampler s = create_sampler(dev);
        CT_REQUIRE(is_valid(s));

        destroy_sampler(s);
        CT_REQUIRE(!s);
        destroy_texture(t);
        CT_REQUIRE(!is_valid(t));

        destroy_device(dev);
    }

    void test_pipelines()
    {
        device dev = create_device();

        shader vs = make_shader(dev, shader_stage::vertex);
        shader fs = make_shader(dev, shader_stage::fragment);
        shader cs = make_shader(dev, shader_stage::compute);

        const std::array<vertex_binding, 1> bindings = {vertex_binding{0, 24, vertex_input_rate::per_vertex}};
        const std::array<vertex_attribute, 2> attributes = {
            vertex_attribute{0, 0, format::rgb32_float, 0},
            vertex_attribute{1, 0, format::rgb32_float, 12},
        };
        const std::array<format, 1> color_formats = {format::bgra8_unorm_srgb};
        const std::array<blend_state, 1> blends = {blend_alpha()};

        graphics_pipeline_desc gp;
        gp.vertex_shader = vs;
        gp.fragment_shader = fs;
        gp.vertex_input = {bindings, attributes};
        gp.color_formats = color_formats;
        gp.color_blend = blends;
        gp.depth_format = format::d32_float;
        gp.depth_stencil.depth_test = true;
        gp.depth_stencil.depth_write = true;

        pipeline p = create_graphics_pipeline(dev, gp);
        CT_REQUIRE(is_valid(p));
        CT_REQUIRE(get_pipeline_type(p) == pipeline_type::graphics);

        // Depth-only pipeline (no fragment shader) is allowed.
        graphics_pipeline_desc depth_only = gp;
        depth_only.fragment_shader = shader{};
        depth_only.color_formats = {};
        depth_only.color_blend = {};
        pipeline dp = create_graphics_pipeline(dev, depth_only);
        CT_REQUIRE(is_valid(dp));

        // Validation failures.
        graphics_pipeline_desc wrong_stage = gp;
        wrong_stage.fragment_shader = vs;
        CT_REQUIRE(!create_graphics_pipeline(dev, wrong_stage));

        graphics_pipeline_desc no_vs = gp;
        no_vs.vertex_shader = fs;
        CT_REQUIRE(!create_graphics_pipeline(dev, no_vs));

        const std::array<vertex_attribute, 1> orphan = {vertex_attribute{0, 3, format::rgb32_float, 0}};
        graphics_pipeline_desc bad_layout = gp;
        bad_layout.vertex_input = {bindings, orphan};
        CT_REQUIRE(!create_graphics_pipeline(dev, bad_layout));

        const std::array<blend_state, 2> too_many_blends = {blend_opaque(), blend_opaque()};
        graphics_pipeline_desc blend_mismatch = gp;
        blend_mismatch.color_blend = too_many_blends;
        CT_REQUIRE(!create_graphics_pipeline(dev, blend_mismatch));

        graphics_pipeline_desc bad_depth = gp;
        bad_depth.depth_format = format::rgba8_unorm;
        CT_REQUIRE(!create_graphics_pipeline(dev, bad_depth));

        const std::array<format, 1> depth_as_color = {format::d32_float};
        graphics_pipeline_desc bad_color = gp;
        bad_color.color_formats = depth_as_color;
        bad_color.color_blend = {};
        CT_REQUIRE(!create_graphics_pipeline(dev, bad_color));

        compute_pipeline_desc cp;
        cp.compute_shader = vs;
        CT_REQUIRE(!create_compute_pipeline(dev, cp));
        cp.compute_shader = cs;
        pipeline c = create_compute_pipeline(dev, cp);
        CT_REQUIRE(is_valid(c));
        CT_REQUIRE(get_pipeline_type(c) == pipeline_type::compute);

        // Pipelines outlive the shaders they were built from.
        destroy_shader(vs);
        destroy_shader(fs);
        destroy_shader(cs);
        CT_REQUIRE(is_valid(p));
        CT_REQUIRE(is_valid(c));

        destroy_pipeline(p);
        destroy_pipeline(dp);
        destroy_pipeline(c);
        CT_REQUIRE(!p && !is_valid(c));

        destroy_device(dev);
    }

    void test_swapchain()
    {
        device dev = create_device();

        CT_REQUIRE(!make_swapchain(dev, 0, 0));
        CT_REQUIRE(!make_swapchain(dev, 8, 8, 0));

        swapchain sc = make_swapchain(dev, 640, 480, 2);
        CT_REQUIRE(is_valid(sc));
        const swapchain_desc sd = get_swapchain_desc(sc);
        CT_REQUIRE((sd.extent == extent2d{640, 480}));
        CT_REQUIRE(sd.image_count == 2);

        CT_REQUIRE(!present(sc)); // Nothing acquired yet.

        texture first = acquire_next_image(sc);
        CT_REQUIRE(is_valid(first));
        const texture_desc td = get_texture_desc(first);
        CT_REQUIRE((td.extent == extent3d{640, 480, 1}));
        CT_REQUIRE(td.pixel_format == sd.pixel_format);
        CT_REQUIRE(has_flag(td.usage, texture_usage::render_target));
        CT_REQUIRE(present(sc));

        texture second = acquire_next_image(sc);
        CT_REQUIRE(is_valid(second));
        CT_REQUIRE(second != first);
        CT_REQUIRE(present(sc));

        CT_REQUIRE(acquire_next_image(sc) == first); // Two images cycle.
        CT_REQUIRE(present(sc));

        // Swapchain images cannot be destroyed by the user.
        texture image = first;
        destroy_texture(image);
        CT_REQUIRE(is_valid(first));

        // Resizing recreates the images.
        CT_REQUIRE(!resize_swapchain(sc, {0, 10}));
        CT_REQUIRE(resize_swapchain(sc, {320, 240}));
        CT_REQUIRE(!is_valid(first));
        CT_REQUIRE((get_swapchain_desc(sc).extent == extent2d{320, 240}));
        texture resized = acquire_next_image(sc);
        CT_REQUIRE((get_texture_desc(resized).extent == extent3d{320, 240, 1}));

        destroy_swapchain(sc);
        CT_REQUIRE(!sc);
        CT_REQUIRE(!is_valid(resized));

        destroy_device(dev);
    }

    void test_command_lists()
    {
        device dev = create_device();
        swapchain sc = make_swapchain(dev, 64, 64);
        shader vs = make_shader(dev, shader_stage::vertex);
        shader fs = make_shader(dev, shader_stage::fragment);

        const std::array<format, 1> color_formats = {get_swapchain_desc(sc).pixel_format};
        graphics_pipeline_desc gp;
        gp.vertex_shader = vs;
        gp.fragment_shader = fs;
        gp.color_formats = color_formats;
        pipeline p = create_graphics_pipeline(dev, gp);
        CT_REQUIRE(is_valid(p));

        const queue graphics = get_queue(dev);
        command_list cl = create_command_list(dev);
        CT_REQUIRE(is_valid(cl));
        CT_REQUIRE(!is_recording(cl));
        CT_REQUIRE(!end_recording(cl));
        CT_REQUIRE(!submit(graphics, cl)); // Never recorded.

        CT_REQUIRE(begin_recording(cl));
        CT_REQUIRE(is_recording(cl));
        CT_REQUIRE(!begin_recording(cl)); // Already recording.

        texture back_buffer = acquire_next_image(sc);
        const color_attachment color{back_buffer, load_op::clear, store_op::store, {0.1f, 0.2f, 0.3f, 1.0f}};
        render_pass_desc pass;
        pass.color_attachments = std::span{&color, 1};

        begin_render_pass(cl, pass);
        set_pipeline(cl, p);
        set_viewport(cl, {0.0f, 0.0f, 64.0f, 64.0f});
        set_scissor(cl, {0, 0, 64, 64});
        draw(cl, 3);
        CT_REQUIRE(!end_recording(cl)); // Render pass still open.
        end_render_pass(cl);
        CT_REQUIRE(end_recording(cl));
        CT_REQUIRE(!is_recording(cl));

        const auto first = submit(graphics, cl);
        CT_REQUIRE(first);
        const auto second = submit(graphics, cl); // Re-submittable until re-recorded.
        CT_REQUIRE(second);

        // Points from one queue are ordered, and a later submission is strictly later.
        CT_REQUIRE(same_timeline(*first, *second));
        CT_REQUIRE(*first < *second);
        CT_REQUIRE(latest(*first, *second) == *second);
        CT_REQUIRE(first->queue() == queue_kind::graphics);
        CT_REQUIRE(*second <= last_submitted(graphics));

        CT_REQUIRE(present(sc));

        // Multi-list submission rejects any invalid or unfinished list.
        command_list other = create_command_list(dev);
        const std::array<command_list, 2> both = {cl, other};
        CT_REQUIRE(!submit(graphics, {.lists = both}));
        CT_REQUIRE(begin_recording(other));
        CT_REQUIRE(!submit(graphics, {.lists = both})); // `other` still recording.
        CT_REQUIRE(end_recording(other));
        CT_REQUIRE(submit(graphics, {.lists = both}));

        // Commands recorded outside begin/end are ignored, not fatal.
        draw(cl, 3);
        begin_render_pass(cl, pass);
        end_render_pass(cl);

        // Buffer copies execute at submit time.
        const std::array<std::uint32_t, 4> values = {10, 20, 30, 40};
        buffer_desc src_desc;
        src_desc.size_bytes = sizeof(values);
        src_desc.usage = buffer_usage::transfer_src;
        src_desc.access = memory_access::cpu_to_gpu;
        buffer src = create_buffer(dev, src_desc, std::as_bytes(std::span{values}));

        buffer_desc dst_desc = src_desc;
        dst_desc.usage = buffer_usage::transfer_dst;
        dst_desc.access = memory_access::gpu_to_cpu;
        buffer dst = create_buffer(dev, dst_desc);

        CT_REQUIRE(begin_recording(cl));
        copy_buffer(cl, src, 8, dst, 0, 8);
        CT_REQUIRE(end_recording(cl));

        std::array<std::uint32_t, 2> out{};
        CT_REQUIRE(read_buffer(dst, 0, std::as_writable_bytes(std::span{out})));
        CT_REQUIRE(out[0] == 0 && out[1] == 0); // Not copied until submitted.
        CT_REQUIRE(submit(graphics, cl));
        CT_REQUIRE(read_buffer(dst, 0, std::as_writable_bytes(std::span{out})));
        CT_REQUIRE(out[0] == 30 && out[1] == 40);

        destroy_command_list(other);
        destroy_command_list(cl);
        CT_REQUIRE(!cl);
        CT_REQUIRE(!submit(graphics, cl));

        destroy_buffer(src);
        destroy_buffer(dst);
        destroy_pipeline(p);
        destroy_shader(vs);
        destroy_shader(fs);
        destroy_swapchain(sc);
        destroy_device(dev);
    }

    /**
     * End-to-end GPU round trip: a compute shader fills a storage buffer from push constants, the result is copied into
     * a readback buffer and compared on the CPU. Only meaningful on backends that execute work.
     */
    void test_gpu_compute()
    {
        if (!backend_executes_gpu_work())
            return;

        constexpr std::uint32_t element_count = 256;
        constexpr std::uint32_t multiplier = 3;

        device dev = create_device();
        CT_REQUIRE(is_valid(dev));

        auto storage = create_structured_buffer<std::uint32_t>(dev, element_count,
                                                               buffer_usage::storage | buffer_usage::transfer_src);
        auto readback = create_structured_buffer<std::uint32_t>(dev, element_count, buffer_usage::transfer_dst,
                                                                memory_access::gpu_to_cpu);
        CT_REQUIRE(storage && readback);

        shader cs = make_shader(dev, shader_stage::compute);
        pipeline p = create_compute_pipeline(dev, {cs, "fill"});
        CT_REQUIRE(is_valid(p));

        command_list cl = create_command_list(dev, {queue_kind::compute, "fill"});
        CT_REQUIRE(begin_recording(cl));
        set_pipeline(cl, p);
        set_storage_buffer(cl, 0, storage.handle());
        push_constants(cl, 0, std::as_bytes(std::span{&multiplier, 1}));
        dispatch(cl, element_count / 64);
        copy_buffer(cl, storage.handle(), 0, readback.handle(), 0, storage.size_bytes());
        CT_REQUIRE(end_recording(cl));
        CT_REQUIRE(submit(get_queue(dev, queue_kind::compute), cl));

        std::array<std::uint32_t, element_count> out{};
        CT_REQUIRE(readback.read(out));
        for (std::uint32_t i = 0; i < element_count; ++i)
            CT_REQUIRE(out[i] == i * multiplier);

        // Re-submitting the same list with a different push constant is not possible (constants are recorded), but
        // re-recording and re-running it is; the previous submission must be retired transparently.
        constexpr std::uint32_t other = 7;
        CT_REQUIRE(begin_recording(cl));
        set_pipeline(cl, p);
        set_storage_buffer(cl, 0, storage.handle());
        push_constants(cl, 0, std::as_bytes(std::span{&other, 1}));
        dispatch(cl, element_count / 64);
        copy_buffer(cl, storage.handle(), 0, readback.handle(), 0, storage.size_bytes());
        CT_REQUIRE(end_recording(cl));
        CT_REQUIRE(submit(get_queue(dev, queue_kind::compute), cl));
        CT_REQUIRE(readback.read(out));
        CT_REQUIRE(out[1] == other && out[255] == 255 * other);

        // A compute list belongs to the compute queue and is refused by every other, because its
        // command buffer came from the compute family's pool. The old `queue_type` made no such
        // check, since every kind was submitted to the same queue regardless.
        CT_REQUIRE(!submit(get_queue(dev, queue_kind::copy), cl));
        CT_REQUIRE(!submit(get_queue(dev, queue_kind::graphics), cl));

        destroy_command_list(cl);
        destroy_pipeline(p);
        destroy_shader(cs);
        storage.destroy();
        readback.destroy();
        destroy_device(dev);
    }

    void test_destroy_device_cascades()
    {
        device dev = create_device();

        buffer_desc bd;
        bd.size_bytes = 16;
        bd.usage = buffer_usage::uniform;
        buffer b = create_buffer(dev, bd);
        shader s = make_shader(dev, shader_stage::compute);
        texture_desc td;
        td.extent = {1, 1, 1};
        texture t = create_texture(dev, td);
        sampler sm = create_sampler(dev);
        pipeline p = create_compute_pipeline(dev, {s});
        swapchain sc = make_swapchain(dev, 2, 2);
        command_list cl = create_command_list(dev);

        CT_REQUIRE(is_valid(b) && is_valid(s) && is_valid(t) && is_valid(sm) && is_valid(p) && is_valid(sc) &&
                   is_valid(cl));

        destroy_device(dev);

        CT_REQUIRE(!is_valid(b));
        CT_REQUIRE(!is_valid(s));
        CT_REQUIRE(!is_valid(t));
        CT_REQUIRE(!is_valid(sm));
        CT_REQUIRE(!is_valid(p));
        CT_REQUIRE(!is_valid(sc));
        CT_REQUIRE(!is_valid(cl));

        // Resources of one device are independent of another.
        device a = create_device();
        device other = create_device();
        buffer ab = create_buffer(a, bd);
        destroy_device(other);
        CT_REQUIRE(is_valid(ab));
        destroy_device(a);
    }
} // namespace

int main()
{
    test_handles();
    test_flags();
    test_formats();
    test_device();
    test_buffers();
    test_structured_buffer();
    test_shaders();
    test_textures_and_samplers();
    test_pipelines();
    test_swapchain();
    test_command_lists();
    test_gpu_compute();
    test_destroy_device_cascades();

    std::printf("catalyst.rendering.resources: OK (backend: %s)\n", module_name());
    return 0;
}
