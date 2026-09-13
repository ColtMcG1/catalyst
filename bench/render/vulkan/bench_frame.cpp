/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief The `frame` suite: the fixed cost of getting a frame on screen before any geometry is drawn.
 * @details Three frames of increasing content - nothing recorded at all, a render pass that discards the target, and a
 * render pass that clears and stores it - separate submission and presentation overhead from the bandwidth a full-target
 * clear costs. The `quads` numbers should be read net of the clear-only frame. The suite closes with the cost of
 * recreating the swapchain, which is what a window resize pays.
 */

#include "render_bench.hpp"

#include <algorithm>
#include <cstdint>
#include <span>

namespace catalyst::bench::render
{
    using namespace catalyst::rendering;

    void run_frame_suite(context &ctx, const options &opt)
    {
        print_suite("frame overhead");

        const extent2d extent = ctx.extent();

        print_frames("empty frame (acquire, submit an empty list, present)",
                     run_frames(ctx, opt, [](const command_list &, const texture &) {}));

        print_frames("render pass, contents discarded (no clear, no store)",
                     run_frames(ctx, opt, [](const command_list &cl, const texture &target) {
                         const color_attachment color{
                             .target = target,
                             .load = load_op::dont_care,
                             .store = store_op::dont_care,
                         };
                         begin_render_pass(cl, {.color_attachments = std::span{&color, 1}, .debug_name = "discard"});
                         end_render_pass(cl);
                     }));

        print_frames("render pass, cleared and stored",
                     run_frames(ctx, opt, [](const command_list &cl, const texture &target) {
                         const color_attachment color{
                             .target = target,
                             .load = load_op::clear,
                             .store = store_op::store,
                             .clear = {0.05f, 0.07f, 0.10f, 1.0f},
                         };
                         begin_render_pass(cl, {.color_attachments = std::span{&color, 1}, .debug_name = "clear"});
                         end_render_pass(cl);
                     }));

        print_frames("render pass with viewport and scissor set",
                     run_frames(ctx, opt, [extent](const command_list &cl, const texture &target) {
                         const color_attachment color{
                             .target = target,
                             .load = load_op::clear,
                             .store = store_op::store,
                             .clear = {0.05f, 0.07f, 0.10f, 1.0f},
                         };
                         begin_render_pass(cl, {.color_attachments = std::span{&color, 1}, .debug_name = "clear"});
                         set_viewport(cl, {0.0f, 0.0f, static_cast<float>(extent.width),
                                           static_cast<float>(extent.height)});
                         set_scissor(cl, {0, 0, extent.width, extent.height});
                         end_render_pass(cl);
                     }));

        // Swapchain recreation, i.e. what a window resize costs. A windowed swapchain is clamped to the current client
        // rect by the surface, so this measures the recreate rather than a genuine change of size.
        {
            const std::size_t iterations = std::max<std::size_t>(opt.resource_iterations / 200, 4);
            bool toggle = false;
            const stats s = measure(iterations, [&] {
                const extent2d target = toggle ? extent : extent2d{extent.width / 2 + 1, extent.height / 2 + 1};
                toggle = !toggle;
                ctx.resize(target);
            });
            ctx.resize(extent);

            print_name("resize_swapchain (recreate back buffers)");
            print_stats("resize", s);
        }
    }

} // namespace catalyst::bench::render
