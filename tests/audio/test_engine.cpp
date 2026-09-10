/**
 * @file test_engine.cpp
 * @brief Exercises the backend-independent half of the audio engine: the lifecycle state machine,
 * configuration validation, error reporting, backend discovery, device enumeration and move
 * semantics. Everything here runs against the offline and null backends, so it is deterministic
 * and requires no audio hardware.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/audio/audio.hpp>
#include <catalyst/audio/engine.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

using namespace catalyst;
using namespace catalyst::audio;

namespace
{

    void render_nothing(render_context &context) noexcept
    {
        for (uint32_t i = 0; i < context.frames * context.output_channels; ++i)
            context.output[i] = 0.0f;
    }

    engine_config offline_config()
    {
        engine_config config;
        config.preferred_backend = engine_backend::offline;
        config.sample_rate = 44100;
        config.output_channels = 2;
        config.frames_per_buffer = 128;
        config.callback = &render_nothing;
        return config;
    }

    void test_module_name()
    {
        CT_REQUIRE(std::string_view(module_name()) == "catalyst::audio");
    }

    void test_default_state()
    {
        engine audio;
        CT_REQUIRE(!audio.is_initialized());
        CT_REQUIRE(!audio.is_running());
        CT_REQUIRE(audio.backend() == engine_backend::automatic);
        CT_REQUIRE(audio.backend_name().empty());
        CT_REQUIRE(audio.captured_output().empty());

        // Teardown on an untouched engine must be harmless and repeatable.
        audio.stop();
        audio.stop();
        audio.shutdown();
        audio.shutdown();

        const auto info = audio.info();
        CT_REQUIRE(info.sample_rate == 0);
        CT_REQUIRE(info.buffer_frames == 0);
    }

    void test_operations_require_initialization()
    {
        engine audio;

        const auto started = audio.start();
        CT_REQUIRE(!started.has_value());
        CT_REQUIRE(started.error() == audio_error::not_initialized);

        const auto rendered = audio.render(64);
        CT_REQUIRE(!rendered.has_value());
        CT_REQUIRE(rendered.error() == audio_error::not_initialized);
    }

    void test_config_validation()
    {
        engine audio;

        {
            auto config = offline_config();
            config.sample_rate = 0;
            const auto result = audio.initialize(config);
            CT_REQUIRE(!result.has_value());
            CT_REQUIRE(result.error() == audio_error::invalid_config);
        }

        {
            auto config = offline_config();
            config.output_channels = 0;
            const auto result = audio.initialize(config);
            CT_REQUIRE(!result.has_value());
            CT_REQUIRE(result.error() == audio_error::invalid_config);
        }

        {
            // Input direction without input channels is meaningless.
            auto config = offline_config();
            config.direction = stream_direction::input;
            config.input_channels = 0;
            const auto result = audio.initialize(config);
            CT_REQUIRE(!result.has_value());
            CT_REQUIRE(result.error() == audio_error::invalid_config);
        }

        {
            // A WAV can only be written from retained samples.
            auto config = offline_config();
            config.offline.capture_output = false;
            config.offline.wav_path = "should-not-be-created.wav";
            const auto result = audio.initialize(config);
            CT_REQUIRE(!result.has_value());
            CT_REQUIRE(result.error() == audio_error::invalid_config);
        }

        {
            auto config = offline_config();
            config.output_channels = 4096;
            const auto result = audio.initialize(config);
            CT_REQUIRE(!result.has_value());
            CT_REQUIRE(result.error() == audio_error::invalid_config);
        }

        // None of the rejected attempts may have left the engine half-open.
        CT_REQUIRE(!audio.is_initialized());
    }

    /// A second initialize() must fail rather than silently discard the caller's new settings.
    void test_double_initialize_is_rejected()
    {
        engine audio;
        CT_REQUIRE(audio.initialize(offline_config()).has_value());

        auto changed = offline_config();
        changed.sample_rate = 96000;

        const auto again = audio.initialize(changed);
        CT_REQUIRE(!again.has_value());
        CT_REQUIRE(again.error() == audio_error::already_initialized);

        // The original configuration is still the live one.
        CT_REQUIRE(audio.info().sample_rate == 44100);

        // After shutdown the new configuration is accepted.
        audio.shutdown();
        CT_REQUIRE(audio.initialize(changed).has_value());
        CT_REQUIRE(audio.info().sample_rate == 96000);
    }

    void test_render_requires_a_started_stream()
    {
        engine audio;
        CT_REQUIRE(audio.initialize(offline_config()).has_value());

        const auto before = audio.render(64);
        CT_REQUIRE(!before.has_value());
        CT_REQUIRE(before.error() == audio_error::not_running);

        CT_REQUIRE(audio.start().has_value());
        CT_REQUIRE(audio.is_running());
        CT_REQUIRE(audio.render(64).has_value());

        audio.stop();
        CT_REQUIRE(!audio.is_running());

        const auto after = audio.render(64);
        CT_REQUIRE(!after.has_value());
        CT_REQUIRE(after.error() == audio_error::not_running);
    }

    void test_lifecycle_is_idempotent()
    {
        engine audio;
        CT_REQUIRE(audio.initialize(offline_config()).has_value());
        CT_REQUIRE(audio.is_initialized());

        CT_REQUIRE(audio.start().has_value());
        CT_REQUIRE(audio.start().has_value()); // already running
        CT_REQUIRE(audio.is_running());

        audio.stop();
        audio.stop();
        CT_REQUIRE(!audio.is_running());
        CT_REQUIRE(audio.is_initialized());

        audio.shutdown();
        audio.shutdown();
        CT_REQUIRE(!audio.is_initialized());
        CT_REQUIRE(!audio.is_running());
    }

    void test_offline_reports_negotiated_format()
    {
        auto config = offline_config();
        config.sample_rate = 32000;
        config.output_channels = 1;
        config.frames_per_buffer = 256;

        engine audio;
        CT_REQUIRE(audio.initialize(config).has_value());

        CT_REQUIRE(audio.backend() == engine_backend::offline);
        CT_REQUIRE(audio.backend_name() == "Offline Renderer");

        const auto info = audio.info();
        CT_REQUIRE(info.backend == engine_backend::offline);
        CT_REQUIRE(info.direction == stream_direction::output);
        CT_REQUIRE(info.sample_rate == 32000);
        CT_REQUIRE(info.output_channels == 1);
        CT_REQUIRE(info.input_channels == 0);
        CT_REQUIRE(info.buffer_frames == 256);
        CT_REQUIRE(!info.exclusive);
        CT_REQUIRE(info.device_id == "offline");

        // 256 frames at 32 kHz is exactly 8 ms.
        CT_REQUIRE(info.output_latency_seconds > 0.0079);
        CT_REQUIRE(info.output_latency_seconds < 0.0081);
    }

    void test_null_backend()
    {
        engine_config config;
        config.preferred_backend = engine_backend::null;
        config.callback = &render_nothing;

        engine audio;
        CT_REQUIRE(audio.initialize(config).has_value());
        CT_REQUIRE(audio.backend() == engine_backend::null);
        CT_REQUIRE(audio.backend_name() == "Null");
        CT_REQUIRE(audio.start().has_value());
        CT_REQUIRE(audio.is_running());

        // The null backend has no clock to advance by hand.
        const auto rendered = audio.render(64);
        CT_REQUIRE(!rendered.has_value());
        CT_REQUIRE(rendered.error() == audio_error::unsupported_operation);

        const auto devices = audio.devices();
        CT_REQUIRE(devices.has_value());
        CT_REQUIRE(devices->empty());

        audio.shutdown();
    }

    void test_backend_availability()
    {
        // Compiled unconditionally on every platform.
        CT_REQUIRE(engine::is_backend_available(engine_backend::offline));
        CT_REQUIRE(engine::is_backend_available(engine_backend::null));
        CT_REQUIRE(engine::is_backend_available(engine_backend::automatic));

        // Not implemented in this module yet.
        CT_REQUIRE(!engine::is_backend_available(engine_backend::alsa));
        CT_REQUIRE(!engine::is_backend_available(engine_backend::coreaudio));

        const auto backends = engine::available_backends();
        CT_REQUIRE(!backends.empty());

        const auto contains = [&backends](engine_backend backend) {
            return std::find(backends.begin(), backends.end(), backend) != backends.end();
        };

        CT_REQUIRE(contains(engine_backend::offline));
        CT_REQUIRE(contains(engine_backend::null));

        // `automatic` is a request, never a result.
        CT_REQUIRE(!contains(engine_backend::automatic));

        // Which platform backends are present depends on how the module was configured, but the
        // listing must always agree with the availability query.
        for (const auto backend : backends)
            CT_REQUIRE(engine::is_backend_available(backend));

        // The always-compiled fallbacks come last, so a caller can rely on the list being ordered
        // best-first without knowing which platform backends were built.
        CT_REQUIRE(backends.size() >= 2);
        CT_REQUIRE(backends[backends.size() - 2] == engine_backend::offline);
        CT_REQUIRE(backends[backends.size() - 1] == engine_backend::null);
    }

    /// Enumeration returns owning values, so results stay valid after further calls and after the
    /// engine that produced them is gone.
    void test_device_enumeration_results_are_owning()
    {
        std::vector<device_info> saved;

        {
            engine audio;
            CT_REQUIRE(audio.initialize(offline_config()).has_value());

            const auto first = audio.devices();
            CT_REQUIRE(first.has_value());
            CT_REQUIRE(first->size() == 1);
            saved = *first;

            // A second enumeration must not disturb the first result.
            const auto second = audio.devices();
            CT_REQUIRE(second.has_value());
            CT_REQUIRE(second->size() == 1);
        }

        CT_REQUIRE(saved.size() == 1);
        CT_REQUIRE(saved[0].id == "offline");
        CT_REQUIRE(saved[0].name == "Offline Renderer");
        CT_REQUIRE(saved[0].backend == engine_backend::offline);
        CT_REQUIRE(saved[0].is_default);

        // Enumeration without constructing an engine.
        const auto direct = engine::devices(engine_backend::offline);
        CT_REQUIRE(direct.has_value());
        CT_REQUIRE(direct->size() == 1);
        CT_REQUIRE((*direct)[0].id == "offline");
    }

    void test_unavailable_backend_is_reported()
    {
        engine_config config;
        config.preferred_backend = engine_backend::alsa;
        config.callback = &render_nothing;

        engine audio;
        const auto result = audio.initialize(config);
        CT_REQUIRE(!result.has_value());
        CT_REQUIRE(result.error() == audio_error::backend_unavailable);

        const auto devices = engine::devices(engine_backend::coreaudio);
        CT_REQUIRE(!devices.has_value());
        CT_REQUIRE(devices.error() == audio_error::backend_unavailable);
    }

    void test_move_semantics()
    {
        engine source;
        CT_REQUIRE(source.initialize(offline_config()).has_value());
        CT_REQUIRE(source.start().has_value());
        CT_REQUIRE(source.render(128).has_value());

        engine moved = std::move(source);
        CT_REQUIRE(moved.is_initialized());
        CT_REQUIRE(moved.is_running());
        CT_REQUIRE(moved.backend() == engine_backend::offline);
        CT_REQUIRE(moved.stats().frames_rendered == 128);
        CT_REQUIRE(moved.render(128).has_value());
        CT_REQUIRE(moved.stats().frames_rendered == 256);

        engine target;
        target = std::move(moved);
        CT_REQUIRE(target.is_initialized());
        CT_REQUIRE(target.stats().frames_rendered == 256);
        CT_REQUIRE(target.info().sample_rate == 44100);

        target.shutdown();
        CT_REQUIRE(!target.is_initialized());
    }

    void test_error_and_backend_names()
    {
        constexpr audio_error errors[] = {
            audio_error::none,
            audio_error::not_initialized,
            audio_error::not_running,
            audio_error::already_initialized,
            audio_error::invalid_config,
            audio_error::backend_unavailable,
            audio_error::no_device,
            audio_error::device_lost,
            audio_error::format_unsupported,
            audio_error::device_busy,
            audio_error::thread_failure,
            audio_error::io_failure,
            audio_error::unsupported_operation,
            audio_error::platform_error,
        };

        for (const auto error : errors)
            CT_REQUIRE(!to_string(error).empty());

        constexpr engine_backend backends[] = {
            engine_backend::automatic,
            engine_backend::wasapi,
            engine_backend::asio,
            engine_backend::alsa,
            engine_backend::coreaudio,
            engine_backend::offline,
            engine_backend::null,
        };

        for (const auto backend : backends)
            CT_REQUIRE(!to_string(backend).empty());

        CT_REQUIRE(to_string(engine_backend::wasapi) == "WASAPI");
        CT_REQUIRE(to_string(audio_error::none) == "none");
    }

} // namespace

int main()
{
    test_module_name();
    test_default_state();
    test_operations_require_initialization();
    test_config_validation();
    test_double_initialize_is_rejected();
    test_render_requires_a_started_stream();
    test_lifecycle_is_idempotent();
    test_offline_reports_negotiated_format();
    test_null_backend();
    test_backend_availability();
    test_device_enumeration_results_are_owning();
    test_unavailable_backend_is_reported();
    test_move_semantics();
    test_error_and_backend_names();

    return 0;
}
