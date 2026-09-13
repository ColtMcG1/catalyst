/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief The `quads` suite: how many quads per second reach the screen, and how much that depends on the way they are
 * submitted.
 * @details Every variant draws the same picture - `count` quads on a grid that fills the render target - so the four
 * numbers are directly comparable:
 *   - *batched*: one draw of a pre-built vertex buffer holding six vertices per quad. The cheapest thing the API can do.
 *   - *instanced*: one `draw_indexed` of a four-vertex unit quad with one instance per quad, so the vertex buffer holds
 *     32 bytes per quad instead of 144.
 *   - *dynamic*: the instanced variant with the instance buffer rewritten from the CPU every frame, which is what a
 *     sprite or UI renderer actually does.
 *   - *per-draw*: one `push_constants` + `draw` call per quad, i.e. the naive object-at-a-time loop. This is the
 *     variant that measures draw-call overhead, so it is capped by `--max-draw-calls`.
 * The quads are sized to 90% of their grid cell, so the total covered area stays roughly constant as `count` grows and
 * the differences between counts are per-quad cost rather than fill rate.
 */

#include "render_bench.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace catalyst::bench::render
{
    using namespace catalyst::rendering;

    namespace
    {
        /** Placement of `count` quads on a grid that covers clip space, each filling 90% of its cell. */
        struct grid
        {
            std::size_t columns = 1;
            std::size_t rows = 1;
            float half_w = 1.0f;
            float half_h = 1.0f;

            explicit grid(std::size_t count)
            {
                columns = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(std::max<std::size_t>(count, 1)))));
                rows = (std::max<std::size_t>(count, 1) + columns - 1) / columns;
                half_w = 0.9f / static_cast<float>(columns);
                half_h = 0.9f / static_cast<float>(rows);
            }

            /** Centre of quad `index` in clip space. */
            [[nodiscard]] std::pair<float, float> center(std::size_t index) const noexcept
            {
                const std::size_t column = index % columns;
                const std::size_t row = index / columns;
                return {-1.0f + (2.0f * static_cast<float>(column) + 1.0f) / static_cast<float>(columns),
                        -1.0f + (2.0f * static_cast<float>(row) + 1.0f) / static_cast<float>(rows)};
            }
        };

        /** A cheap spread of colours so the picture is readable while it runs. */
        void color_for(std::size_t index, float &r, float &g, float &b) noexcept
        {
            r = 0.25f + 0.75f * static_cast<float>((index * 37u) % 100u) / 100.0f;
            g = 0.25f + 0.75f * static_cast<float>((index * 61u) % 100u) / 100.0f;
            b = 0.25f + 0.75f * static_cast<float>((index * 89u) % 100u) / 100.0f;
        }

        /** Six vertices per quad (two triangles), laid out on `layout`. */
        std::vector<quad_vertex> build_quad_vertices(std::size_t count)
        {
            const grid layout(count);
            std::vector<quad_vertex> vertices;
            vertices.reserve(count * 6);

            for (std::size_t i = 0; i < count; ++i)
            {
                const auto [cx, cy] = layout.center(i);
                float r = 1.0f, g = 1.0f, b = 1.0f;
                color_for(i, r, g, b);

                const float x0 = cx - layout.half_w;
                const float x1 = cx + layout.half_w;
                const float y0 = cy - layout.half_h;
                const float y1 = cy + layout.half_h;

                const quad_vertex top_left{x0, y1, r, g, b, 1.0f};
                const quad_vertex top_right{x1, y1, r, g, b, 1.0f};
                const quad_vertex bottom_left{x0, y0, r, g, b, 1.0f};
                const quad_vertex bottom_right{x1, y0, r, g, b, 1.0f};

                vertices.insert(vertices.end(), {top_left, bottom_left, bottom_right, top_left, bottom_right, top_right});
            }
            return vertices;
        }

        /**
         * @brief Fills `out` with one instance per quad. `phase` shifts every quad slightly, so rewriting the buffer
         * every frame is not a no-op. Reuses the storage of `out` so a per-frame refill does not allocate.
         */
        void fill_quad_instances(std::vector<quad_instance> &out, std::size_t count, float phase = 0.0f)
        {
            const grid layout(count);
            out.resize(count);

            for (std::size_t i = 0; i < count; ++i)
            {
                const auto [cx, cy] = layout.center(i);
                float r = 1.0f, g = 1.0f, b = 1.0f;
                color_for(i, r, g, b);
                const float jitter = phase * layout.half_w * 0.1f;
                out[i] = {cx + jitter, cy, layout.half_w, layout.half_h, r, g, b, 1.0f};
            }
        }

        std::vector<quad_instance> build_quad_instances(std::size_t count, float phase = 0.0f)
        {
            std::vector<quad_instance> instances;
            fill_quad_instances(instances, count, phase);
            return instances;
        }

        /** The unit quad the instanced and per-draw variants expand: corners in [-1, 1] and the two triangles' indices. */
        constexpr quad_corner unit_corners[] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
        constexpr std::uint16_t unit_indices[] = {0, 1, 2, 0, 2, 3};
        constexpr quad_vertex unit_quad[] = {
            {-1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, {-1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            {1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            {1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        };

        /** Opens the pass every variant draws into: clear, full-target viewport and scissor, pipeline bound. */
        void begin_quad_pass(const command_list &cl, const texture &target, extent2d extent, const pipeline &p)
        {
            const color_attachment color{
                .target = target,
                .load = load_op::clear,
                .store = store_op::store,
                .clear = {0.05f, 0.07f, 0.10f, 1.0f},
            };
            begin_render_pass(cl, {.color_attachments = std::span{&color, 1}, .debug_name = "quads"});
            set_viewport(cl, {0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height)});
            set_scissor(cl, {0, 0, extent.width, extent.height});
            set_pipeline(cl, p);
        }

        void report_quads(std::string_view name, const frame_report &report, std::size_t quads_per_frame,
                          std::size_t draws_per_frame)
        {
            print_frames(name, report);
            if (report.frames == 0)
                return;

            const double seconds = report.wall_ms / 1'000.0;
            if (seconds <= 0.0)
                return;
            const double frames = static_cast<double>(report.frames);
            print_value("quads", static_cast<double>(quads_per_frame) * frames / seconds, "quads/s");
            print_value("draws", static_cast<double>(draws_per_frame) * frames / seconds, "draw calls/s");
            if (draws_per_frame > 1)
            {
                const double per_draw_us = report.record.mean_ms * 1'000.0 / static_cast<double>(draws_per_frame);
                print_value("per draw", per_draw_us, "us of CPU recording");
            }
        }
    } // namespace

    void run_quad_suite(context &ctx, const options &opt)
    {
        print_suite("quad throughput");

        if (!backend_consumes_spirv())
        {
            print_note("skipped", "the compiled-in backend does not consume SPIR-V");
            return;
        }

        const device &dev = ctx.device();
        const extent2d extent = ctx.extent();

        pipeline batched_pipeline = create_quad_pipeline(dev, ctx.color_format());
        pipeline instanced_pipeline = create_instanced_quad_pipeline(dev, ctx.color_format());
        if (!batched_pipeline || !instanced_pipeline)
        {
            print_note("skipped", "the quad pipelines could not be built");
            destroy_pipeline(batched_pipeline);
            destroy_pipeline(instanced_pipeline);
            return;
        }

        // Geometry shared by every count: the unit quad the instanced and per-draw variants expand.
        auto corners = create_structured_buffer<quad_corner>(dev, std::size(unit_corners), buffer_usage::vertex,
                                                             memory_access::gpu_only,
                                                             std::span<const quad_corner>{unit_corners}, "unit corners");
        auto indices = create_structured_buffer<std::uint16_t>(dev, std::size(unit_indices), buffer_usage::index,
                                                               memory_access::gpu_only,
                                                               std::span<const std::uint16_t>{unit_indices},
                                                               "unit indices");
        auto unit = create_structured_buffer<quad_vertex>(dev, std::size(unit_quad), buffer_usage::vertex,
                                                          memory_access::gpu_only,
                                                          std::span<const quad_vertex>{unit_quad}, "unit quad");

        for (const std::size_t count : opt.quad_counts)
        {
            const std::string suffix = " (" + std::to_string(count) + " quads)";

            // --- one draw call, six vertices per quad ---------------------------------------------------------
            {
                const std::vector<quad_vertex> vertices = build_quad_vertices(count);
                auto vb = create_structured_buffer<quad_vertex>(dev, vertices.size(), buffer_usage::vertex,
                                                                memory_access::gpu_only,
                                                                std::span<const quad_vertex>{vertices}, "batched quads");
                if (vb)
                {
                    const auto vertex_count = static_cast<std::uint32_t>(vertices.size());
                    const frame_report report =
                        run_frames(ctx, opt, [&](const command_list &cl, const texture &target) {
                            begin_quad_pass(cl, target, extent, batched_pipeline);
                            const quad_transform identity{};
                            push_constants(cl, 0, std::as_bytes(std::span{&identity, 1}));
                            set_vertex_buffer(cl, 0, vb.handle());
                            draw(cl, vertex_count);
                            end_render_pass(cl);
                        });
                    report_quads("batched, one draw call" + suffix, report, count, 1);
                }
                vb.destroy();
            }

            // --- one instanced draw call ---------------------------------------------------------------------
            {
                const std::vector<quad_instance> instances = build_quad_instances(count);
                auto ib = create_structured_buffer<quad_instance>(dev, instances.size(), buffer_usage::vertex,
                                                                  memory_access::gpu_only,
                                                                  std::span<const quad_instance>{instances},
                                                                  "quad instances");
                if (ib)
                {
                    const auto instance_count = static_cast<std::uint32_t>(instances.size());
                    const frame_report report =
                        run_frames(ctx, opt, [&](const command_list &cl, const texture &target) {
                            begin_quad_pass(cl, target, extent, instanced_pipeline);
                            set_vertex_buffer(cl, 0, corners.handle());
                            set_vertex_buffer(cl, 1, ib.handle());
                            set_index_buffer(cl, indices.handle(), index_type::uint16);
                            draw_indexed(cl, static_cast<std::uint32_t>(std::size(unit_indices)), instance_count);
                            end_render_pass(cl);
                        });
                    report_quads("instanced, one draw call" + suffix, report, count, 1);
                }
                ib.destroy();
            }

            // --- instanced with the instance buffer rewritten every frame ------------------------------------
            {
                std::vector<quad_instance> instances = build_quad_instances(count);
                auto ib = create_structured_buffer<quad_instance>(dev, instances.size(), buffer_usage::vertex,
                                                                  memory_access::cpu_to_gpu,
                                                                  std::span<const quad_instance>{instances},
                                                                  "dynamic quad instances");
                if (ib)
                {
                    const auto instance_count = static_cast<std::uint32_t>(instances.size());
                    float phase = 0.0f;
                    const frame_report report =
                        run_frames(ctx, opt, [&](const command_list &cl, const texture &target) {
                            phase = phase >= 1.0f ? 0.0f : phase + 0.05f;
                            fill_quad_instances(instances, count, phase);
                            ib.write(instances);

                            begin_quad_pass(cl, target, extent, instanced_pipeline);
                            set_vertex_buffer(cl, 0, corners.handle());
                            set_vertex_buffer(cl, 1, ib.handle());
                            set_index_buffer(cl, indices.handle(), index_type::uint16);
                            draw_indexed(cl, static_cast<std::uint32_t>(std::size(unit_indices)), instance_count);
                            end_render_pass(cl);
                        });
                    report_quads("instanced, instances rewritten every frame" + suffix, report, count, 1);
                    print_value("upload", static_cast<double>(instances.size() * sizeof(quad_instance)) / 1024.0,
                                "KiB per frame");
                }
                ib.destroy();
            }

            // --- one push_constants + draw per quad ----------------------------------------------------------
            if (count > opt.max_draw_calls)
            {
                print_name("per-draw, one draw call per quad" + suffix);
                print_note("skipped", "above --max-draw-calls");
            }
            else if (unit)
            {
                const grid layout(count);
                std::vector<quad_transform> transforms;
                transforms.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto [cx, cy] = layout.center(i);
                    transforms.push_back({cx, cy, layout.half_w, layout.half_h});
                }

                const frame_report report = run_frames(ctx, opt, [&](const command_list &cl, const texture &target) {
                    begin_quad_pass(cl, target, extent, batched_pipeline);
                    set_vertex_buffer(cl, 0, unit.handle());
                    for (const quad_transform &transform : transforms)
                    {
                        push_constants(cl, 0, std::as_bytes(std::span{&transform, 1}));
                        draw(cl, static_cast<std::uint32_t>(std::size(unit_quad)));
                    }
                    end_render_pass(cl);
                });
                report_quads("per-draw, one draw call per quad" + suffix, report, count, count);
            }
        }

        corners.destroy();
        indices.destroy();
        unit.destroy();
        destroy_pipeline(batched_pipeline);
        destroy_pipeline(instanced_pipeline);
    }

} // namespace catalyst::bench::render
