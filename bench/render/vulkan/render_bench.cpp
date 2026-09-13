/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Implementation of the shared benchmark scaffolding declared in render_bench.hpp: option parsing, the
 * device/window/swapchain context and the reporting helpers.
 */

#include "render_bench.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <numeric>

namespace catalyst::bench::render
{
    namespace
    {
        /** Parses a non-negative integer; leaves `out` untouched and returns false on anything unparsable. */
        bool parse_size(std::string_view text, std::size_t &out) noexcept
        {
            std::size_t value = 0;
            const char *first = text.data();
            const char *last = first + text.size();
            const auto [ptr, ec] = std::from_chars(first, last, value);
            if (ec != std::errc{} || ptr != last)
                return false;
            out = value;
            return true;
        }

        /** Parses "1,1000,10000" into `out`. Returns false if any element is unparsable or zero. */
        bool parse_size_list(std::string_view text, std::vector<std::size_t> &out)
        {
            std::vector<std::size_t> values;
            while (!text.empty())
            {
                const std::size_t comma = text.find(',');
                const std::string_view element = text.substr(0, comma);
                std::size_t value = 0;
                if (element.empty() || !parse_size(element, value) || value == 0)
                    return false;
                values.push_back(value);
                text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
            }
            if (values.empty())
                return false;
            out = std::move(values);
            return true;
        }

        void print_usage()
        {
            std::cout << "Catalyst rendering benchmarks\n"
                         "\n"
                         "Usage: catalyst_bench_render_vulkan [options] [suite ...]\n"
                         "\n"
                         "Suites (default: all)\n"
                         "  pipeline    shader module and pipeline construction\n"
                         "  resources   buffer / texture creation, uploads and GPU copies\n"
                         "  frame       swapchain acquire, clear-only frames, resize\n"
                         "  quads       quad throughput: batched, instanced, dynamic, one draw per quad\n"
                         "\n"
                         "Options\n"
                         "  --offscreen             render into an image ring instead of a window\n"
                         "  --serialize             wait for the GPU after every frame (per-frame GPU cost)\n"
                         "  --validation            enable the backend's validation layers\n"
                         "  --vsync                 cap presentation to the display refresh rate\n"
                         "  --width N --height N    render target size (default 1280x720)\n"
                         "  --images N              swapchain image count (default 3)\n"
                         "  --in-flight N           frames the ring keeps in flight (default 3; 1 fully serialises)\n"
                         "  --frames N              measured frames per frame benchmark (default 240)\n"
                         "  --warmup N              unmeasured frames before each one (default 32)\n"
                         "  --quads a,b,c           quad counts to sweep (default 1,1000,10000,100000)\n"
                         "  --max-draw-calls N      cap on the one-draw-per-quad variant (default 20000)\n"
                         "  --pipeline-iterations N pipeline / shader creations per measurement (default 100)\n"
                         "  --resource-iterations N buffer / texture creations per measurement (default 2000)\n"
                         "  --help                  print this text\n";
        }

        /** Left-aligned "label:" column shared by every report line. */
        std::string label_field(std::string_view label)
        {
            std::string field{label};
            field.push_back(':');
            field.resize(std::max<std::size_t>(field.size(), 12), ' ');
            return field;
        }
    } // namespace

    bool options::wants(std::string_view suite) const
    {
        return suites.empty() || std::find(suites.begin(), suites.end(), suite) != suites.end();
    }

    bool parse_options(int argc, char **argv, options &out)
    {
        const auto value_of = [&](int &i, std::string_view flag) -> std::string_view {
            if (i + 1 >= argc)
            {
                std::cerr << "missing value for " << flag << '\n';
                return {};
            }
            return argv[++i];
        };

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            std::size_t value = 0;

            if (arg == "--help" || arg == "-h")
            {
                print_usage();
                return false;
            }
            else if (arg == "--offscreen")
                out.offscreen = true;
            else if (arg == "--serialize")
                out.serialize = true;
            else if (arg == "--validation")
                out.validation = true;
            else if (arg == "--vsync")
                out.vsync = true;
            else if (arg == "--width" || arg == "--height" || arg == "--images" || arg == "--frames" ||
                     arg == "--warmup" || arg == "--max-draw-calls" || arg == "--pipeline-iterations" ||
                     arg == "--in-flight" ||
                     arg == "--resource-iterations")
            {
                const std::string_view text = value_of(i, arg);
                if (text.empty() || !parse_size(text, value))
                {
                    std::cerr << "bad value for " << arg << '\n';
                    return false;
                }
                if (arg == "--width")
                    out.width = static_cast<std::uint32_t>(value);
                else if (arg == "--height")
                    out.height = static_cast<std::uint32_t>(value);
                else if (arg == "--images")
                    out.image_count = static_cast<std::uint32_t>(value);
                else if (arg == "--frames")
                    out.frames = static_cast<std::uint32_t>(value);
                else if (arg == "--in-flight")
                    out.frames_in_flight = static_cast<std::uint32_t>(value);
                else if (arg == "--warmup")
                    out.warmup_frames = static_cast<std::uint32_t>(value);
                else if (arg == "--max-draw-calls")
                    out.max_draw_calls = value;
                else if (arg == "--pipeline-iterations")
                    out.pipeline_iterations = value;
                else
                    out.resource_iterations = value;
            }
            else if (arg == "--quads")
            {
                const std::string_view text = value_of(i, arg);
                if (text.empty() || !parse_size_list(text, out.quad_counts))
                {
                    std::cerr << "bad value for --quads (expected a comma-separated list of positive counts)\n";
                    return false;
                }
            }
            else if (arg.starts_with("--"))
            {
                std::cerr << "unknown option " << arg << "\n\n";
                print_usage();
                return false;
            }
            else
            {
                out.suites.emplace_back(arg);
            }
        }

        for (const std::string &suite : out.suites)
        {
            if (suite != "pipeline" && suite != "resources" && suite != "frame" && suite != "quads")
            {
                std::cerr << "unknown suite '" << suite << "'\n\n";
                print_usage();
                return false;
            }
        }

        if (out.frames == 0 || out.width == 0 || out.height == 0 || out.image_count == 0)
        {
            std::cerr << "--frames, --width, --height and --images must all be non-zero\n";
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------------
    // Reporting
    // -----------------------------------------------------------------------------

    stats summarize(std::vector<double> &samples_ms)
    {
        stats s;
        s.count = samples_ms.size();
        if (samples_ms.empty())
            return s;

        std::sort(samples_ms.begin(), samples_ms.end());
        s.min_ms = samples_ms.front();
        s.max_ms = samples_ms.back();
        s.median_ms = samples_ms[samples_ms.size() / 2];
        s.p95_ms = samples_ms[static_cast<std::size_t>(static_cast<double>(samples_ms.size() - 1) * 0.95)];
        s.mean_ms = std::accumulate(samples_ms.begin(), samples_ms.end(), 0.0) / static_cast<double>(samples_ms.size());
        return s;
    }

    void print_suite(std::string_view name)
    {
        std::cout << "\n== " << name << " ==\n";
    }

    void print_name(std::string_view name)
    {
        std::cout << '\n' << name << '\n';
    }

    void print_stats(std::string_view label, const stats &s)
    {
        std::cout << "  " << label_field(label) << std::fixed << std::setprecision(4) << "min " << s.min_ms << "  med "
                  << s.median_ms << "  mean " << s.mean_ms << "  p95 " << s.p95_ms << "  max " << s.max_ms << " ms\n"
                  << std::defaultfloat;
    }

    void print_value(std::string_view label, double value, std::string_view unit)
    {
        std::cout << "  " << label_field(label);
        const double magnitude = std::fabs(value);
        if (magnitude >= 1'000'000.0 || (magnitude > 0.0 && magnitude < 0.001))
            std::cout << std::scientific << std::setprecision(3);
        else
            std::cout << std::fixed << std::setprecision(3);
        std::cout << value << ' ' << unit << '\n' << std::defaultfloat;
    }

    void print_note(std::string_view label, std::string_view text)
    {
        std::cout << "  " << label_field(label) << text << '\n';
    }

    double mib_per_second(std::size_t bytes, double seconds) noexcept
    {
        if (seconds <= 0.0)
            return 0.0;
        return static_cast<double>(bytes) / seconds / (1024.0 * 1024.0);
    }

    void print_lifecycle(std::string_view name, const lifecycle_report &report)
    {
        print_name(name);
        if (report.create.count == 0)
        {
            print_note("result", "not measured");
            return;
        }
        if (report.failures != 0)
            print_note("FAILED", std::to_string(report.failures) + " creations returned an invalid handle");

        std::cout << "  " << "iterations: " << report.create.count << '\n';
        print_stats("create", report.create);
        print_stats("destroy", report.destroy);
        print_value("rate", report.create.mean_ms <= 0.0 ? 0.0 : 1'000.0 / report.create.mean_ms, "creations/s");
    }

    void print_frames(std::string_view name, const frame_report &report)
    {
        print_name(name);
        if (report.frames == 0)
        {
            print_note("frames", "none completed (render target never became available)");
            return;
        }

        std::cout << "  " << label_field("frames") << report.frames;
        if (report.skipped != 0)
            std::cout << " (" << report.skipped << " skipped)";
        std::cout << '\n';

        print_value("wall", report.wall_ms, "ms");
        print_value("avg frame", report.average_frame_ms(), "ms");
        print_value("throughput", report.frames_per_second(), "FPS");
        print_stats("acquire", report.acquire);
        print_stats("sync", report.sync);
        print_stats("record", report.record);
        print_stats("submit", report.submit);
        print_stats("frame", report.frame);
    }

    // -----------------------------------------------------------------------------
    // Context
    // -----------------------------------------------------------------------------

    namespace
    {
        /** The window's client rect as a swapchain extent; {0, 0} when it is minimised. */
        rendering::extent2d client_extent(const platform::window &w) noexcept
        {
            const auto client = platform::client_rect_px(w);
            return {static_cast<std::uint32_t>(std::max(client.size().x(), 0)),
                    static_cast<std::uint32_t>(std::max(client.size().y(), 0))};
        }
    } // namespace

    bool context::create(const options &opt)
    {
        if (!opt.offscreen)
        {
            platform::window_desc wd;
            wd.title = "Catalyst rendering benchmark";
            wd.width_px = ui::px(static_cast<float>(opt.width));
            wd.height_px = ui::px(static_cast<float>(opt.height));
            wd.resizable = false; // A resize mid-run would invalidate the numbers.
            window_ = platform::create_window(wd);
            if (!window_)
            {
                std::cerr << "failed to create the benchmark window (try --offscreen)\n";
                return false;
            }
            platform::pump_events();
        }

        rendering::device_desc dd;
        dd.application_name = "catalyst_bench_render";
        dd.enable_validation = opt.validation;
        device_ = rendering::create_device(dd);
        if (!device_)
        {
            std::cerr << "failed to create the rendering device\n";
            return false;
        }

        rendering::swapchain_desc sd;
        if (window_)
        {
            sd.window = platform::get_native_handle(window_);
            const rendering::extent2d extent = client_extent(window_);
            sd.extent = extent.width != 0 && extent.height != 0 ? extent : rendering::extent2d{opt.width, opt.height};
        }
        else
        {
            sd.extent = {opt.width, opt.height};
        }
        sd.image_count = opt.image_count;
        sd.vsync = opt.vsync;
        sd.debug_name = "benchmark swapchain";

        swapchain_ = rendering::create_swapchain(device_, sd);
        if (!swapchain_)
        {
            std::cerr << "failed to create the swapchain\n";
            return false;
        }
        desc_ = rendering::get_swapchain_desc(swapchain_); // The backend may have adjusted extent, format or count.
        return true;
    }

    void context::destroy() noexcept
    {
        if (device_)
            rendering::wait_idle(device_);
        rendering::destroy_swapchain(swapchain_);
        rendering::destroy_device(device_);
        platform::destroy_window(window_);
    }

    bool context::executes_gpu_work() const noexcept
    {
        return rendering::backend() == rendering::backend_kind::vulkan;
    }

    void context::pump() noexcept
    {
        if (window_)
            platform::pump_events();
    }

    rendering::texture context::acquire()
    {
        rendering::texture back_buffer = rendering::acquire_next_image(swapchain_);
        if (back_buffer || !window_)
            return back_buffer;

        // Out of date (resized or minimised): match the current client size and try once more.
        const rendering::extent2d extent = client_extent(window_);
        if (extent.width == 0 || extent.height == 0 || !resize(extent))
            return {};
        return rendering::acquire_next_image(swapchain_);
    }

    bool context::present()
    {
        return rendering::present(swapchain_);
    }

    bool context::resize(rendering::extent2d extent)
    {
        if (!rendering::resize_swapchain(swapchain_, extent))
            return false;
        desc_ = rendering::get_swapchain_desc(swapchain_);
        return true;
    }

} // namespace catalyst::bench::render
