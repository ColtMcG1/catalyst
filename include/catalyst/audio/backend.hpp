/**
 * @file backend.hpp
 * @brief Which platform audio APIs this build can drive, and how to ask.
 * @details A backend is the module's adapter onto one platform API - WASAPI, ASIO, and the two
 * hardware-free ones. Which are compiled in is a build-time decision, which are usable is a
 * run-time one, and both are answered here by free functions rather than by statics hanging off a
 * stream class: asking what this machine can do should not require opening a device first.
 *
 * @ref backend_kind::automatic is a request, not a backend. It resolves at open time to whatever
 * @ref default_backend returns, and never to @ref backend_kind::offline - a stream with no clock of
 * its own is never what a caller who did not name it meant.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/audio/types.hpp>

#include <cstdint>
#include <format>
#include <string_view>
#include <vector>

namespace catalyst::audio
{

    /**
     * @enum backend_kind
     * @brief The platform audio API a stream drives.
     */
    enum class backend_kind : std::uint8_t
    {
        /** @brief Not a backend: "pick the best one on this machine". See @ref default_backend. */
        automatic = 0,
        /** @brief Windows Audio Session API. The default on Windows. */
        wasapi,
        /** @brief Steinberg ASIO, through a driver the user installed. Lower latency, if present. */
        asio,
        /** @brief Advanced Linux Sound Architecture. Not yet implemented. */
        alsa,
        /** @brief Apple CoreAudio. Not yet implemented. */
        coreaudio,
        /**
         * @brief No hardware, no threads, and a clock the caller turns by hand.
         * @details Reached through @ref offline_stream rather than @ref stream, because a stream
         * the caller advances and a stream a device advances are not the same object. This is what
         * makes the audio path testable in CI on a machine with no sound card.
         */
        offline,
        /** @brief Accepts everything and produces nothing. The fallback where no API exists. */
        null,
    };

    /** @brief Number of values in @ref backend_kind, including `automatic`. */
    inline constexpr std::size_t backend_kind_count = 7;

    /**
     * @brief The display name of a backend: "WASAPI", "ASIO", "offline", ...
     * @details Capitalised the way the vendor capitalises it, because this ends up in logs and in
     * device pickers that a person reads.
     */
    [[nodiscard]] constexpr std::string_view name(backend_kind backend) noexcept
    {
        switch (backend)
        {
        case backend_kind::automatic:
            return "automatic";
        case backend_kind::wasapi:
            return "WASAPI";
        case backend_kind::asio:
            return "ASIO";
        case backend_kind::alsa:
            return "ALSA";
        case backend_kind::coreaudio:
            return "CoreAudio";
        case backend_kind::offline:
            return "offline";
        case backend_kind::null:
            return "null";
        }
        return "unknown";
    }

    /**
     * @brief Whether @p backend was compiled into this build and can be constructed here.
     * @param backend The backend to ask about.
     * @return True if a stream could use it. `automatic` is available whenever anything is, which
     * is always: @ref backend_kind::null is compiled unconditionally.
     * @note This answers "is the API present", not "is a device plugged in". A machine with WASAPI
     * and no sound card reports `true` here and fails to open with @ref error_code::no_device.
     */
    [[nodiscard]] bool is_available(backend_kind backend) noexcept;

    /**
     * @brief Every usable backend, best first.
     * @return A fresh vector. `automatic` is never listed - it is a request, not a backend - and
     * `offline` and `null` are always last, in that order, because neither is a device.
     */
    [[nodiscard]] std::vector<backend_kind> available_backends();

    /**
     * @brief What @ref backend_kind::automatic resolves to on this machine.
     * @return The best backend that drives real hardware, or @ref backend_kind::null where none
     * does. Never `automatic`, and never `offline`: a stream with no clock of its own is never what
     * a caller who did not name it meant.
     */
    [[nodiscard]] backend_kind default_backend() noexcept;

} // namespace catalyst::audio

/** @brief Formats a backend as its name, so `log::info("Backend: {}", backend)` works. */
template <>
struct std::formatter<catalyst::audio::backend_kind> : std::formatter<std::string_view>
{
    template <typename Context>
    auto format(catalyst::audio::backend_kind backend, Context &ctx) const
    {
        return std::formatter<std::string_view>::format(catalyst::audio::name(backend), ctx);
    }
};
