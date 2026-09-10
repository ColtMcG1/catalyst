/**
 * @file test_stream.cpp
 * @brief Exercises the backend-independent half of the module: opening and closing a stream,
 * configuration validation, error reporting and formatting, backend discovery, device enumeration
 * and selection, the renderer seam, and move semantics.
 * @details Everything here runs against the null backend and against types with no backend at all,
 * so it is deterministic and needs no audio hardware. What a *rendering* stream does is
 * test_offline.cpp's job.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/audio/audio.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

using namespace catalyst;
using namespace catalyst::audio;

namespace
{

    void render_silence(render_block &block) noexcept
    {
        block.silence();
    }

    stream_config null_config()
    {
        stream_config config;
        config.backend = backend_kind::null;
        config.sample_rate = 44100;
        config.output_channels = 2;
        config.block_frames = 128;
        return config;
    }

    void test_module_name()
    {
        CT_REQUIRE(std::string_view(module_name()) == "catalyst::audio");
    }

    /// An open stream is the only kind there is: `open` either hands back a usable object or an
    /// error, so there is no "constructed but not initialized" state left to test.
    void test_open_yields_a_configured_stream()
    {
        auto opened = stream::open(null_config(), &render_silence);
        CT_REQUIRE(opened.has_value());

        stream &s = *opened;
        CT_REQUIRE(s.backend() == backend_kind::null);
        CT_REQUIRE(s.backend_name() == "null");
        CT_REQUIRE(!s.is_running());

        const stream_info &info = s.info();
        CT_REQUIRE(info.backend == backend_kind::null);
        CT_REQUIRE(info.direction == stream_direction::output);
        CT_REQUIRE(info.sample_rate == 44100);
        CT_REQUIRE(info.output_channels == 2);
        CT_REQUIRE(info.input_channels == 0);
        CT_REQUIRE(info.output_layout == channel_layout::stereo);
        CT_REQUIRE(info.block_frames == 128);
        CT_REQUIRE(info.device_id == "null");
    }

    void test_config_validation()
    {
        const auto rejects = [](auto mutate) {
            stream_config config = null_config();
            mutate(config);
            const auto result = stream::open(config, &render_silence);
            CT_REQUIRE(!result.has_value());
            CT_REQUIRE(result.error().code == error_code::invalid_config);
        };

        rejects([](stream_config &c) { c.sample_rate = 0; });
        rejects([](stream_config &c) { c.output_channels = 0; });
        rejects([](stream_config &c) { c.output_channels = 4096; });
        rejects([](stream_config &c) { c.block_frames = 1u << 24; });

        // Input direction without input channels is meaningless.
        rejects([](stream_config &c) {
            c.direction = stream_direction::input;
            c.input_channels = 0;
        });
    }

    void test_lifecycle_is_idempotent()
    {
        auto opened = stream::open(null_config(), &render_silence);
        CT_REQUIRE(opened.has_value());

        stream &s = *opened;

        CT_REQUIRE(s.start().has_value());
        CT_REQUIRE(s.start().has_value()); // already running
        CT_REQUIRE(s.is_running());

        s.stop();
        s.stop();
        CT_REQUIRE(!s.is_running());

        // Still open after stopping: the device is only released by the destructor.
        CT_REQUIRE(s.info().sample_rate == 44100);
        CT_REQUIRE(s.start().has_value());
    }

    /// The renderer is optional. A stream without one is legal and silent.
    void test_empty_renderer_is_accepted()
    {
        auto opened = stream::open(null_config(), renderer{});
        CT_REQUIRE(opened.has_value());
        CT_REQUIRE(opened->start().has_value());
    }

    void test_backend_availability()
    {
        // Compiled unconditionally on every platform.
        CT_REQUIRE(is_available(backend_kind::offline));
        CT_REQUIRE(is_available(backend_kind::null));
        CT_REQUIRE(is_available(backend_kind::automatic));

        // Not implemented in this module yet.
        CT_REQUIRE(!is_available(backend_kind::alsa));
        CT_REQUIRE(!is_available(backend_kind::coreaudio));

        const auto backends = available_backends();
        CT_REQUIRE(!backends.empty());

        const auto contains = [&backends](backend_kind backend) {
            return std::find(backends.begin(), backends.end(), backend) != backends.end();
        };

        CT_REQUIRE(contains(backend_kind::offline));
        CT_REQUIRE(contains(backend_kind::null));

        // `automatic` is a request, never a result.
        CT_REQUIRE(!contains(backend_kind::automatic));

        for (const auto backend : backends)
            CT_REQUIRE(is_available(backend));

        // The always-compiled fallbacks come last, so a caller can rely on the list being ordered
        // best-first without knowing which platform backends were built.
        CT_REQUIRE(backends.size() >= 2);
        CT_REQUIRE(backends[backends.size() - 2] == backend_kind::offline);
        CT_REQUIRE(backends[backends.size() - 1] == backend_kind::null);

        // `automatic` resolves to something real, and never to the caller-clocked renderer.
        CT_REQUIRE(default_backend() != backend_kind::automatic);
        CT_REQUIRE(default_backend() != backend_kind::offline);
        CT_REQUIRE(is_available(default_backend()));
    }

    void test_unavailable_backend_is_reported()
    {
        stream_config config = null_config();
        config.backend = backend_kind::alsa;

        const auto result = stream::open(config, &render_silence);
        CT_REQUIRE(!result.has_value());
        CT_REQUIRE(result.error().code == error_code::backend_unavailable);

        const auto listed = devices(backend_kind::coreaudio);
        CT_REQUIRE(!listed.has_value());
        CT_REQUIRE(listed.error().code == error_code::backend_unavailable);

        // The offline renderer is not a device, so it has no endpoints rather than no answer.
        const auto offline_devices = devices(backend_kind::offline);
        CT_REQUIRE(offline_devices.has_value());
        CT_REQUIRE(offline_devices->empty());

        // And it cannot be opened as a live stream: that is `offline_stream`.
        stream_config as_live = null_config();
        as_live.backend = backend_kind::offline;
        const auto refused = stream::open(as_live, &render_silence);
        CT_REQUIRE(!refused.has_value());
        CT_REQUIRE(refused.error().code == error_code::backend_unavailable);
    }

    /// Enumeration returns owning values, so a result stays valid after the call that produced it.
    void test_device_enumeration_is_owning()
    {
        std::vector<device_info> saved;

        {
            const auto listed = devices(backend_kind::null);
            CT_REQUIRE(listed.has_value());
            saved = *listed;
        }

        // The null backend reports no endpoints; the point here is that asking is safe and that
        // the result outlives everything that produced it.
        CT_REQUIRE(saved.empty());

        const auto missing = default_device(stream_direction::output, backend_kind::null);
        CT_REQUIRE(!missing.has_value());
        CT_REQUIRE(missing.error().code == error_code::no_device);
    }

    /// The selector answers one question at a time, which is what the old "an id, or maybe a name"
    /// field could not do.
    void test_device_selector()
    {
        std::vector<device_info> list;

        device_info headset;
        headset.id = "{id-headset}";
        headset.name = "USB Headset";
        headset.max_output_channels = 2;
        list.push_back(headset);

        device_info interface_device;
        interface_device.id = "{id-focusrite}";
        interface_device.name = "Focusrite Scarlett 2i2";
        interface_device.max_output_channels = 2;
        interface_device.max_input_channels = 2;
        interface_device.is_default = true;
        list.push_back(interface_device);

        const device_selector by_default{};
        CT_REQUIRE(by_default.by == device_selector::match::system_default);

        const device_info *found = find_device(list, by_default);
        CT_REQUIRE(found != nullptr);
        CT_REQUIRE(found->id == "{id-focusrite}");

        found = find_device(list, device_selector::by_id("{id-headset}"));
        CT_REQUIRE(found != nullptr);
        CT_REQUIRE(found->name == "USB Headset");

        // A name match is a substring, so a picker's search box works.
        found = find_device(list, device_selector::by_name("Focusrite"));
        CT_REQUIRE(found != nullptr);
        CT_REQUIRE(found->id == "{id-focusrite}");

        CT_REQUIRE(find_device(list, device_selector::by_id("USB Headset")) == nullptr);
        CT_REQUIRE(find_device(list, device_selector::by_name("Nonexistent")) == nullptr);

        // Direction is part of the question: only one of these can capture.
        found = find_device(list, device_selector::by_name("USB"), stream_direction::input);
        CT_REQUIRE(found == nullptr);
        found = find_device(list, device_selector::by_name("Focusrite"), stream_direction::duplex);
        CT_REQUIRE(found != nullptr);
    }

    void test_move_semantics()
    {
        auto opened = stream::open(null_config(), &render_silence);
        CT_REQUIRE(opened.has_value());
        CT_REQUIRE(opened->start().has_value());

        stream moved = std::move(*opened);
        CT_REQUIRE(moved.is_running());
        CT_REQUIRE(moved.backend() == backend_kind::null);
        CT_REQUIRE(moved.info().sample_rate == 44100);

        auto other = stream::open(null_config(), &render_silence);
        CT_REQUIRE(other.has_value());

        *other = std::move(moved);
        CT_REQUIRE(other->is_running());
        CT_REQUIRE(other->info().sample_rate == 44100);
    }

    void test_names_and_formatting()
    {
        constexpr error_code codes[] = {
            error_code::none,
            error_code::invalid_config,
            error_code::backend_unavailable,
            error_code::no_device,
            error_code::device_lost,
            error_code::format_unsupported,
            error_code::device_busy,
            error_code::unsupported_operation,
            error_code::thread_failure,
            error_code::io_failure,
            error_code::platform_error,
        };

        for (const auto code : codes)
            CT_REQUIRE(!name(code).empty());

        constexpr backend_kind backends[] = {
            backend_kind::automatic, backend_kind::wasapi, backend_kind::asio,
            backend_kind::alsa,      backend_kind::coreaudio, backend_kind::offline,
            backend_kind::null,
        };

        for (const auto backend : backends)
            CT_REQUIRE(!name(backend).empty());

        CT_REQUIRE(name(backend_kind::wasapi) == "WASAPI");
        CT_REQUIRE(name(stream_direction::duplex) == "duplex");
        CT_REQUIRE(name(channel_layout::surround_5_1) == "5.1");

        // Every enum formats, so a log line needs no conversion call.
        CT_REQUIRE(std::format("{}", backend_kind::asio) == "ASIO");
        CT_REQUIRE(std::format("{}", stream_direction::input) == "input");
        CT_REQUIRE(std::format("{}", channel_layout::stereo) == "stereo");
        CT_REQUIRE(std::format("{}", error_code::no_device) == "no matching audio device");
    }

    /// An error is worth reading: it names the backend, and says what the device would have taken.
    void test_error_messages_carry_context()
    {
        const error bare = make_error(error_code::no_device, backend_kind::wasapi);
        CT_REQUIRE(bare.message() == "WASAPI: no matching audio device");

        error refused = make_error(error_code::format_unsupported, backend_kind::asio);
        refused.offered_sample_rate = 44100;
        refused.offered_channels = 2;
        CT_REQUIRE(refused.message() ==
                   "ASIO: device cannot provide the requested format (device offers 44100 Hz, 2 ch)");

        // An unresolved backend is left out rather than printed as "automatic".
        const error anonymous = make_error(error_code::invalid_config);
        CT_REQUIRE(anonymous.message() == "stream configuration is invalid");

        // Formatting an error gives the whole message, which is what a call site wants.
        CT_REQUIRE(std::format("{}", bare) == bare.message());

        CT_REQUIRE(bare != refused);
        CT_REQUIRE(bare == make_error(error_code::no_device, backend_kind::wasapi));
    }

    void test_block_helpers()
    {
        std::vector<sample> buffer(8 * 2, 0.0f);

        render_block block;
        block.output = buffer;
        block.frames = 8;
        block.output_channels = 2;
        block.sample_rate = 48000;
        block.position = 96000;

        CT_REQUIRE(block.time().count() == 2.0);
        CT_REQUIRE(block.duration().count() > 0.0);

        // Frame views remove the interleaving arithmetic from the call site.
        for (std::uint32_t frame = 0; frame < block.frames; ++frame)
        {
            const auto channels = block.output_frame(frame);
            CT_REQUIRE(channels.size() == 2);
            channels[0] = static_cast<sample>(frame);
            channels[1] = static_cast<sample>(frame) + 0.5f;
        }

        CT_REQUIRE(buffer[0] == 0.0f);
        CT_REQUIRE(buffer[1] == 0.5f);
        CT_REQUIRE(buffer[14] == 7.0f);
        CT_REQUIRE(buffer[15] == 7.5f);

        // Out of range is an empty view, not a read past the end.
        CT_REQUIRE(block.output_frame(8).empty());

        block.silence();
        for (const sample value : buffer)
            CT_REQUIRE(value == 0.0f);
    }

    /// A renderer refers to the callable rather than owning it, and calls through to it.
    void test_renderer_seam()
    {
        CT_REQUIRE(!renderer{}.valid());
        CT_REQUIRE(renderer{&render_silence}.valid());

        int calls = 0;
        auto counting = [&calls](render_block &) noexcept { ++calls; };

        const renderer render = counting;
        CT_REQUIRE(render.valid());

        render_block block;
        render(block);
        render(block);
        CT_REQUIRE(calls == 2);

        // A stateless lambda has no lifetime to get wrong, so it converts to a plain function.
        const renderer stateless = +[](render_block &b) noexcept { b.silence(); };
        CT_REQUIRE(stateless.valid());
    }

    void test_conversions()
    {
        CT_REQUIRE(frames_to_time(48000, 48000).count() == 1.0);
        CT_REQUIRE(frames_to_time(1000, 0).count() == 0.0);
        CT_REQUIRE(time_to_frames(seconds{0.5}, 48000) == 24000);
        CT_REQUIRE(time_to_frames(seconds{-1.0}, 48000) == 0);

        CT_REQUIRE(channels_in(channel_layout::surround_7_1) == 8);
        CT_REQUIRE(layout_for(6) == channel_layout::surround_5_1);
        CT_REQUIRE(layout_for(3) == channel_layout::unspecified);

        CT_REQUIRE(has_output(stream_direction::duplex));
        CT_REQUIRE(has_input(stream_direction::duplex));
        CT_REQUIRE(!has_input(stream_direction::output));

        // A fade reaches silence rather than approaching it.
        CT_REQUIRE(db_to_gain(0.0f) == 1.0f);
        CT_REQUIRE(db_to_gain(-200.0f) == 0.0f);
        CT_REQUIRE(gain_to_db(0.0f) == -120.0f);

        const sample half = db_to_gain(-6.0f);
        CT_REQUIRE(half > 0.49f && half < 0.51f);
    }

} // namespace

int main()
{
    test_module_name();
    test_open_yields_a_configured_stream();
    test_config_validation();
    test_lifecycle_is_idempotent();
    test_empty_renderer_is_accepted();
    test_backend_availability();
    test_unavailable_backend_is_reported();
    test_device_enumeration_is_owning();
    test_device_selector();
    test_move_semantics();
    test_names_and_formatting();
    test_error_messages_carry_context();
    test_block_helpers();
    test_renderer_seam();
    test_conversions();

    return 0;
}
