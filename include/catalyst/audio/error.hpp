/**
 * @file error.hpp
 * @brief The failure value every fallible audio operation returns through `std::expected`.
 * @details Losing a device to a driver update, or being handed one another process already holds
 * exclusively, is an ordinary outcome for a program that talks to sound hardware - not an
 * exceptional one - so failures are values here, the way `resource::json` reports malformed input.
 *
 * An @ref error is a code plus the little that makes the code actionable. The code alone is enough
 * to branch on; the rest is what a log line needs to be worth reading. `format_unsupported` is the
 * case that motivated the struct: knowing the device refused is useless, and knowing it refused
 * 48000/2 and offers 44100/2 tells the caller exactly what to ask for next. Nothing in here
 * allocates, so an @ref error costs the same to return as the bare enum did.
 *
 * Three codes from the old surface are gone rather than renamed, and their absence is the point:
 * `not_initialized` and `already_initialized` cannot happen now that a stream is opened by a
 * factory that returns one only on success, and `unsupported_operation` no longer covers "you
 * called `render()` on a live stream" because @ref offline_stream is a separate type. What remains
 * of `unsupported_operation` is the honest case: a backend that cannot serve a configuration it
 * otherwise understands.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/audio/backend.hpp>
#include <catalyst/audio/types.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace catalyst::audio
{

    /**
     * @enum error_code
     * @brief What went wrong.
     */
    enum class error_code : std::uint8_t
    {
        /** @brief No failure. Never carried by a returned @ref error; present so the struct has a
         * meaningful default. */
        none = 0,

        /** @brief The configuration contains values no device could honour: a zero sample rate, an
         * output stream with no output channels, a block size larger than a megaframe. */
        invalid_config,

        /** @brief The requested backend was not compiled into this build, or is not present on this
         * system. Ask @ref is_available first to distinguish the two. */
        backend_unavailable,

        /** @brief No device matched the @ref device_selector, and there is no usable default. */
        no_device,

        /** @brief The device disappeared or was invalidated while the stream was open. The stream
         * is dead; open a new one. Also published as a @ref device_lost_event. */
        device_lost,

        /** @brief The device cannot provide the requested format, and fallback was either disabled
         * or exhausted. `offered_sample_rate` and `offered_channels` say what it would accept, when
         * the backend could find out. */
        format_unsupported,

        /** @brief The device is held exclusively by another process. */
        device_busy,

        /** @brief The backend understands the configuration but cannot serve it - WASAPI asked for
         * @ref stream_direction::duplex, for instance, where render and capture are independent
         * clocks that need drift compensation this module does not yet do. */
        unsupported_operation,

        /** @brief The render thread could not be created. */
        thread_failure,

        /** @brief A file could not be opened or written. See @ref offline_stream::write_wav. */
        io_failure,

        /** @brief The platform API returned a failure with no better mapping. */
        platform_error,
    };

    /**
     * @brief A short, stable description of a code. Never empty.
     * @details A sentence fragment, lowercase, that reads correctly after "audio: " - so a caller
     * can log `"audio: {}"` and get a whole message.
     */
    [[nodiscard]] constexpr std::string_view name(error_code code) noexcept
    {
        switch (code)
        {
        case error_code::none:
            return "no error";
        case error_code::invalid_config:
            return "stream configuration is invalid";
        case error_code::backend_unavailable:
            return "requested backend is unavailable";
        case error_code::no_device:
            return "no matching audio device";
        case error_code::device_lost:
            return "audio device was lost";
        case error_code::format_unsupported:
            return "device cannot provide the requested format";
        case error_code::device_busy:
            return "device is in use by another process";
        case error_code::unsupported_operation:
            return "backend does not support this configuration";
        case error_code::thread_failure:
            return "render thread could not be started";
        case error_code::io_failure:
            return "file I/O failed";
        case error_code::platform_error:
            return "platform audio API failure";
        }
        return "unknown audio error";
    }

    /**
     * @struct error
     * @brief The failure value of every fallible operation in the module.
     * @details Compare on @ref code to branch; call @ref message to report. The context fields are
     * populated where the backend knows them and left at their defaults where it does not, so a
     * reader should treat a zero as "not known" rather than as a value.
     */
    struct error
    {
        /** @brief What went wrong. */
        error_code code = error_code::none;

        /** @brief The backend that failed, resolved - never @ref backend_kind::automatic once a
         * backend has been chosen. */
        backend_kind backend = backend_kind::automatic;

        /** @brief A rate the device would have accepted. Zero when unknown or not applicable. */
        sample_rate_t offered_sample_rate = 0;

        /** @brief A channel count the device would have accepted. Zero when unknown. */
        channel_count offered_channels = 0;

        /**
         * @brief The error as a sentence, e.g.
         * `"WASAPI: device cannot provide the requested format (device offers 44100 Hz, 2 ch)"`.
         * @return A freshly allocated string. This is the only thing in the type that allocates,
         * and only when a caller asks for it.
         */
        [[nodiscard]] std::string message() const;

        [[nodiscard]] friend bool operator==(const error &, const error &) noexcept = default;
    };

    /** @brief Builds an @ref error carrying only a code, for the failures with nothing to add. */
    [[nodiscard]] constexpr error make_error(error_code code, backend_kind backend = backend_kind::automatic) noexcept
    {
        return error{code, backend, 0, 0};
    }

} // namespace catalyst::audio

/** @brief Formats a code as its description, so `log::error("{}", err.code)` works. */
template <>
struct std::formatter<catalyst::audio::error_code> : std::formatter<std::string_view>
{
    template <typename Context>
    auto format(catalyst::audio::error_code code, Context &ctx) const
    {
        return std::formatter<std::string_view>::format(catalyst::audio::name(code), ctx);
    }
};

/**
 * @brief Formats an error as its full message, so `log::critical("{}", result.error())` works.
 * @details This is the one that call sites want: it names the backend and the context, where the
 * code on its own would not.
 */
template <>
struct std::formatter<catalyst::audio::error> : std::formatter<std::string>
{
    template <typename Context>
    auto format(const catalyst::audio::error &err, Context &ctx) const
    {
        return std::formatter<std::string>::format(err.message(), ctx);
    }
};
