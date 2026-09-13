/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Exercises the rendering module's diagnostic vocabulary: `error_code`, `error`, their
 * formatters, and the event types and tag block declared in events.hpp.
 * @details These are the types every fallible call will report through from Tier 2 on, so the
 * properties worth pinning down are the ones later code will rely on without checking: that
 * `name()` is total and never empty, that an `error` stays trivially copyable and cheap to return,
 * that `message()` degrades gracefully when the backend filled in nothing, and that the event tags
 * sit inside the block this module owns and collide with nobody.
 */

#include "../test_common.hpp"

#include <catalyst/audio/events.hpp>
#include <catalyst/input/device.hpp>
#include <catalyst/logging/logging.hpp>
#include <catalyst/logging/sinks/callback.hpp>
#include <catalyst/rendering/rendering.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

using namespace catalyst::rendering;

namespace
{
    /** Every enumerator, so the tests below are total rather than a spot check. */
    constexpr error_code all_codes[] = {
        error_code::none,
        error_code::backend_unavailable,
        error_code::no_adapter,
        error_code::device_lost,
        error_code::surface_lost,
        error_code::swapchain_out_of_date,
        error_code::not_ready,
        error_code::out_of_device_memory,
        error_code::out_of_host_memory,
        error_code::invalid_argument,
        error_code::unsupported_format,
        error_code::unsupported_operation,
        error_code::shader_invalid,
        error_code::pipeline_creation_failed,
        error_code::timeout,
        error_code::platform_error,
    };

    void test_error_code_names()
    {
        std::set<std::string_view> seen;
        for (const error_code code : all_codes)
        {
            const std::string_view text = name(code);
            CT_REQUIRE(!text.empty());
            // Lowercase sentence fragments, so `"vulkan: {}"` reads as one message.
            CT_REQUIRE(text.front() >= 'a' && text.front() <= 'z');
            CT_REQUIRE(text.back() != '.');
            // Distinct: two codes sharing a description would make a log line ambiguous.
            CT_REQUIRE(seen.insert(text).second);
        }

        // Total: an enumerator added later without a case falls through to the catch-all rather
        // than returning an empty view or running off the end of the switch.
        CT_REQUIRE(name(static_cast<error_code>(200)) == "unknown rendering error");
    }

    void test_error_is_cheap()
    {
        // The whole point of a value-based failure is that returning one costs nothing. If this
        // ever fails, someone has put a std::string in the struct.
        static_assert(std::is_trivially_copyable_v<error>);
        static_assert(std::is_nothrow_default_constructible_v<error>);
        static_assert(sizeof(error) <= 32);

        constexpr error e = make_error(error_code::device_lost);
        static_assert(e.code == error_code::device_lost);
        static_assert(e.backend_result == 0);
        static_assert(e.backend_result_name == nullptr);
        static_assert(e.operation == nullptr);

        constexpr error named = make_error(error_code::invalid_argument, "create_buffer");
        static_assert(named.operation != nullptr);

        CT_REQUIRE(e != named);
        CT_REQUIRE(e == make_error(error_code::device_lost));
    }

    void test_classification()
    {
        // The frame loop branches on these every frame, so they have to mean what they say.
        static_assert(is_transient(error_code::not_ready));
        static_assert(is_transient(error_code::swapchain_out_of_date));
        static_assert(is_transient(error_code::timeout));
        static_assert(!is_transient(error_code::device_lost));
        static_assert(!is_transient(error_code::invalid_argument));

        static_assert(is_fatal(error_code::device_lost));
        static_assert(!is_fatal(error_code::not_ready));

        // Nothing is both: a caller that retries a transient code must not be retrying a dead
        // device forever.
        for (const error_code code : all_codes)
            CT_REQUIRE(!(is_transient(code) && is_fatal(code)));
    }

    void test_messages()
    {
        const std::string_view backend_name = to_string(backend());

        // Bare code: the backend name and the description, nothing invented.
        const std::string bare = make_error(error_code::no_adapter).message();
        CT_REQUIRE(bare.starts_with(backend_name));
        CT_REQUIRE(bare.find(name(error_code::no_adapter)) != std::string::npos);
        CT_REQUIRE(bare.find('(') == std::string::npos);

        // Operation only.
        error op = make_error(error_code::pipeline_creation_failed, "vkCreateGraphicsPipelines");
        const std::string op_text = op.message();
        CT_REQUIRE(op_text.find("vkCreateGraphicsPipelines") != std::string::npos);
        CT_REQUIRE(op_text.find("returned") == std::string::npos);

        // Operation and a named driver result: the sentence a bug report can be searched for.
        error full;
        full.code = error_code::device_lost;
        full.backend_result = -4;
        full.backend_result_name = "VK_ERROR_DEVICE_LOST";
        full.operation = "vkQueueSubmit";
        const std::string full_text = full.message();
        CT_REQUIRE(full_text.find("vkQueueSubmit returned VK_ERROR_DEVICE_LOST") != std::string::npos);

        // A result with no spelling falls back to the number rather than dropping it.
        error numeric;
        numeric.code = error_code::platform_error;
        numeric.backend_result = -1337;
        const std::string numeric_text = numeric.message();
        CT_REQUIRE(numeric_text.find("-1337") != std::string::npos);
    }

    void test_formatters()
    {
        // `logging::error<c>("{}", code)` and `("{}", err)` both have to work, and the error's
        // formatter has to be the full sentence rather than just the code.
        CT_REQUIRE(std::format("{}", error_code::timeout) == name(error_code::timeout));

        error err;
        err.code = error_code::surface_lost;
        err.operation = "vkCreateWin32SurfaceKHR";
        CT_REQUIRE(std::format("{}", err) == err.message());
        CT_REQUIRE(std::format("{}", err).find("vkCreateWin32SurfaceKHR") != std::string::npos);
    }

    void test_event_tags()
    {
        constexpr auto block_first = tags::rendering_base;
        constexpr auto block_last = tags::rendering_base + 0xFFFFu;

        constexpr catalyst::events::event_type_t rendering_tags[] = {
            tags::device_lost,
            tags::swapchain_out_of_date,
            tags::swapchain_resized,
        };

        std::set<catalyst::events::event_type_t> seen;
        for (const auto tag : rendering_tags)
        {
            CT_REQUIRE(tag >= block_first && tag <= block_last);
            CT_REQUIRE(seen.insert(tag).second);
        }

        // The block sits after audio's and does not overlap it or input's. This is the check that
        // catches a copy-pasted base value, which would silently misroute every event on the bus.
        static_assert(tags::rendering_base == 0x0003'0000u);
        static_assert(tags::rendering_base > catalyst::audio::tags::audio_base);
        CT_REQUIRE(tags::rendering_base - catalyst::audio::tags::audio_base == 0x0001'0000u);

        // Structs carry the tag the namespace declares, so the bus dispatches on a constant.
        static_assert(device_lost_event::tag == tags::device_lost);
        static_assert(swapchain_out_of_date_event::tag == tags::swapchain_out_of_date);
        static_assert(swapchain_resized_event::tag == tags::swapchain_resized);
        static_assert(catalyst::events::has_static_tag_v<device_lost_event>);
    }

    void test_event_payloads()
    {
        device_lost_event lost;
        lost.backend = backend();
        lost.cause = make_error(error_code::device_lost, "vkQueueSubmit");
        CT_REQUIRE(lost.cause.code == error_code::device_lost);
        CT_REQUIRE(is_fatal(lost.cause.code));

        swapchain_resized_event resized;
        resized.previous = {1280, 720};
        resized.current = {1920, 1080};
        CT_REQUIRE(resized.previous != resized.current);
        CT_REQUIRE(resized.swapchain == 0);
    }

    /**
     * Backend diagnostics reach `catalyst::logging` rather than stderr.
     *
     * This is the behaviour change that matters to an existing caller: before, a failing backend
     * call printed to stderr unconditionally and nothing could stop it; now it is a routed log
     * event, which means a program that installs no sink sees nothing. Worth pinning down, because
     * "the diagnostics disappeared" is otherwise a mystifying upgrade.
     *
     * The failure used is a shader whose bytecode is not SPIR-V, which the Vulkan backend rejects
     * before it touches the driver. Backends whose resource layer is the bookkeeping stand-in
     * accept it and log nothing, so the assertion on *having* logged is made only where a backend
     * is known to reject it; the assertion on *how* it logged applies to whatever did arrive.
     */
    void test_diagnostics_are_routed()
    {
        std::vector<catalyst::logging::log_event> seen;

        auto &router = catalyst::logging::default_logger();
        const auto sink = router.add_sink(catalyst::logging::callback_sink{
            [&seen](const catalyst::logging::log_event &e) { seen.push_back(e); }});

        device dev = create_device();
        CT_REQUIRE(is_valid(dev));

        // Not SPIR-V: no magic number, and a backend that inspects bytecode says so rather than
        // forwarding it. A backend whose resource layer is the bookkeeping stand-in - which is what
        // the d3d12 and metal builds link today - has nothing to inspect it with and accepts it.
        // Either is correct here; what is being tested is where the complaint goes, not whether
        // there is one.
        const std::array<std::byte, 8> garbage{};
        shader_desc desc;
        desc.stage = shader_stage::vertex;
        desc.bytecode = garbage;
        desc.bytecode_format = native_bytecode_format(backend());
        shader bad = create_shader(dev, desc);
        if (bad)
            destroy_shader(bad);

        destroy_device(dev);
        router.remove_sink(sink);

        // Whatever arrived came from this module, under one category, with the call site of the
        // backend file it was written in rather than of a logging header.
        std::size_t problems = 0;
        for (const auto &e : seen)
        {
            if (e.category != "rendering")
                continue;
            CT_REQUIRE(!e.message.empty());
            CT_REQUIRE(std::string_view{e.location.file_name()}.find("rendering") != std::string_view::npos);

            // The module also logs facts, not only failures - `create_device` reports the queue
            // families it resolved, which is exactly the sort of line worth having and worth not
            // counting here.
            if (e.level >= catalyst::logging::log_level::warn)
                ++problems;
        }

        // Vulkan rejects the bytecode itself, so it is the one backend guaranteed to have complained.
        // Elsewhere an empty capture is the honest result and not a failure.
        if (backend() == backend_kind::vulkan)
            CT_REQUIRE(problems > 0);
    }
} // namespace

int main()
{
    test_error_code_names();
    test_error_is_cheap();
    test_classification();
    test_messages();
    test_formatters();
    test_event_tags();
    test_event_payloads();
    test_diagnostics_are_routed();

    std::printf("rendering diagnostics: ok (backend %s)\n", to_string(backend()));
    return 0;
}
