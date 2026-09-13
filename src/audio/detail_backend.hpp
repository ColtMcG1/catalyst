/**
 * @file detail_backend.hpp
 * @brief The internal interface every live audio backend implements, the request it is opened
 * with, and the queue it reports device changes through.
 * @details Backends are selected at build time by CMake, which defines the `CATALYST_AUDIO_HAS_*`
 * macros; the null backend is always compiled so the module builds and links on every platform.
 * The offline renderer is deliberately *not* a backend - it owns no device and no thread, so it is
 * implemented directly in offline.cpp rather than pretending to be hardware.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/audio/block.hpp>
#include <catalyst/audio/device.hpp>
#include <catalyst/audio/error.hpp>
#include <catalyst/audio/stream.hpp>
#include <catalyst/audio/types.hpp>

#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace catalyst::audio::detail
{

    /**
     * @struct device_notice
     * @brief One device-topology change a backend observed, on its way to the caller's bus.
     * @details Backends push these from whichever thread the platform notified them on;
     * `stream::pump()` turns them into the events in `<catalyst/audio/events.hpp>` on the caller's
     * thread. Keeping the two apart is what lets a listener be an ordinary function.
     */
    struct device_notice
    {
        enum class kind : std::uint8_t
        {
            added,
            removed,
            default_changed,
            lost,
        };

        kind what = kind::added;
        std::string device_id;
        stream_direction direction = stream_direction::output;
        audio_time time{};
    };

    /**
     * @class notice_queue
     * @brief A small, bounded, thread-safe hand-off from a platform notification thread to the
     * thread that calls `pump()`.
     * @details Bounded and oldest-dropping on purpose: a program that never pumps must not grow a
     * queue, and the newest topology state is the one worth keeping. Not lock-free, because nothing
     * that touches it is real-time - the render thread never comes near it.
     */
    class notice_queue
    {
    public:
        /** @brief Any thread. Drops the oldest entry when full, and drops this one on OOM. */
        void push(device_notice notice) noexcept
        {
            try
            {
                const std::lock_guard lock(mutex_);
                if (items_.size() >= capacity)
                    items_.pop_front();
                items_.push_back(std::move(notice));
            }
            catch (...)
            {
                // A dropped notice costs the caller one event. Throwing out of a driver's
                // notification callback would cost it the process.
            }
        }

        /** @brief The caller's thread. Takes everything queued and leaves the queue empty. */
        [[nodiscard]] std::deque<device_notice> drain()
        {
            std::deque<device_notice> out;
            {
                const std::lock_guard lock(mutex_);
                out.swap(items_);
            }
            return out;
        }

    private:
        static constexpr std::size_t capacity = 64;

        mutable std::mutex mutex_;
        std::deque<device_notice> items_;
    };

    /**
     * @struct open_request
     * @brief What a backend is asked to open: `stream_config` with the caller's bus replaced by the
     * queue it should report through, and the renderer resolved.
     * @details `backend` is already resolved here - never `automatic` - so no backend has to know
     * how selection works.
     */
    struct open_request
    {
        backend_kind backend = backend_kind::automatic;
        device_selector device{};
        stream_direction direction = stream_direction::output;
        sample_rate_t sample_rate = 48000;
        channel_count output_channels = 2;
        channel_count input_channels = 0;
        std::uint32_t block_frames = 512;
        bool exclusive = false;
        bool allow_format_fallback = true;

        renderer render{};
        notice_queue *notices = nullptr;
    };

    /**
     * @class backend
     * @brief One platform audio API, opened against one device.
     * @details The lifecycle is open -> start -> stop -> close, and `stream` is the only caller, so
     * a backend may assume the calls arrive in that order and from one thread. Everything a backend
     * discovers on its own thread - a lost device, a failed block - reaches the caller through
     * `take_failure()` and the notice queue, never by calling back into the stream.
     */
    class backend
    {
    public:
        virtual ~backend() = default;

        /** @brief Which backend this is. Resolved, never `automatic`. */
        [[nodiscard]] virtual backend_kind kind() const noexcept = 0;

        /** @brief Every endpoint this API can see. Valid before `open()`. */
        [[nodiscard]] virtual std::expected<std::vector<device_info>, error> enumerate_devices() const = 0;

        /** @brief Selects a device and negotiates a format. The stream is silent afterwards. */
        virtual std::expected<void, error> open() = 0;

        /** @brief Starts the render thread. */
        virtual std::expected<void, error> start() = 0;

        /** @brief Stops the render thread and joins it. Safe when already stopped. */
        virtual void stop() noexcept = 0;

        /** @brief Releases the device. Safe when never opened. */
        virtual void close() noexcept = 0;

        [[nodiscard]] virtual bool is_running() const noexcept = 0;

        [[nodiscard]] virtual stream_info info() const = 0;
        [[nodiscard]] virtual stream_stats stats() const noexcept = 0;
        virtual void reset_stats() noexcept = 0;

        /**
         * @brief The failure the render thread found, if any, clearing it.
         * @details How a stream that died on its own thread reports what happened. `stream::pump()`
         * takes it and publishes a `stream_failed_event`.
         */
        [[nodiscard]] virtual std::optional<error> take_failure() noexcept { return std::nullopt; }

        /** @brief Xruns since the last call to this, for coalescing into one `xrun_event`. */
        [[nodiscard]] virtual std::uint64_t take_xruns() noexcept { return 0; }
    };

    /** @brief What `backend_kind::automatic` resolves to. Never `automatic`, never `offline`. */
    [[nodiscard]] backend_kind resolve(backend_kind requested) noexcept;

    /** @brief Rejects configurations no device could honour, so each backend need not re-check. */
    [[nodiscard]] std::expected<void, error> validate(const open_request &request) noexcept;

    /** @brief Constructs the backend named by `request.backend`, unopened. */
    [[nodiscard]] std::expected<std::unique_ptr<backend>, error> create_backend(const open_request &request);

    /** @brief Constructs a backend purely to enumerate with; nothing is opened. */
    [[nodiscard]] std::expected<std::unique_ptr<backend>, error> create_enumerator(backend_kind kind);

    // Per-backend factories. The null backend is always available.
    std::unique_ptr<backend> create_null_backend(const open_request &request);

#if defined(CATALYST_AUDIO_HAS_WASAPI)
    std::unique_ptr<backend> create_wasapi_backend_win32(const open_request &request) noexcept;
#endif

#if defined(CATALYST_AUDIO_HAS_ASIO)
    std::unique_ptr<backend> create_asio_backend_win32(const open_request &request) noexcept;
#endif

} // namespace catalyst::audio::detail
