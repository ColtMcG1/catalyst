/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief The `resources` suite: how fast buffers, textures, samplers and command lists are created, and how much
 * bandwidth the upload, readback and device-to-device paths deliver.
 * @details Creation is measured with `measure_lifecycle`, which times a batch of creations and then their destruction
 * separately, so a destroy that has to wait for the GPU is never charged to the create. The transfer numbers are the
 * ones to watch when a frame stalls: `memory_access::cpu_to_gpu` writes are a plain memcpy into host-visible memory,
 * while `memory_access::gpu_only` writes go through the staging ring and a copy submitted on the copy queue.
 *
 * Since Tier 4 the second of those no longer waits for the GPU, so what these numbers measure is the CPU cost of
 * staging - the copy itself overlaps whatever else the device is doing. The `(direct)` suffix in a label means the
 * adapter's device-local memory turned out to be host-visible, as it is on every unified-memory part, and the write
 * never touched the staging path at all; that is the correct behaviour and it is also why this suite says little
 * about Tier 4 on such a machine.
 */

#include "render_bench.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace catalyst::bench::render
{
    using namespace catalyst::rendering;

    namespace
    {
        constexpr std::size_t kib = 1024;
        constexpr std::size_t mib = 1024 * 1024;

        buffer make_buffer(const device &dev, std::size_t size, buffer_usage usage, memory_access access)
        {
            buffer_desc desc;
            desc.size_bytes = size;
            desc.usage = usage;
            desc.access = access;
            return create_buffer(dev, desc);
        }

        std::string size_label(std::size_t bytes)
        {
            if (bytes >= mib)
                return std::to_string(bytes / mib) + " MiB";
            if (bytes >= kib)
                return std::to_string(bytes / kib) + " KiB";
            return std::to_string(bytes) + " B";
        }

        /** Times `write_buffer` of `payload` into `target` and reports the bandwidth it sustained. */
        void measure_upload(std::string_view name, const buffer &target, std::span<const std::byte> payload,
                            std::size_t iterations)
        {
            if (!target)
            {
                print_name(name);
                print_note("skipped", "buffer creation failed");
                return;
            }

            const stats s = measure(iterations, [&] { write_buffer(target, 0, payload); });
            print_name(name);
            print_stats("write", s);
            print_value("bandwidth", mib_per_second(payload.size(), s.mean_ms / 1'000.0), "MiB/s");
        }
    } // namespace

    void run_resource_suite(context &ctx, const options &opt)
    {
        print_suite("resource creation and transfers");

        const device &dev = ctx.device();
        const std::size_t many = opt.resource_iterations;
        const std::size_t some = std::max<std::size_t>(many / 20, 8);
        const std::size_t few = std::max<std::size_t>(many / 200, 4);

        // -------------------------------------------------------------------------
        // Creation
        // -------------------------------------------------------------------------

        print_lifecycle("buffer 256 B (cpu_to_gpu)",
                        measure_lifecycle(
                            many,
                            [&] { return make_buffer(dev, 256, buffer_usage::uniform, memory_access::cpu_to_gpu); },
                            [](buffer &b) { destroy_buffer(b); }));

        print_lifecycle("buffer 256 B (gpu_only)",
                        measure_lifecycle(
                            many,
                            [&] { return make_buffer(dev, 256, buffer_usage::vertex, memory_access::gpu_only); },
                            [](buffer &b) { destroy_buffer(b); }));

        print_lifecycle("buffer 1 MiB (gpu_only)",
                        measure_lifecycle(
                            some,
                            [&] { return make_buffer(dev, mib, buffer_usage::vertex, memory_access::gpu_only); },
                            [](buffer &b) { destroy_buffer(b); }));

        print_lifecycle("texture 256x256 rgba8 (sampled)",
                        measure_lifecycle(
                            some,
                            [&] {
                                texture_desc desc;
                                desc.extent = {256, 256, 1};
                                desc.pixel_format = format::rgba8_unorm;
                                desc.usage = texture_usage::sampled | texture_usage::transfer_dst;
                                return create_texture(dev, desc);
                            },
                            [](texture &t) { destroy_texture(t); }));

        print_lifecycle("texture 1024x1024 rgba8 (render target)",
                        measure_lifecycle(
                            few,
                            [&] {
                                texture_desc desc;
                                desc.extent = {1024, 1024, 1};
                                desc.pixel_format = format::rgba8_unorm;
                                desc.usage = texture_usage::render_target | texture_usage::sampled;
                                return create_texture(dev, desc);
                            },
                            [](texture &t) { destroy_texture(t); }));

        print_lifecycle("sampler", measure_lifecycle(
                                      many, [&] { return create_sampler(dev, {}); },
                                      [](sampler &s) { destroy_sampler(s); }));

        print_lifecycle("command list", measure_lifecycle(
                                            some, [&] { return create_command_list(dev, {}); },
                                            [](command_list &cl) { destroy_command_list(cl); }));

        // -------------------------------------------------------------------------
        // Uploads
        // -------------------------------------------------------------------------

        std::vector<std::byte> payload(16 * mib, std::byte{0x5A});

        for (const std::size_t size : {64 * kib, 4 * mib})
        {
            const std::size_t iterations = size <= 64 * kib ? some : few;
            const std::span<const std::byte> data{payload.data(), size};

            buffer host = make_buffer(dev, size, buffer_usage::uniform | buffer_usage::transfer_src,
                                      memory_access::cpu_to_gpu);
            measure_upload("write_buffer " + size_label(size) + " (cpu_to_gpu, direct)", host, data, iterations);
            destroy_buffer(host);

            // Whether this stages or writes straight through depends on the adapter, so say which one was measured
            // rather than assuming a staging copy.
            const char *const local_path = get_device_info(dev).unified_memory ? ", direct" : ", staged";
            buffer local = make_buffer(dev, size, buffer_usage::vertex | buffer_usage::transfer_dst,
                                       memory_access::gpu_only);
            measure_upload("write_buffer " + size_label(size) + " (gpu_only" + local_path + ")", local, data,
                           iterations);
            destroy_buffer(local);
        }

        // create_buffer with initial data: the path a static mesh upload actually takes.
        {
            const std::span<const std::byte> data{payload.data(), 4 * mib};
            buffer_desc desc;
            desc.size_bytes = data.size();
            desc.usage = buffer_usage::vertex | buffer_usage::transfer_dst;
            desc.access = memory_access::gpu_only;

            const lifecycle_report report = measure_lifecycle(
                few, [&] { return create_buffer(dev, desc, data); }, [](buffer &b) { destroy_buffer(b); });
            print_lifecycle("create_buffer 4 MiB with initial data (gpu_only)", report);
            print_value("bandwidth", mib_per_second(data.size(), report.create.mean_ms / 1'000.0), "MiB/s");
        }

        // -------------------------------------------------------------------------
        // Readback and device-to-device copies
        // -------------------------------------------------------------------------

        {
            const std::size_t size = 4 * mib;
            buffer readback = make_buffer(dev, size, buffer_usage::transfer_dst, memory_access::gpu_to_cpu);
            if (readback)
            {
                std::vector<std::byte> out(size);
                const stats s = measure(few, [&] { read_buffer(readback, 0, out); });
                print_name("read_buffer 4 MiB (gpu_to_cpu)");
                print_stats("read", s);
                print_value("bandwidth", mib_per_second(size, s.mean_ms / 1'000.0), "MiB/s");
            }
            destroy_buffer(readback);
        }

        if (ctx.executes_gpu_work())
        {
            const std::size_t size = 16 * mib;
            buffer src = make_buffer(dev, size, buffer_usage::transfer_src | buffer_usage::vertex,
                                     memory_access::gpu_only);
            buffer dst = make_buffer(dev, size, buffer_usage::transfer_dst | buffer_usage::vertex,
                                     memory_access::gpu_only);
            command_list cl = create_command_list(dev, {.debug_name = "copy"});

            if (src && dst && cl)
            {
                // Recorded, submitted and waited for as one unit: device-to-device bandwidth plus one submit.
                const stats s = measure(few, [&] {
                    begin_recording(cl);
                    copy_buffer(cl, src, 0, dst, 0, size);
                    end_recording(cl);
                    (void)submit(get_queue(dev), cl);
                    wait_idle(dev);
                });
                print_name("copy_buffer 16 MiB on device (submit + wait)");
                print_stats("copy", s);
                print_value("bandwidth", mib_per_second(size, s.mean_ms / 1'000.0), "MiB/s");
            }

            destroy_command_list(cl);
            destroy_buffer(src);
            destroy_buffer(dst);
        }
    }

} // namespace catalyst::bench::render
