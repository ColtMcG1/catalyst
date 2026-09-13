/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief The `pipeline` suite: how long shader modules and pipeline state objects take to build.
 * @details Pipelines are immutable, so every material permutation, every render-target format change and every shader
 * edit costs a full construction. This suite measures that cost four ways: a shader module on its own, a pipeline built
 * from modules that already exist, a pipeline built from SPIR-V end to end (what a shader hot-reload really costs), and
 * a pipeline whose state differs every iteration, which defeats any identical-pipeline caching the driver does. It also
 * builds the shared quad pipelines used by the `quads` suite.
 */

#include "render_bench.hpp"
#include "shaders.hpp"

#include <array>
#include <cstddef>

namespace catalyst::bench::render
{
    using namespace catalyst::rendering;

    namespace
    {
        shader make_shader(const device &dev, shader_stage stage, std::span<const std::byte> bytecode,
                           const char *name)
        {
            shader_desc desc;
            desc.stage = stage;
            desc.bytecode_format = shader_bytecode_format::spirv;
            desc.bytecode = bytecode;
            desc.debug_name = name;
            return create_shader(dev, desc);
        }

        /** Vertex layout of shaders/quad.vert: interleaved clip-space position and colour. */
        constexpr std::array<vertex_binding, 1> quad_bindings = {
            vertex_binding{0, sizeof(quad_vertex), vertex_input_rate::per_vertex}};
        constexpr std::array<vertex_attribute, 2> quad_attributes = {
            vertex_attribute{0, 0, format::rg32_float, offsetof(quad_vertex, x)},
            vertex_attribute{1, 0, format::rgba32_float, offsetof(quad_vertex, r)},
        };

        /** Vertex layout of shaders/quad_instanced.vert: a unit quad plus a per-instance placement stream. */
        constexpr std::array<vertex_binding, 2> instanced_bindings = {
            vertex_binding{0, sizeof(quad_corner), vertex_input_rate::per_vertex},
            vertex_binding{1, sizeof(quad_instance), vertex_input_rate::per_instance},
        };
        constexpr std::array<vertex_attribute, 4> instanced_attributes = {
            vertex_attribute{0, 0, format::rg32_float, offsetof(quad_corner, x)},
            vertex_attribute{1, 1, format::rg32_float, offsetof(quad_instance, center_x)},
            vertex_attribute{2, 1, format::rg32_float, offsetof(quad_instance, half_w)},
            vertex_attribute{3, 1, format::rgba32_float, offsetof(quad_instance, r)},
        };
    } // namespace

    bool backend_consumes_spirv() noexcept
    {
        return native_bytecode_format(backend()) == shader_bytecode_format::spirv;
    }

    pipeline create_quad_pipeline(const device &dev, format color_format)
    {
        if (!backend_consumes_spirv())
            return {};

        shader vs = make_shader(dev, shader_stage::vertex, spirv::quad_vertex_bytes(), "quad.vert");
        shader fs = make_shader(dev, shader_stage::fragment, spirv::quad_fragment_bytes(), "quad.frag");

        const std::array<format, 1> color_formats = {color_format};
        graphics_pipeline_desc desc;
        desc.vertex_shader = vs;
        desc.fragment_shader = fs;
        desc.vertex_input = {quad_bindings, quad_attributes};
        desc.color_formats = color_formats;
        desc.rasterizer.cull = cull_mode::none;
        desc.debug_name = "quad";
        pipeline p = create_graphics_pipeline(dev, desc);

        destroy_shader(vs);
        destroy_shader(fs);
        return p;
    }

    pipeline create_instanced_quad_pipeline(const device &dev, format color_format)
    {
        if (!backend_consumes_spirv())
            return {};

        shader vs = make_shader(dev, shader_stage::vertex, spirv::quad_instanced_vertex_bytes(), "quad_instanced.vert");
        shader fs = make_shader(dev, shader_stage::fragment, spirv::quad_fragment_bytes(), "quad.frag");

        const std::array<format, 1> color_formats = {color_format};
        graphics_pipeline_desc desc;
        desc.vertex_shader = vs;
        desc.fragment_shader = fs;
        desc.vertex_input = {instanced_bindings, instanced_attributes};
        desc.color_formats = color_formats;
        desc.rasterizer.cull = cull_mode::none;
        desc.debug_name = "quad instanced";
        pipeline p = create_graphics_pipeline(dev, desc);

        destroy_shader(vs);
        destroy_shader(fs);
        return p;
    }

    void run_pipeline_suite(context &ctx, const options &opt)
    {
        print_suite("pipeline construction");

        if (!backend_consumes_spirv())
        {
            print_note("skipped", "the compiled-in backend does not consume SPIR-V");
            return;
        }

        const device &dev = ctx.device();
        const format color = ctx.color_format();
        const std::size_t iterations = opt.pipeline_iterations;

        print_lifecycle("shader module (vertex SPIR-V)",
                        measure_lifecycle(
                            iterations,
                            [&] { return make_shader(dev, shader_stage::vertex, spirv::quad_vertex_bytes(), nullptr); },
                            [](shader &s) { destroy_shader(s); }));

        print_lifecycle(
            "shader module (fragment)",
            measure_lifecycle(
                iterations,
                [&] { return make_shader(dev, shader_stage::fragment, spirv::quad_fragment_bytes(), nullptr); },
                [](shader &s) { destroy_shader(s); }));

        // Modules the pipeline measurements below reuse, so they time pipeline construction alone.
        shader vs = make_shader(dev, shader_stage::vertex, spirv::quad_vertex_bytes(), "quad.vert");
        shader fs = make_shader(dev, shader_stage::fragment, spirv::quad_fragment_bytes(), "quad.frag");
        shader instanced_vs =
            make_shader(dev, shader_stage::vertex, spirv::quad_instanced_vertex_bytes(), "quad_instanced.vert");
        shader cs = make_shader(dev, shader_stage::compute, spirv::fill_compute_bytes(), "fill.comp");

        const std::array<format, 1> color_formats = {color};

        const auto base_desc = [&] {
            graphics_pipeline_desc desc;
            desc.vertex_shader = vs;
            desc.fragment_shader = fs;
            desc.vertex_input = {quad_bindings, quad_attributes};
            desc.color_formats = color_formats;
            desc.rasterizer.cull = cull_mode::none;
            return desc;
        };

        print_lifecycle("graphics pipeline (1 binding, 2 attributes, opaque)",
                        measure_lifecycle(
                            iterations, [&] { return create_graphics_pipeline(dev, base_desc()); },
                            [](pipeline &p) { destroy_pipeline(p); }));

        // Heavier state: two vertex streams, four attributes, alpha blending and a depth test.
        const std::array<blend_state, 1> alpha_blend = {blend_alpha()};
        print_lifecycle("graphics pipeline (2 bindings, 4 attributes, alpha blend + depth)",
                        measure_lifecycle(
                            iterations,
                            [&] {
                                graphics_pipeline_desc desc;
                                desc.vertex_shader = instanced_vs;
                                desc.fragment_shader = fs;
                                desc.vertex_input = {instanced_bindings, instanced_attributes};
                                desc.color_formats = color_formats;
                                desc.color_blend = alpha_blend;
                                desc.depth_format = format::d32_float;
                                desc.depth_stencil.depth_test = true;
                                desc.depth_stencil.depth_write = true;
                                desc.rasterizer.cull = cull_mode::back;
                                return create_graphics_pipeline(dev, desc);
                            },
                            [](pipeline &p) { destroy_pipeline(p); }));

        // Distinct state every iteration, so nothing the driver may keep for identical pipelines can help.
        std::size_t permutation = 0;
        print_lifecycle("graphics pipeline (distinct state per iteration)",
                        measure_lifecycle(
                            iterations,
                            [&] {
                                graphics_pipeline_desc desc = base_desc();
                                desc.rasterizer.cull =
                                    std::array{cull_mode::none, cull_mode::front, cull_mode::back}[permutation % 3];
                                desc.rasterizer.fill = permutation % 2 == 0 ? fill_mode::solid : fill_mode::wireframe;
                                desc.topology = permutation % 4 == 0 ? primitive_topology::triangle_strip
                                                                     : primitive_topology::triangle_list;
                                desc.rasterizer.depth_bias = static_cast<float>(permutation % 7);
                                ++permutation;
                                return create_graphics_pipeline(dev, desc);
                            },
                            [](pipeline &p) { destroy_pipeline(p); }));

        // The full cost of a shader reload: modules rebuilt from bytecode, then the pipeline, then the modules dropped.
        print_lifecycle("graphics pipeline rebuilt from SPIR-V (shader reload)",
                        measure_lifecycle(
                            iterations,
                            [&] {
                                shader reload_vs =
                                    make_shader(dev, shader_stage::vertex, spirv::quad_vertex_bytes(), nullptr);
                                shader reload_fs =
                                    make_shader(dev, shader_stage::fragment, spirv::quad_fragment_bytes(), nullptr);
                                graphics_pipeline_desc desc = base_desc();
                                desc.vertex_shader = reload_vs;
                                desc.fragment_shader = reload_fs;
                                pipeline p = create_graphics_pipeline(dev, desc);
                                destroy_shader(reload_vs);
                                destroy_shader(reload_fs);
                                return p;
                            },
                            [](pipeline &p) { destroy_pipeline(p); }));

        print_lifecycle("compute pipeline",
                        measure_lifecycle(
                            iterations,
                            [&] {
                                compute_pipeline_desc desc;
                                desc.compute_shader = cs;
                                return create_compute_pipeline(dev, desc);
                            },
                            [](pipeline &p) { destroy_pipeline(p); }));

        destroy_shader(vs);
        destroy_shader(fs);
        destroy_shader(instanced_vs);
        destroy_shader(cs);
    }

} // namespace catalyst::bench::render
