/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Shared scaffolding for the rendering benchmarks: command-line options, a `context` that owns the one device and
 * render target every suite shares, per-frame sample statistics and the frame loop the draw benchmarks are built on.
 * @details The suites live in bench_pipeline.cpp, bench_resources.cpp, bench_frame.cpp and bench_quads.cpp and are
 * driven by main.cpp. Everything here is written against the public rendering API only, so the same binary is
 * meaningful on any compiled-in backend; suites that need real GPU execution say so when the backend does not provide
 * it.
 */

#pragma once

#include <catalyst/platform/window.hpp>
#include <catalyst/rendering/rendering.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace catalyst::bench::render
{
    /**
     * @struct options
     * @brief Everything the command line can change. Defaults aim at a run of a few seconds on a desktop GPU.
     */
    struct options
    {
        /** Render into an off-screen image ring instead of a window; no presentation engine is involved. */
        bool offscreen = false;
        /** Frames the ring keeps in flight; 1 reproduces the fully serialised loop the module had before Tier 3. */
        std::uint32_t frames_in_flight = 3;
        /** Wait for the GPU after every frame, so per-frame samples are GPU time rather than queue-submit time. */
        bool serialize = false;
        /** Turn on the backend's validation layers. Costs a lot of performance; off by default. */
        bool validation = false;
        /** Cap presentation to the display refresh rate. Off by default or every frame number would be the refresh. */
        bool vsync = false;

        std::uint32_t width = 1280;
        std::uint32_t height = 720;
        std::uint32_t image_count = 3;

        /** Measured frames per frame-based benchmark, and unmeasured frames run before them. */
        std::uint32_t frames = 240;
        std::uint32_t warmup_frames = 32;

        /** Iterations for the CPU-side resource benchmarks. Pipeline creation is milliseconds, so it gets fewer. */
        std::size_t pipeline_iterations = 100;
        std::size_t resource_iterations = 2000;

        /** Quad counts the draw benchmarks sweep. */
        std::vector<std::size_t> quad_counts{1, 1'000, 10'000, 100'000};
        /** Upper bound on the one-draw-call-per-quad variant, which is orders of magnitude slower than the others. */
        std::size_t max_draw_calls = 20'000;

        /** Suites to run; empty means all of them. */
        std::vector<std::string> suites;

        [[nodiscard]] bool wants(std::string_view suite) const;
    };

    /** @brief Parses `argv`. Returns false when the caller should exit (`--help`, or a bad argument). */
    bool parse_options(int argc, char **argv, options &out);

    // -----------------------------------------------------------------------------
    // Reporting
    // -----------------------------------------------------------------------------

    /**
     * @struct stats
     * @brief Distribution of a set of millisecond samples.
     */
    struct stats
    {
        std::size_t count = 0;
        double min_ms = 0.0;
        double median_ms = 0.0;
        double mean_ms = 0.0;
        double p95_ms = 0.0;
        double max_ms = 0.0;
    };

    /** @brief Summarises `samples_ms`, sorting it in the process. */
    [[nodiscard]] stats summarize(std::vector<double> &samples_ms);

    /** @brief Prints a suite banner. */
    void print_suite(std::string_view name);
    /** @brief Prints the name a set of `print_*` lines belongs to. */
    void print_name(std::string_view name);
    /** @brief Prints `label` followed by min / median / p95 / max of `s` in milliseconds. */
    void print_stats(std::string_view label, const stats &s);
    /** @brief Prints one `label: value unit` line, e.g. "quads: 2.9e+06 quads/s". */
    void print_value(std::string_view label, double value, std::string_view unit);
    /** @brief Prints one `label: text` line. */
    void print_note(std::string_view label, std::string_view text);

    /** @brief `bytes / seconds` expressed in MiB/s. */
    [[nodiscard]] double mib_per_second(std::size_t bytes, double seconds) noexcept;

    /**
     * @brief Times `op` `iterations` times and summarises the samples. `op` is invoked once first to warm up.
     */
    template <typename Op>
    stats measure(std::size_t iterations, Op &&op)
    {
        using clock = std::chrono::steady_clock;
        using ms = std::chrono::duration<double, std::milli>;

        std::vector<double> samples;
        if (iterations == 0)
            return summarize(samples);

        op();
        samples.reserve(iterations);
        for (std::size_t i = 0; i < iterations; ++i)
        {
            const auto start = clock::now();
            op();
            samples.push_back(ms(clock::now() - start).count());
        }
        return summarize(samples);
    }

    /**
     * @struct lifecycle_report
     * @brief Creation and destruction cost of a resource, measured as two separate phases so a destroy that has to
     * wait for the GPU is never charged to the create.
     */
    struct lifecycle_report
    {
        stats create;
        stats destroy;
        /** Creations that came back invalid; a non-zero count invalidates the numbers. */
        std::size_t failures = 0;
    };

    /**
     * @brief Times `iterations` creations, then times destroying all of them.
     * @param create Callable returning a handle; called `iterations` times plus once to warm up.
     * @param destroy Callable taking that handle by reference, as the `destroy_*` functions do.
     * @note Every handle is held until the destroy phase, so keep `iterations` times the resource size sane.
     */
    template <typename Create, typename Destroy>
    lifecycle_report measure_lifecycle(std::size_t iterations, Create &&create, Destroy &&destroy)
    {
        using clock = std::chrono::steady_clock;
        using ms = std::chrono::duration<double, std::milli>;
        using handle_type = decltype(create());

        lifecycle_report report;
        if (iterations == 0)
            return report;

        // The first resource of a kind pays for backend state created lazily behind it; leave it out.
        {
            handle_type warmup = create();
            if (!warmup)
                ++report.failures;
            destroy(warmup);
        }

        std::vector<handle_type> handles;
        std::vector<double> create_samples;
        std::vector<double> destroy_samples;
        handles.reserve(iterations);
        create_samples.reserve(iterations);
        destroy_samples.reserve(iterations);

        for (std::size_t i = 0; i < iterations; ++i)
        {
            const auto start = clock::now();
            handle_type handle = create();
            create_samples.push_back(ms(clock::now() - start).count());
            if (!handle)
                ++report.failures;
            handles.push_back(handle);
        }

        for (handle_type &handle : handles)
        {
            const auto start = clock::now();
            destroy(handle);
            destroy_samples.push_back(ms(clock::now() - start).count());
        }

        report.create = summarize(create_samples);
        report.destroy = summarize(destroy_samples);
        return report;
    }

    /** @brief Prints a `lifecycle_report` under `name`, with the creation rate derived from the mean. */
    void print_lifecycle(std::string_view name, const lifecycle_report &report);

    // -----------------------------------------------------------------------------
    // Shared geometry and pipelines
    // -----------------------------------------------------------------------------

    /** @brief Vertex of the batched and one-draw-per-quad benchmarks; matches shaders/quad.vert. */
    struct quad_vertex
    {
        float x = 0.0f, y = 0.0f;
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    };

    /** @brief Unit-quad corner consumed by the instanced benchmark; matches location 0 of shaders/quad_instanced.vert. */
    struct quad_corner
    {
        float x = 0.0f, y = 0.0f;
    };

    /** @brief Per-instance placement and colour; matches locations 1-3 of shaders/quad_instanced.vert. */
    struct quad_instance
    {
        float center_x = 0.0f, center_y = 0.0f;
        float half_w = 0.0f, half_h = 0.0f;
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    };

    /** @brief Push-constant block of shaders/quad.vert: xy translation, zw scale. */
    struct quad_transform
    {
        float offset_x = 0.0f, offset_y = 0.0f;
        float scale_x = 1.0f, scale_y = 1.0f;
    };

    /** @brief True when the compiled-in backend consumes the SPIR-V embedded in shaders.hpp. */
    [[nodiscard]] bool backend_consumes_spirv() noexcept;

    /** @brief Pipeline for the batched and one-draw-per-quad benchmarks. Invalid when the backend cannot build it. */
    [[nodiscard]] rendering::pipeline create_quad_pipeline(const rendering::device &dev,
                                                           rendering::format color_format);
    /** @brief Pipeline for the instanced quad benchmark. Invalid when the backend cannot build it. */
    [[nodiscard]] rendering::pipeline create_instanced_quad_pipeline(const rendering::device &dev,
                                                                     rendering::format color_format);

    // -----------------------------------------------------------------------------
    // Device and render target
    // -----------------------------------------------------------------------------

    /**
     * @class context
     * @brief The device, optional window and swapchain shared by every suite of one run.
     * @details Off-screen runs create a window-less swapchain, which the backend serves as a plain ring of render-target
     * textures: `acquire`/`present` then cost nothing and the frame numbers are pure rendering. Windowed runs go through
     * the real presentation engine, so they also include acquire and present.
     */
    class context
    {
    public:
        context() = default;
        context(const context &) = delete;
        context &operator=(const context &) = delete;
        ~context() { destroy(); }

        /** @brief Creates the window (unless off-screen), device and swapchain. Reports what failed to stderr. */
        [[nodiscard]] bool create(const options &opt);
        void destroy() noexcept;

        [[nodiscard]] const rendering::device &device() const noexcept { return device_; }
        [[nodiscard]] const rendering::swapchain &swapchain() const noexcept { return swapchain_; }
        [[nodiscard]] const rendering::swapchain_desc &swapchain_desc() const noexcept { return desc_; }
        [[nodiscard]] rendering::format color_format() const noexcept { return desc_.pixel_format; }
        [[nodiscard]] rendering::extent2d extent() const noexcept { return desc_.extent; }
        [[nodiscard]] bool windowed() const noexcept { return static_cast<bool>(window_); }
        /** @brief True when the backend actually executes submitted work on a GPU. */
        [[nodiscard]] bool executes_gpu_work() const noexcept;

        /** @brief Drains the OS event queue so a windowed run stays responsive. No-op off-screen. */
        void pump() noexcept;

        /**
         * @brief Acquires the next render target, resizing the swapchain to the window's client rect when the backend
         * reports it is out of date. An invalid handle means "skip this frame" (minimised window).
         */
        [[nodiscard]] rendering::texture acquire();
        bool present();

        /** @brief Resizes the swapchain and refreshes the cached description. */
        bool resize(rendering::extent2d extent);

    private:
        platform::window window_{};
        rendering::device device_{};
        rendering::swapchain swapchain_{};
        rendering::swapchain_desc desc_{};
    };

    // -----------------------------------------------------------------------------
    // Frame loop
    // -----------------------------------------------------------------------------

    /**
     * @struct frame_report
     * @brief Result of `run_frames`: wall-clock throughput plus where each frame's time went.
     * @details `wall_ms` covers the measured frames end to end and includes a final `wait_idle`, so `average_frame_ms`
     * is a true throughput figure even when the backend lets submissions pile up. Time spent pumping the window's
     * message queue and backing off after a skipped frame is subtracted: neither is work the renderer does, and a
     * burst of OS messages would otherwise show up as a slow frame.
     *
     * The phases matter because only one of them is CPU work. `acquire` is the presentation engine handing over a back
     * buffer, `sync` is `frame_ring::begin` waiting for the frame `frames_in_flight` frames back - with a GPU-bound
     * scene that is where the frame spends its time, and it is the same wait `begin_recording` used to perform
     * invisibly with a single list - `record` is the commands themselves, and `submit` is `submit` plus `present`
     * (plus a wait on the frame's own point under `--serialize`).
     */
    struct frame_report
    {
        std::size_t frames = 0;
        std::size_t skipped = 0;
        double wall_ms = 0.0;
        stats acquire;
        stats sync;
        stats record;
        stats submit;
        stats frame;

        [[nodiscard]] double average_frame_ms() const noexcept
        {
            return frames == 0 ? 0.0 : wall_ms / static_cast<double>(frames);
        }
        [[nodiscard]] double frames_per_second() const noexcept
        {
            const double ms = average_frame_ms();
            return ms <= 0.0 ? 0.0 : 1'000.0 / ms;
        }
    };

    /**
     * @brief Runs `record` for `opt.warmup_frames` unmeasured and then `opt.frames` measured frames.
     * @param record Invoked as `record(command_list, back_buffer)` between `begin_recording` and `end_recording`; it
     * must not begin or end the recording itself. The list is submitted and presented for it.
     */
    template <typename Record>
    frame_report run_frames(context &ctx, const options &opt, Record &&record)
    {
        using clock = std::chrono::steady_clock;
        using ms = std::chrono::duration<double, std::milli>;

        frame_report report;

        // One pool per frame in flight, and one list per pool. `frames.begin()` performs the wait
        // that `begin_recording` used to hide, which is why `sync` is measured around it.
        auto frames = rendering::frame_ring::create(
            ctx.device(), {.frames_in_flight = opt.frames_in_flight, .debug_name = "benchmark"});
        if (!frames)
            return report;

        std::vector<rendering::command_list> lists;

        std::vector<double> acquire_samples;
        std::vector<double> sync_samples;
        std::vector<double> record_samples;
        std::vector<double> submit_samples;
        std::vector<double> frame_samples;

        // One frame of work: acquire, record, submit, present. Returns false when the target was not available.
        const auto frame = [&](bool measured) {
            const auto acquire_start = clock::now();
            const rendering::texture back_buffer = ctx.acquire();
            const auto acquired = clock::now();
            if (!back_buffer)
                return false;

            // The frame's sync point, named and timed: it waits for the frame `frames_in_flight`
            // frames back and recycles its pool.
            auto f = frames->begin();
            if (!f)
                return false;
            while (lists.size() <= f->slot())
                lists.push_back(rendering::create_command_list(f->pool(), "benchmark frame"));
            const rendering::command_list cl = lists[f->slot()];

            rendering::begin_recording(cl);
            const auto record_start = clock::now();
            record(cl, back_buffer);
            rendering::end_recording(cl);
            const auto record_end = clock::now();

            const auto done = rendering::submit(rendering::get_queue(ctx.device()), cl);
            ctx.present();
            f->end(done ? *done : rendering::timeline_point{});
            if (opt.serialize && done)
                (void)done->wait(); // The frame's own point, rather than every queue on the device.
            const auto submitted = clock::now();

            if (measured)
            {
                acquire_samples.push_back(ms(acquired - acquire_start).count());
                sync_samples.push_back(ms(record_start - acquired).count());
                record_samples.push_back(ms(record_end - record_start).count());
                submit_samples.push_back(ms(submitted - record_end).count());
            }
            return true;
        };

        for (std::uint32_t i = 0; i < opt.warmup_frames;)
        {
            ctx.pump();
            if (frame(false))
                ++i;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        rendering::wait_idle(ctx.device());

        acquire_samples.reserve(opt.frames);
        sync_samples.reserve(opt.frames);
        record_samples.reserve(opt.frames);
        submit_samples.reserve(opt.frames);
        frame_samples.reserve(opt.frames);

        // A minimised window can starve the loop; give up rather than spin forever.
        const std::size_t max_attempts = static_cast<std::size_t>(opt.frames) * 4u + 64u;
        std::size_t attempts = 0;

        // Window-message pumping and the post-skip backoff happen between frames but are not rendering work, so they
        // are timed and deducted rather than left to inflate the throughput figure.
        double overhead_ms = 0.0;

        const auto wall_start = clock::now();
        while (frame_samples.size() < opt.frames && attempts < max_attempts)
        {
            ++attempts;

            const auto pump_start = clock::now();
            ctx.pump();
            overhead_ms += ms(clock::now() - pump_start).count();

            const auto frame_start = clock::now();
            if (!frame(true))
            {
                ++report.skipped;
                const auto backoff_start = clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                overhead_ms += ms(clock::now() - backoff_start).count();
                continue;
            }
            frame_samples.push_back(ms(clock::now() - frame_start).count());
        }
        rendering::wait_idle(ctx.device());
        report.wall_ms = ms(clock::now() - wall_start).count() - overhead_ms;

        for (rendering::command_list &list : lists)
            rendering::destroy_command_list(list);
        *frames = rendering::frame_ring{};

        report.frames = frame_samples.size();
        report.acquire = summarize(acquire_samples);
        report.sync = summarize(sync_samples);
        report.record = summarize(record_samples);
        report.submit = summarize(submit_samples);
        report.frame = summarize(frame_samples);
        return report;
    }

    /** @brief Prints a `frame_report` under `name`. */
    void print_frames(std::string_view name, const frame_report &report);

    // -----------------------------------------------------------------------------
    // Suites
    // -----------------------------------------------------------------------------

    void run_pipeline_suite(context &ctx, const options &opt);
    void run_resource_suite(context &ctx, const options &opt);
    void run_frame_suite(context &ctx, const options &opt);
    void run_quad_suite(context &ctx, const options &opt);

} // namespace catalyst::bench::render
