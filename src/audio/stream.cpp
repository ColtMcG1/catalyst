/**
 * @file stream.cpp
 * @brief The live stream: backend selection, the open/start/stop lifecycle, and the translation of
 * queued device notices into bus events.
 * @details All device and format specifics live in the backends. This layer decides which backend
 * to build, enforces the small state machine that is left once "initialized" stopped being a state,
 * and owns the hand-off from the platform's notification thread to the caller's.
 * License: MIT (see LICENSE).
 */

#include "detail_backend.hpp"

#include <catalyst/audio/events.hpp>
#include <catalyst/audio/stream.hpp>
#include <catalyst/events/bus.hpp>

#include <utility>

namespace catalyst::audio
{

    /**
     * @struct stream::impl
     * @brief Everything a stream owns. Held behind a pointer so moving a stream never disturbs the
     * address the render thread reads its state from.
     */
    struct stream::impl
    {
        std::unique_ptr<detail::backend> backend;
        detail::notice_queue notices;
        events::bus *bus = nullptr;
        stream_info info;
        bool running = false;
        bool start_pending = false;
        bool stop_pending = false;
    };

    namespace
    {

        /// Fills the fields every audio event carries.
        template <typename Event>
        Event make_event(backend_kind backend, audio_time time)
        {
            Event event;
            event.backend = backend;
            event.time = time;
            return event;
        }

    } // namespace

    std::expected<stream, error> stream::open(const stream_config &config, renderer render)
    {
        detail::open_request request;
        request.backend = detail::resolve(config.backend);
        request.device = config.device;
        request.direction = config.direction;
        request.sample_rate = config.sample_rate;
        request.output_channels = has_output(config.direction) ? config.output_channels : 0;
        request.input_channels = has_input(config.direction) ? config.input_channels : 0;
        request.block_frames = config.block_frames;
        request.exclusive = config.exclusive;
        request.allow_format_fallback = config.allow_format_fallback;
        request.render = render;

        if (const auto valid = detail::validate(request); !valid)
            return std::unexpected(valid.error());

        auto state = std::make_unique<impl>();
        state->bus = config.bus;
        request.notices = &state->notices;

        auto created = detail::create_backend(request);
        if (!created)
            return std::unexpected(created.error());

        state->backend = std::move(*created);

        if (const auto opened = state->backend->open(); !opened)
        {
            // Leave no half-open device behind: the caller gets an error, not an object.
            state->backend->close();
            return std::unexpected(opened.error());
        }

        state->info = state->backend->info();

        return stream{std::move(state)};
    }

    stream::stream(std::unique_ptr<impl> state) noexcept : impl_(std::move(state)) {}

    stream::stream(stream &&) noexcept = default;
    stream &stream::operator=(stream &&) noexcept = default;

    stream::~stream()
    {
        if (!impl_ || !impl_->backend)
            return;

        impl_->backend->stop();
        impl_->backend->close();
    }

    std::expected<void, error> stream::start()
    {
        if (!impl_ || !impl_->backend)
            return std::unexpected(make_error(error_code::device_lost));

        if (impl_->running)
            return {};

        if (const auto started = impl_->backend->start(); !started)
            return std::unexpected(started.error());

        impl_->running = true;
        impl_->start_pending = true;
        return {};
    }

    void stream::stop() noexcept
    {
        if (!impl_ || !impl_->backend || !impl_->running)
            return;

        impl_->backend->stop();
        impl_->running = false;
        impl_->stop_pending = true;
    }

    bool stream::is_running() const noexcept
    {
        return impl_ && impl_->running && impl_->backend && impl_->backend->is_running();
    }

    void stream::pump()
    {
        if (!impl_ || !impl_->backend)
            return;

        // The failure has to be taken whether or not there is a bus, so that a stream which died on
        // its own thread stops reporting itself as running either way.
        const std::optional<error> failure = impl_->backend->take_failure();
        if (failure)
            impl_->running = false;

        auto notices = impl_->notices.drain();
        const std::uint64_t xruns = impl_->backend->take_xruns();

        events::bus *bus = impl_->bus;
        if (!bus)
        {
            // Nothing to publish to, so nothing stays owed. Without this the pending flags would
            // sit set for the life of a stream that never had a bus.
            impl_->start_pending = false;
            impl_->stop_pending = false;
            return;
        }

        const backend_kind backend = impl_->info.backend;

        if (impl_->start_pending)
        {
            impl_->start_pending = false;
            auto event = make_event<stream_started_event>(backend, audio_clock::now());
            event.sample_rate = impl_->info.sample_rate;
            event.output_channels = impl_->info.output_channels;
            event.input_channels = impl_->info.input_channels;
            bus->dispatch(std::move(event));
        }

        for (detail::device_notice &notice : notices)
        {
            switch (notice.what)
            {
            case detail::device_notice::kind::added:
            {
                auto event = make_event<device_added_event>(backend, notice.time);
                event.device_id = std::move(notice.device_id);
                bus->dispatch(std::move(event));
                break;
            }
            case detail::device_notice::kind::removed:
            {
                auto event = make_event<device_removed_event>(backend, notice.time);
                event.device_id = std::move(notice.device_id);
                bus->dispatch(std::move(event));
                break;
            }
            case detail::device_notice::kind::default_changed:
            {
                auto event = make_event<default_device_changed_event>(backend, notice.time);
                event.device_id = std::move(notice.device_id);
                event.direction = notice.direction;
                bus->dispatch(std::move(event));
                break;
            }
            case detail::device_notice::kind::lost:
            {
                auto event = make_event<device_lost_event>(backend, notice.time);
                event.device_id = std::move(notice.device_id);
                bus->dispatch(std::move(event));
                break;
            }
            }
        }

        if (xruns != 0)
        {
            auto event = make_event<xrun_event>(backend, audio_clock::now());
            event.count = xruns;
            event.total = impl_->backend->stats().xruns;
            bus->dispatch(std::move(event));
        }

        if (failure)
        {
            auto event = make_event<stream_failed_event>(backend, audio_clock::now());
            event.error = *failure;
            bus->dispatch(std::move(event));
            impl_->stop_pending = true;
        }

        if (impl_->stop_pending)
        {
            impl_->stop_pending = false;
            auto event = make_event<stream_stopped_event>(backend, audio_clock::now());
            event.frames_rendered = impl_->backend->stats().frames_rendered;
            bus->dispatch(std::move(event));
        }
    }

    const stream_info &stream::info() const noexcept
    {
        // A moved-from stream has no impl. Returning a shared empty is kinder than dereferencing
        // null, and matches the other accessors, which all answer rather than crash.
        static const stream_info empty{};
        return impl_ ? impl_->info : empty;
    }

    stream_stats stream::stats() const noexcept
    {
        if (!impl_ || !impl_->backend)
            return {};

        return impl_->backend->stats();
    }

    void stream::reset_stats() noexcept
    {
        if (impl_ && impl_->backend)
            impl_->backend->reset_stats();
    }

    backend_kind stream::backend() const noexcept
    {
        return impl_ ? impl_->info.backend : backend_kind::automatic;
    }

    std::string_view stream::backend_name() const noexcept
    {
        return name(backend());
    }

} // namespace catalyst::audio
