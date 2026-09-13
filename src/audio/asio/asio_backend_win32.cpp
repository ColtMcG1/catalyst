/**
 * @file asio_backend_win32.cpp
 * @brief ASIO output/input/duplex backend for Windows, driven through the SDK-free loader in
 * `asio_loader.h`.
 * @details What is ASIO's own is here: drivers identified by CLSID rather than by a display name
 * they can be renamed out of, a negotiated rate and buffer size read back from the driver rather
 * than assumed, planar per-channel buffers that have to be interleaved on the way in and out, and
 * callbacks that carry no user pointer and so have to find their backend through a global.
 *
 * What is not ASIO's own it does not contain. Stats, failure reporting, device-change notices and
 * the render dispatcher come from `backend_base`; sample conversion is the module's shared table in
 * `detail_convert.hpp`, which is also what WASAPI now uses; COM apartment and interface ownership
 * are the shared `win32::com_apartment` and `win32::com_ptr`.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../detail_backend.hpp"

#if defined(_WIN32)

#include "../detail_backend_base.hpp"
#include "../detail_convert.hpp"
#include "../detail_render.hpp"
#include "../win32/detail_win32.hpp"

#include "asio_loader.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace catalyst::audio::detail
{

    namespace
    {
        using win32::com_apartment;
        using win32::guid_to_string;
        using win32::wide_to_utf8;

        /** @brief A driver's sample type, split into the two things a conversion needs. */
        struct device_layout
        {
            sample_format format = sample_format::unknown;
            std::endian order = std::endian::native;
        };

        /**
         * @brief What one of Steinberg's sample types means to the shared converters.
         * @details The `int32_lsb16` family - fewer valid bits right-justified inside a 32-bit
         * container - is deliberately absent: converting it as a plain `int32` would write
         * full-scale values into a field that saturates at a fraction of that, so a stream that
         * negotiates one is refused rather than made to clip.
         */
        [[nodiscard]] constexpr device_layout layout_of(asio::asio_sample_type type) noexcept
        {
            using t = asio::asio_sample_type;
            switch (type)
            {
            case t::int16_lsb:
                return {sample_format::int16, std::endian::little};
            case t::int16_msb:
                return {sample_format::int16, std::endian::big};
            case t::int24_lsb:
                return {sample_format::int24, std::endian::little};
            case t::int24_msb:
                return {sample_format::int24, std::endian::big};
            case t::int32_lsb:
                return {sample_format::int32, std::endian::little};
            case t::int32_msb:
                return {sample_format::int32, std::endian::big};
            case t::float32_lsb:
                return {sample_format::float32, std::endian::little};
            case t::float32_msb:
                return {sample_format::float32, std::endian::big};
            case t::float64_lsb:
                return {sample_format::float64, std::endian::little};
            case t::float64_msb:
                return {sample_format::float64, std::endian::big};
            default:
                return {};
            }
        }

        /** @brief How `enumerate_devices` and `open()` both describe a driver, so they agree. */
        [[nodiscard]] device_info describe(const asio::installed_driver &installed)
        {
            device_info info;
            info.backend = backend_kind::asio;
            // The CLSID is stable across driver renames and unique per driver; the name is neither.
            info.id = guid_to_string(installed.clsid);
            info.name = wide_to_utf8(installed.name);
            return info;
        }

        class asio_backend_win32;

        /// ASIO callbacks carry no user pointer, so the active instance must be reachable from a
        /// global. Claiming it with a compare-exchange means a second engine fails loudly with
        /// `device_busy` instead of silently stealing the first one's callbacks.
        std::atomic<asio_backend_win32 *> g_active_backend{nullptr};

        class asio_backend_win32 final : public backend_base
        {
        public:
            explicit asio_backend_win32(open_request request)
                : backend_base(backend_kind::asio, std::move(request))
            {
            }

            ~asio_backend_win32() override { close(); }

            [[nodiscard]] std::expected<std::vector<device_info>, error> enumerate_devices() const override
            {
                const auto drivers = asio::enumerate_installed_drivers();

                std::vector<device_info> devices;
                devices.reserve(drivers.size());

                for (const auto &installed : drivers)
                {
                    device_info info = describe(installed);
                    // ASIO has no notion of a system default, so the first installed driver is it.
                    info.is_default = devices.empty();
                    devices.push_back(std::move(info));
                }

                return devices;
            }

            std::expected<void, error> open() override
            {
                close();

                // Not for the driver interface, which is reached without the COM runtime, but for
                // the drivers themselves: most call into OLE from their own control panels and
                // worker threads, and expect the host to have initialized the apartment.
                apartment_ = com_apartment::enter();
                if (!apartment_.usable())
                    return failure(error_code::platform_error);

                // Only one ASIO stream can own the global callback slot.
                asio_backend_win32 *unclaimed = nullptr;
                if (!g_active_backend.compare_exchange_strong(unclaimed, this))
                    return failure(error_code::device_busy);
                claimed_ = true;

                if (const auto opened = open_driver(); !opened)
                    return std::unexpected(opened.error());

                if (const auto configured = configure_stream(); !configured)
                    return std::unexpected(configured.error());

                mark_opened();
                return {};
            }

            std::expected<void, error> start() override
            {
                if (!opened_ || !driver_.is_open() || running_.load(std::memory_order_acquire))
                    return {};

                clear_failure();

                // Fill both halves of the double buffer before the driver's clock starts.
                render_half(0);
                render_half(1);

                if (driver_->start() != 0)
                    return failure(error_code::platform_error);

                running_.store(true, std::memory_order_release);
                return {};
            }

            void stop() noexcept override
            {
                if (!running_.exchange(false, std::memory_order_acq_rel))
                    return;

                if (driver_.is_open())
                    (void)driver_->stop();
            }

            void close() noexcept override
            {
                stop();

                // Stop receiving callbacks before the buffers they read are released.
                if (claimed_)
                {
                    auto *self = this;
                    (void)g_active_backend.compare_exchange_strong(self, nullptr);
                    claimed_ = false;
                }

                if (driver_.is_open())
                    (void)driver_->dispose_buffers();

                driver_.close();

                buffer_infos_.clear();
                output_converters_.clear();
                input_converters_.clear();
                output_interleaved_.clear();
                input_interleaved_.clear();

                buffer_frames_ = 0;
                output_channels_ = 0;
                input_channels_ = 0;
                output_latency_frames_ = 0;
                input_latency_frames_ = 0;

                mark_closed();
                apartment_.leave();
            }

            [[nodiscard]] stream_info info() const override
            {
                const auto rate = this->rate();

                stream_info out = base_info();
                out.output_channels = static_cast<channel_count>(output_channels_);
                out.input_channels = static_cast<channel_count>(input_channels_);
                out.output_layout = layout_for(static_cast<channel_count>(output_channels_));
                out.block_frames = static_cast<std::uint32_t>(buffer_frames_);

                out.output_latency =
                    frames_to_time(static_cast<frame_count>(output_latency_frames_), rate);
                out.input_latency =
                    frames_to_time(static_cast<frame_count>(input_latency_frames_), rate);

                // ASIO always owns the device outright.
                out.exclusive = true;
                return out;
            }

        private:
            std::expected<void, error> open_driver()
            {
                const auto drivers = asio::enumerate_installed_drivers();
                if (drivers.empty())
                    return failure(error_code::no_device);

                const asio::installed_driver *selected = nullptr;

                if (request_.device.by == device_selector::match::system_default)
                {
                    selected = &drivers.front();
                }
                else
                {
                    // The selector decides how to match; `describe` only has to present each driver
                    // exactly as `enumerate_devices` did, so both agree on what a selector picks.
                    for (const auto &installed : drivers)
                    {
                        if (request_.device.matches(describe(installed)))
                        {
                            selected = &installed;
                            break;
                        }
                    }

                    // An explicit request that cannot be honoured is an error, not a silent
                    // downgrade to some other driver.
                    if (!selected)
                        return failure(error_code::no_device);
                }

                if (const auto loaded = driver_.open(*selected); !loaded)
                    return failure(loaded.error());

                // ASIO wants a platform system handle; a desktop window works for most drivers.
                if (driver_->init(GetDesktopWindow()) == 0)
                    return failure(error_code::platform_error);

                identify(guid_to_string(selected->clsid), wide_to_utf8(selected->name));
                return {};
            }

            std::expected<void, error> configure_stream()
            {
                // Sample rate. `can_sample_rate` returns ASE_OK (0) when the rate is available.
                const auto wanted = static_cast<asio::asio_sample_rate>(request_.sample_rate);
                if (request_.sample_rate != 0 && driver_->can_sample_rate(wanted) == 0)
                    (void)driver_->set_sample_rate(wanted);

                // Read back what the driver settled on rather than assuming the request stuck.
                asio::asio_sample_rate actual = 0.0;
                if (driver_->get_sample_rate(&actual) != 0 || actual <= 0.0)
                    return failure(error_code::platform_error);

                set_rate(static_cast<sample_rate_t>(actual + 0.5));

                if (!request_.allow_format_fallback && rate() != request_.sample_rate)
                {
                    error refused = make_error(error_code::format_unsupported, backend_kind::asio);
                    refused.offered_sample_rate = rate();
                    return std::unexpected(refused);
                }

                std::int32_t available_inputs = 0;
                std::int32_t available_outputs = 0;
                if (driver_->get_channels(&available_inputs, &available_outputs) != 0)
                    return failure(error_code::platform_error);

                const bool wants_output = has_output(request_.direction);
                const bool wants_input = has_input(request_.direction);

                output_channels_ =
                    wants_output
                        ? std::min<std::int32_t>(
                              static_cast<std::int32_t>(request_.output_channels), available_outputs)
                        : 0;
                input_channels_ =
                    wants_input
                        ? std::min<std::int32_t>(
                              static_cast<std::int32_t>(request_.input_channels), available_inputs)
                        : 0;

                if (output_channels_ <= 0 && input_channels_ <= 0)
                    return failure(error_code::no_device);

                if (!request_.allow_format_fallback &&
                    ((wants_output && output_channels_ != static_cast<std::int32_t>(request_.output_channels)) ||
                     (wants_input && input_channels_ != static_cast<std::int32_t>(request_.input_channels))))
                {
                    error refused = make_error(error_code::format_unsupported, backend_kind::asio);
                    refused.offered_channels =
                        static_cast<channel_count>(wants_output ? output_channels_ : input_channels_);
                    return std::unexpected(refused);
                }

                // Buffer size, clamped to the driver's advertised range.
                std::int32_t minimum = 0;
                std::int32_t maximum = 0;
                std::int32_t preferred = 0;
                std::int32_t granularity = 0;
                if (driver_->get_buffer_size(&minimum, &maximum, &preferred, &granularity) != 0)
                    return failure(error_code::platform_error);

                auto requested = static_cast<std::int32_t>(request_.block_frames);
                if (requested <= 0)
                    requested = preferred;

                requested = std::clamp(requested, minimum, maximum);
                if (requested <= 0)
                    return failure(error_code::platform_error);

                buffer_frames_ = requested;

                if (const auto prepared = create_buffers(); !prepared)
                    return std::unexpected(prepared.error());

                std::int32_t input_latency = 0;
                std::int32_t output_latency = 0;
                if (driver_->get_latencies(&input_latency, &output_latency) == 0)
                {
                    input_latency_frames_ = input_latency;
                    output_latency_frames_ = output_latency;
                }

                return {};
            }

            /// Resolves every channel's conversion before the driver can call back, so the
            /// real-time path is an indirect call rather than a sample-type comparison per channel.
            template <typename Fn, typename Pick>
            std::expected<void, error> resolve_converters(
                std::vector<Fn> &converters, std::int32_t channels, bool input, Pick pick)
            {
                converters.assign(static_cast<std::size_t>(channels), nullptr);

                for (std::int32_t channel = 0; channel < channels; ++channel)
                {
                    asio::asio_channel_info info{};
                    info.channel = channel;
                    info.is_input = input ? 1 : 0;
                    if (driver_->get_channel_info(&info) != 0)
                        return failure(error_code::platform_error);

                    const device_layout layout = layout_of(info.sample_type);
                    const Fn converter = pick(layout.format, layout.order);
                    if (!converter)
                        return failure(error_code::format_unsupported);

                    converters[static_cast<std::size_t>(channel)] = converter;
                }

                return {};
            }

            std::expected<void, error> create_buffers()
            {
                const auto total = static_cast<std::size_t>(output_channels_) +
                                   static_cast<std::size_t>(input_channels_);

                buffer_infos_.assign(total, asio::asio_buffer_info{});

                // Outputs occupy [0, output_channels_), inputs follow.
                for (std::int32_t channel = 0; channel < output_channels_; ++channel)
                {
                    auto &info = buffer_infos_[static_cast<std::size_t>(channel)];
                    info.is_input = 0;
                    info.channel_num = channel;
                }

                for (std::int32_t channel = 0; channel < input_channels_; ++channel)
                {
                    auto &info = buffer_infos_[static_cast<std::size_t>(output_channels_ + channel)];
                    info.is_input = 1;
                    info.channel_num = channel;
                }

                if (const auto resolved = resolve_converters(
                        output_converters_, output_channels_, false, &pack_for);
                    !resolved)
                {
                    return std::unexpected(resolved.error());
                }

                if (const auto resolved = resolve_converters(
                        input_converters_, input_channels_, true, &unpack_for);
                    !resolved)
                {
                    return std::unexpected(resolved.error());
                }

                output_interleaved_.assign(
                    static_cast<std::size_t>(buffer_frames_) * static_cast<std::size_t>(output_channels_), 0.0f);
                input_interleaved_.assign(
                    static_cast<std::size_t>(buffer_frames_) * static_cast<std::size_t>(input_channels_), 0.0f);

                callbacks_ = {};
                callbacks_.buffer_switch = &asio_backend_win32::on_buffer_switch;
                callbacks_.sample_rate_did_change = &asio_backend_win32::on_sample_rate_changed;
                callbacks_.asio_message = &asio_backend_win32::on_message;
                callbacks_.buffer_switch_time_info = &asio_backend_win32::on_buffer_switch_time_info;

                if (driver_->create_buffers(
                        buffer_infos_.data(),
                        static_cast<std::int32_t>(total),
                        buffer_frames_,
                        &callbacks_) != 0)
                {
                    return failure(error_code::platform_error);
                }

                return {};
            }

            /// Real-time path. Interleaves the input, runs the callback, deinterleaves the output.
            void render_half(std::int32_t half) noexcept
            {
                if (buffer_frames_ <= 0 || half < 0 || half > 1)
                    return;

                const auto frames = static_cast<std::size_t>(buffer_frames_);

                for (std::int32_t channel = 0; channel < input_channels_; ++channel)
                {
                    const auto &info = buffer_infos_[static_cast<std::size_t>(output_channels_ + channel)];
                    const auto *source = static_cast<const std::byte *>(info.buffers[half]);
                    if (!source)
                        continue;

                    input_converters_[static_cast<std::size_t>(channel)](
                        input_interleaved_.data() + channel,
                        source,
                        frames,
                        static_cast<std::size_t>(input_channels_));
                }

                dispatcher_.dispatch(
                    std::span<sample>(output_interleaved_),
                    std::span<const sample>(input_interleaved_),
                    static_cast<std::uint32_t>(buffer_frames_),
                    static_cast<channel_count>(output_channels_),
                    static_cast<channel_count>(input_channels_),
                    rate());

                for (std::int32_t channel = 0; channel < output_channels_; ++channel)
                {
                    const auto &info = buffer_infos_[static_cast<std::size_t>(channel)];
                    auto *destination = static_cast<std::byte *>(info.buffers[half]);
                    if (!destination)
                        continue;

                    output_converters_[static_cast<std::size_t>(channel)](
                        destination,
                        output_interleaved_.data() + channel,
                        frames,
                        static_cast<std::size_t>(output_channels_));
                }
            }

            static void on_buffer_switch(std::int32_t half, std::int32_t direct_process) noexcept
            {
                (void)direct_process;

                auto *self = g_active_backend.load(std::memory_order_acquire);
                if (!self)
                    return;

                self->render_half(half);

                if (self->driver_.is_open())
                    (void)self->driver_->output_ready();
            }

            static void on_sample_rate_changed(asio::asio_sample_rate sample_rate) noexcept
            {
                auto *self = g_active_backend.load(std::memory_order_acquire);
                if (!self || sample_rate <= 0.0)
                    return;

                // The renderer is handed the new rate from the next block onwards, and `info()`
                // reports it, so a caller that watches either sees what it is actually running at.
                self->set_rate(static_cast<sample_rate_t>(sample_rate + 0.5));

                if (const auto &publisher = self->publisher())
                    publisher->note();
            }

            static std::int32_t on_message(
                std::int32_t selector, std::int32_t value, void *message, double *opt) noexcept
            {
                (void)value;
                (void)message;
                (void)opt;

                using selector_kind = asio::asio_message_selector;

                switch (static_cast<selector_kind>(selector))
                {
                case selector_kind::selector_supported:
                    return 1;

                case selector_kind::engine_version:
                    return 2;

                case selector_kind::reset_request:
                case selector_kind::reset_needed:
                    // The driver is telling us this stream is over and must be reopened. Saying so
                    // is the point: the stream used to keep running against a driver that had
                    // already given up, producing silence with no way for the caller to find out.
                    if (auto *self = g_active_backend.load(std::memory_order_acquire))
                        self->fail_stream(error_code::device_lost);
                    return 1;

                case selector_kind::latencies_changed:
                case selector_kind::buffer_size_change:
                    if (auto *self = g_active_backend.load(std::memory_order_acquire))
                    {
                        if (const auto &publisher = self->publisher())
                            publisher->note();
                    }
                    return 0;
                }

                return 0;
            }

            static asio::asio_time *on_buffer_switch_time_info(
                asio::asio_time *params, std::int32_t half, std::int32_t direct_process) noexcept
            {
                on_buffer_switch(half, direct_process);
                return params;
            }

            com_apartment apartment_;
            asio::driver driver_;
            asio::asio_callbacks callbacks_{};

            bool claimed_ = false;

            std::int32_t buffer_frames_ = 0;
            std::int32_t output_channels_ = 0;
            std::int32_t input_channels_ = 0;
            std::int32_t output_latency_frames_ = 0;
            std::int32_t input_latency_frames_ = 0;

            std::vector<asio::asio_buffer_info> buffer_infos_;
            std::vector<pack_fn> output_converters_;
            std::vector<unpack_fn> input_converters_;
            std::vector<sample> output_interleaved_;
            std::vector<sample> input_interleaved_;
        };

    } // namespace

    std::unique_ptr<backend> create_asio_backend_win32(const open_request &request) noexcept
    {
        return std::make_unique<asio_backend_win32>(request);
    }

} // namespace catalyst::audio::detail

#endif // _WIN32
