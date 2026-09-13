/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file main.cpp
 * @brief Driver for the Catalyst rendering benchmarks: parses the command line, creates the one device and render
 * target every suite shares (see render_bench.hpp) and runs the selected suites.
 * @details The four suites answer separate questions about the backend:
 *   - `pipeline`  how long shader modules and pipeline state objects take to build, i.e. the cost of a pipeline
 *                 rebuild after a shader edit or a permutation miss;
 *   - `resources` how fast buffers and textures are created, uploaded to and copied on the GPU;
 *   - `frame`     what an empty and a clear-only frame cost, so draw numbers can be read net of frame overhead;
 *   - `quads`     how many quads reach the screen per second, batched into one draw, instanced, re-uploaded every
 *                 frame, or one draw call each.
 * Run with `--help` for the options; `--offscreen` takes the presentation engine out of the picture and `--serialize`
 * turns the per-frame samples from CPU pacing into GPU cost.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "render_bench.hpp"

#include <catalyst/rendering/rendering.hpp>

#include <iostream>

int main(int argc, char **argv)
{
    using namespace catalyst::bench::render;

    options opt;
    if (!parse_options(argc, argv, opt))
        return 1;

    context ctx;
    if (!ctx.create(opt))
        return 1;

    const catalyst::rendering::device_info info = catalyst::rendering::get_device_info(ctx.device());
    const catalyst::rendering::swapchain_desc &sd = ctx.swapchain_desc();

    std::cout << "Catalyst rendering benchmarks\n"
              << "  backend:    " << catalyst::rendering::to_string(info.backend) << '\n'
              << "  adapter:    " << info.adapter_name << " ("
              << (info.dedicated_video_memory_bytes >> 20) << " MiB device-local, "
              << (info.unified_memory ? "unified memory" : "discrete memory") << ")\n"
              << "  alloc cap:  " << info.max_memory_allocation_count
              << " simultaneous allocations (one per buffer and texture)\n"
              << "  target:     " << (ctx.windowed() ? "window" : "off-screen") << ' ' << sd.extent.width << 'x'
              << sd.extent.height << ", " << sd.image_count << " images, vsync "
              << (sd.vsync ? "on" : "off") << '\n'
              << "  pacing:     " << (opt.serialize ? "serialized (wait_idle every frame)" : "pipelined") << '\n';

    if (!ctx.executes_gpu_work())
    {
        std::cout << "  note:       this backend only does CPU-side bookkeeping; the numbers below are API overhead,\n"
                     "              not GPU performance\n";
    }

    if (opt.wants("pipeline"))
        run_pipeline_suite(ctx, opt);
    if (opt.wants("resources"))
        run_resource_suite(ctx, opt);
    if (opt.wants("frame"))
        run_frame_suite(ctx, opt);
    if (opt.wants("quads"))
        run_quad_suite(ctx, opt);

    std::cout << std::endl;
    return 0;
}
