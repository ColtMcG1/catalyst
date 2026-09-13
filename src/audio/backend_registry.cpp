/**
 * @file backend_registry.cpp
 * @brief Which backends this build contains, which one `automatic` means, and the construction of
 * the chosen one. Also the configuration validation every backend would otherwise repeat.
 * @details This is the single definition of `create_backend` in the module; platform backends only
 * expose their own factory, so adding or removing one from the build can never produce duplicate or
 * missing symbols.
 * License: MIT (see LICENSE).
 */

#include "detail_backend.hpp"

#include <catalyst/audio/backend.hpp>

namespace catalyst::audio
{

    namespace
    {

        // Guards against configurations no device could satisfy, so backends do not each re-derive
        // the same sanity checks.
        constexpr sample_rate_t min_sample_rate = 4000;
        constexpr sample_rate_t max_sample_rate = 768000;
        constexpr channel_count max_channels = 64;
        constexpr std::uint32_t max_block_frames = 1u << 20;

        constexpr bool compiled_in(backend_kind kind) noexcept
        {
            switch (kind)
            {
            case backend_kind::automatic:
            case backend_kind::offline:
            case backend_kind::null:
                // Offline and null need no platform API, so `automatic` always resolves.
                return true;

            case backend_kind::wasapi:
#if defined(CATALYST_AUDIO_HAS_WASAPI)
                return true;
#else
                return false;
#endif

            case backend_kind::asio:
#if defined(CATALYST_AUDIO_HAS_ASIO)
                return true;
#else
                return false;
#endif

            case backend_kind::alsa:
            case backend_kind::coreaudio:
                return false;
            }
            return false;
        }

    } // namespace

    bool is_available(backend_kind backend) noexcept
    {
        return compiled_in(backend);
    }

    backend_kind default_backend() noexcept
    {
        // Never `offline`: a stream the caller has to advance by hand is not what someone who
        // asked for "the best available" was after.
#if defined(CATALYST_AUDIO_HAS_WASAPI)
        return backend_kind::wasapi;
#elif defined(CATALYST_AUDIO_HAS_ASIO)
        return backend_kind::asio;
#else
        return backend_kind::null;
#endif
    }

    std::vector<backend_kind> available_backends()
    {
        std::vector<backend_kind> out;
        out.reserve(4);

#if defined(CATALYST_AUDIO_HAS_WASAPI)
        out.push_back(backend_kind::wasapi);
#endif
#if defined(CATALYST_AUDIO_HAS_ASIO)
        out.push_back(backend_kind::asio);
#endif
        out.push_back(backend_kind::offline);
        out.push_back(backend_kind::null);

        return out;
    }

} // namespace catalyst::audio

namespace catalyst::audio::detail
{

    backend_kind resolve(backend_kind requested) noexcept
    {
        return requested == backend_kind::automatic ? default_backend() : requested;
    }

    std::expected<void, error> validate(const open_request &request) noexcept
    {
        const auto invalid = [&] { return std::unexpected(make_error(error_code::invalid_config, request.backend)); };

        if (request.sample_rate < min_sample_rate || request.sample_rate > max_sample_rate)
            return invalid();

        if (request.block_frames > max_block_frames)
            return invalid();

        if (request.output_channels > max_channels || request.input_channels > max_channels)
            return invalid();

        if (has_output(request.direction) && request.output_channels == 0)
            return invalid();

        if (has_input(request.direction) && request.input_channels == 0)
            return invalid();

        return {};
    }

    std::expected<std::unique_ptr<backend>, error> create_backend(const open_request &request)
    {
        std::unique_ptr<backend> created;

        switch (request.backend)
        {
        case backend_kind::null:
            created = create_null_backend(request);
            break;

#if defined(CATALYST_AUDIO_HAS_WASAPI)
        case backend_kind::wasapi:
            created = create_wasapi_backend_win32(request);
            break;
#endif

#if defined(CATALYST_AUDIO_HAS_ASIO)
        case backend_kind::asio:
            created = create_asio_backend_win32(request);
            break;
#endif

        default:
            // Includes `offline`, which is `offline_stream` rather than a device, and every
            // backend this build does not contain.
            return std::unexpected(make_error(error_code::backend_unavailable, request.backend));
        }

        if (!created)
            return std::unexpected(make_error(error_code::backend_unavailable, request.backend));

        return created;
    }

    std::expected<std::unique_ptr<backend>, error> create_enumerator(backend_kind kind)
    {
        open_request request;
        request.backend = resolve(kind);
        return create_backend(request);
    }

} // namespace catalyst::audio::detail
