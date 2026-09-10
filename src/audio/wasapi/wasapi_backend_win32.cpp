/**
 * @file wasapi_backend_win32.cpp
 * @brief WASAPI output and capture backend for Windows. Enumerates endpoints by their stable
 * endpoint identifier, negotiates a format and reports what was actually agreed, runs its render
 * thread under MMCSS "Pro Audio" scheduling, detects device invalidation instead of silently
 * going quiet, and queues endpoint topology changes for `stream::pump()` to publish.
 *
 * A negotiated format no longer has to be float32. It is preferred - and when the engine agrees to
 * it the renderer still writes straight into the driver's buffer with no copy at all - but any
 * layout the module's shared converters understand is accepted, which is what makes exclusive mode
 * usable on the many interfaces that only offer 24- or 32-bit PCM. That table is the same one the
 * ASIO backend resolves its per-channel conversions from.
 *
 * Duplex is not implemented here: WASAPI render and capture are independent clients with
 * independent clocks, so a correct implementation needs an asynchronous ring buffer and drift
 * compensation. Rather than ship a version that glitches, this backend reports
 * `error_code::unsupported_operation` for `stream_direction::duplex`.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../detail_backend.hpp"

#if defined(_WIN32)

#include "../detail_backend_base.hpp"
#include "../detail_convert.hpp"
#include "../detail_render.hpp"
#include "../win32/detail_win32.hpp"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace catalyst::audio::detail
{

    namespace
    {
        using win32::com_apartment;
        using win32::com_ptr;
        using win32::co_task_ptr;
        using win32::error_from_hresult;
        using win32::wide_to_utf8;

        using format_ptr = co_task_ptr<WAVEFORMATEX>;

        /// Windows audio data is little-endian, whatever the sample layout.
        constexpr std::endian device_order = std::endian::little;

        /**
         * @class notification_client
         * @brief Minimal `IMMNotificationClient`, forwarding to a `notice_publisher` and nothing
         * else.
         * @details It never touches the backend: the publisher is shared-owned, so a notification
         * already in flight when the stream tears down cannot dereference freed state. The one
         * piece of judgement here is distinguishing *our* endpoint going inactive - which ends this
         * stream, and is reported as `lost` - from any other endpoint doing so.
         */
        class notification_client final : public IMMNotificationClient
        {
        public:
            notification_client(std::shared_ptr<notice_publisher> publisher, std::wstring watched)
                : publisher_(std::move(publisher)), watched_(std::move(watched))
            {
            }

            ULONG STDMETHODCALLTYPE AddRef() override
            {
                return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
            }

            ULONG STDMETHODCALLTYPE Release() override
            {
                const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (remaining == 0)
                    delete this;
                return remaining;
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
            {
                if (!object)
                    return E_POINTER;

                if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
                {
                    *object = static_cast<IMMNotificationClient *>(this);
                    AddRef();
                    return S_OK;
                }

                *object = nullptr;
                return E_NOINTERFACE;
            }

            HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
                EDataFlow flow, ERole role, LPCWSTR device_id) override
            {
                (void)flow;
                if (role == eConsole)
                    fire(device_notice::kind::default_changed, device_id);
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR device_id) override
            {
                fire(device_notice::kind::added, device_id);
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR device_id) override
            {
                fire(device_notice::kind::removed, device_id);
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR device_id, DWORD new_state) override
            {
                if (new_state == DEVICE_STATE_ACTIVE)
                {
                    fire(device_notice::kind::added, device_id);
                    return S_OK;
                }

                const bool ours = !watched_.empty() && device_id && watched_ == device_id;
                fire(ours ? device_notice::kind::lost : device_notice::kind::removed, device_id);
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
            {
                return S_OK;
            }

        private:
            ~notification_client() = default;

            void fire(device_notice::kind what, LPCWSTR device_id) noexcept
            {
                if (!publisher_)
                    return;

                try
                {
                    // The conversion allocates, and this is a COM notification thread: a failure
                    // here must cost the caller one event, not the process.
                    publisher_->fire(what, wide_to_utf8(device_id));
                }
                catch (...)
                {
                }
            }

            std::atomic<ULONG> refs_{1};
            std::shared_ptr<notice_publisher> publisher_;
            const std::wstring watched_;
        };

        DWORD channel_mask_for(channel_count channels) noexcept
        {
            switch (channels)
            {
            case 1:
                return SPEAKER_FRONT_CENTER;
            case 2:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            case 4:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                       SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
            case 6:
                return KSAUDIO_SPEAKER_5POINT1;
            case 8:
                return KSAUDIO_SPEAKER_7POINT1_SURROUND;
            default:
                // No standard layout; let the engine treat the channels as discrete.
                return 0;
            }
        }

        /**
         * @brief Which of the module's sample layouts a `WAVEFORMATEX` describes.
         * @details `WAVE_FORMAT_EXTENSIBLE` is unwrapped to the tag its subformat means, so the
         * two spellings of the same layout resolve identically. `wValidBitsPerSample` is not
         * consulted: where it is smaller than the container - 24 valid bits in a 32-bit field is
         * the common case - the samples are left-justified in the container, so converting at the
         * container's width is correct and merely writes bits the hardware discards.
         */
        [[nodiscard]] sample_format format_of(const WAVEFORMATEX *format) noexcept
        {
            if (!format)
                return sample_format::unknown;

            WORD tag = format->wFormatTag;

            if (tag == WAVE_FORMAT_EXTENSIBLE &&
                format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
            {
                const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);

                if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                    tag = WAVE_FORMAT_IEEE_FLOAT;
                else if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
                    tag = WAVE_FORMAT_PCM;
                else
                    return sample_format::unknown;
            }

            if (tag == WAVE_FORMAT_IEEE_FLOAT)
            {
                switch (format->wBitsPerSample)
                {
                case 32:
                    return sample_format::float32;
                case 64:
                    return sample_format::float64;
                default:
                    return sample_format::unknown;
                }
            }

            if (tag == WAVE_FORMAT_PCM)
            {
                switch (format->wBitsPerSample)
                {
                case 16:
                    return sample_format::int16;
                case 24:
                    return sample_format::int24;
                case 32:
                    return sample_format::int32;
                default:
                    return sample_format::unknown;
                }
            }

            return sample_format::unknown;
        }

        /** @brief Builds an extensible format of @p layout, which is what drivers prefer to see. */
        format_ptr make_format(sample_rate_t rate, channel_count channels, sample_format layout)
        {
            const std::size_t width = bytes_per_sample(layout);
            if (width == 0 || channels == 0)
                return nullptr;

            auto *extensible =
                static_cast<WAVEFORMATEXTENSIBLE *>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
            if (!extensible)
                return nullptr;

            std::memset(extensible, 0, sizeof(WAVEFORMATEXTENSIBLE));

            const auto bits = static_cast<WORD>(width * 8);
            const auto block_align = static_cast<WORD>(channels * width);

            extensible->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            extensible->Format.nChannels = static_cast<WORD>(channels);
            extensible->Format.nSamplesPerSec = rate;
            extensible->Format.wBitsPerSample = bits;
            extensible->Format.nBlockAlign = block_align;
            extensible->Format.nAvgBytesPerSec = rate * block_align;
            extensible->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

            extensible->Samples.wValidBitsPerSample = bits;
            extensible->dwChannelMask = channel_mask_for(channels);
            extensible->SubFormat =
                (layout == sample_format::float32 || layout == sample_format::float64)
                    ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                    : KSDATAFORMAT_SUBTYPE_PCM;

            return format_ptr(reinterpret_cast<WAVEFORMATEX *>(extensible));
        }

        REFERENCE_TIME hns_from_frames(std::uint32_t frames, sample_rate_t rate) noexcept
        {
            if (rate == 0 || frames == 0)
                return 0;

            const double duration = static_cast<double>(frames) / static_cast<double>(rate);
            const auto hns = static_cast<REFERENCE_TIME>(duration * 10000000.0 + 0.5);
            return hns > 0 ? hns : 1;
        }

        std::uint32_t frames_from_hns(REFERENCE_TIME hns, sample_rate_t rate) noexcept
        {
            if (hns <= 0 || rate == 0)
                return 0;

            const double duration = static_cast<double>(hns) / 10000000.0;
            return static_cast<std::uint32_t>(duration * static_cast<double>(rate) + 0.5);
        }

        /** @brief The endpoint identifier `IMMDevice::GetId` allocates, or an empty string. */
        std::wstring endpoint_id(IMMDevice *device)
        {
            LPWSTR raw = nullptr;
            if (!device || FAILED(device->GetId(&raw)) || !raw)
                return {};

            const co_task_ptr<WCHAR> owned(raw);
            return std::wstring(raw);
        }

        std::string friendly_name(IMMDevice *device)
        {
            com_ptr<IPropertyStore> properties;
            if (!device || FAILED(device->OpenPropertyStore(STGM_READ, properties.put())) || !properties)
                return {};

            PROPVARIANT value;
            PropVariantInit(&value);

            std::string name;
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal)
            {
                name = wide_to_utf8(value.pwszVal);
            }

            PropVariantClear(&value);
            return name;
        }

        EDataFlow flow_of(IMMDevice *device) noexcept
        {
            com_ptr<IMMEndpoint> endpoint;
            if (device && SUCCEEDED(device->QueryInterface(__uuidof(IMMEndpoint), endpoint.put_void())) &&
                endpoint)
            {
                EDataFlow flow = eRender;
                if (SUCCEEDED(endpoint->GetDataFlow(&flow)))
                    return flow;
            }
            return eRender;
        }

        std::wstring default_endpoint_id(IMMDeviceEnumerator *enumerator, EDataFlow flow)
        {
            com_ptr<IMMDevice> device;
            if (!enumerator ||
                FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, device.put())) || !device)
            {
                return {};
            }
            return endpoint_id(device.get());
        }

        com_ptr<IMMDeviceEnumerator> make_enumerator() noexcept
        {
            com_ptr<IMMDeviceEnumerator> enumerator;
            (void)CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                nullptr,
                CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator),
                enumerator.put_void());
            return enumerator;
        }

        class wasapi_backend_win32 final : public backend_base
        {
        public:
            explicit wasapi_backend_win32(open_request request)
                : backend_base(backend_kind::wasapi, std::move(request))
            {
            }

            ~wasapi_backend_win32() override { close(); }

            [[nodiscard]] std::expected<std::vector<device_info>, error> enumerate_devices() const override
            {
                const com_apartment apartment = com_apartment::enter();
                if (!apartment.usable())
                    return failure(error_code::platform_error);

                const com_ptr<IMMDeviceEnumerator> enumerator = make_enumerator();
                if (!enumerator)
                    return failure(error_code::platform_error);

                const EDataFlow flow = enumeration_flow();

                com_ptr<IMMDeviceCollection> collection;
                if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put())) ||
                    !collection)
                {
                    return failure(error_code::platform_error);
                }

                UINT count = 0;
                if (FAILED(collection->GetCount(&count)))
                    return failure(error_code::platform_error);

                const std::wstring default_render = default_endpoint_id(enumerator.get(), eRender);
                const std::wstring default_capture = default_endpoint_id(enumerator.get(), eCapture);

                std::vector<device_info> devices;
                devices.reserve(count);

                for (UINT index = 0; index < count; ++index)
                {
                    com_ptr<IMMDevice> device;
                    if (FAILED(collection->Item(index, device.put())) || !device)
                        continue;

                    const std::wstring wide_id = endpoint_id(device.get());
                    if (wide_id.empty())
                        continue;

                    device_info info;
                    info.backend = backend_kind::wasapi;
                    info.id = wide_to_utf8(wide_id);
                    info.name = friendly_name(device.get());
                    if (info.name.empty())
                        info.name = info.id;

                    const EDataFlow device_flow = flow_of(device.get());
                    info.is_default = (device_flow == eRender && wide_id == default_render) ||
                                      (device_flow == eCapture && wide_id == default_capture);

                    // Activating the client is the only reliable way to learn the endpoint's
                    // channel count and rate. Enumeration is not a hot path.
                    com_ptr<IAudioClient> client;
                    if (SUCCEEDED(device->Activate(
                            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void())) &&
                        client)
                    {
                        WAVEFORMATEX *raw = nullptr;
                        if (SUCCEEDED(client->GetMixFormat(&raw)) && raw)
                        {
                            const format_ptr mix(raw);
                            info.default_sample_rate = mix->nSamplesPerSec;
                            if (device_flow == eCapture)
                                info.max_input_channels = mix->nChannels;
                            else
                                info.max_output_channels = mix->nChannels;
                        }
                    }

                    devices.push_back(std::move(info));
                }

                return devices;
            }

            std::expected<void, error> open() override
            {
                if (request_.direction == stream_direction::duplex)
                    return failure(error_code::unsupported_operation);

                close();

                apartment_ = com_apartment::enter();
                if (!apartment_.usable())
                    return failure(error_code::platform_error);

                capture_ = request_.direction == stream_direction::input;

                enumerator_ = make_enumerator();
                if (!enumerator_)
                    return failure(error_code::platform_error);

                if (const auto opened = open_device(); !opened)
                    return std::unexpected(opened.error());

                if (const auto negotiated = negotiate_format(); !negotiated)
                    return std::unexpected(negotiated.error());

                if (const auto services = create_services(); !services)
                    return std::unexpected(services.error());

                register_notifications();

                mark_opened();
                return {};
            }

            std::expected<void, error> start() override
            {
                if (!opened_ || running_.load(std::memory_order_acquire))
                    return {};

                stopping_.store(false, std::memory_order_release);
                clear_failure();

                // Fill the buffer before the clock starts, or the first period is silence.
                if (!capture_)
                {
                    if (const auto primed = pump_output(); !primed)
                        return std::unexpected(primed.error());
                }

                if (const HRESULT hr = client_->Start(); FAILED(hr))
                    return failure(error_from_hresult(hr));

                running_.store(true, std::memory_order_release);

                try
                {
                    thread_ = std::thread([this] { run(); });
                }
                catch (...)
                {
                    (void)client_->Stop();
                    running_.store(false, std::memory_order_release);
                    return failure(error_code::thread_failure);
                }

                return {};
            }

            void stop() noexcept override
            {
                if (!running_.exchange(false, std::memory_order_acq_rel))
                    return;

                stopping_.store(true, std::memory_order_release);
                if (event_)
                    SetEvent(event_);

                if (thread_.joinable())
                {
                    try
                    {
                        thread_.join();
                    }
                    catch (...)
                    {
                        // A thread that cannot be joined must not take the process down from a
                        // noexcept teardown path; leaking it is the lesser failure.
                    }
                }

                if (client_)
                    (void)client_->Stop();
            }

            void close() noexcept override
            {
                stop();
                unregister_notifications();

                capture_client_.reset();
                render_client_.reset();
                client_.reset();
                device_.reset();
                enumerator_.reset();

                active_format_.reset();
                scratch_.clear();
                scratch_.shrink_to_fit();

                if (event_)
                {
                    CloseHandle(event_);
                    event_ = nullptr;
                }

                buffer_frames_ = 0;
                period_frames_ = 0;
                channels_ = 0;
                device_layout_ = sample_format::unknown;
                pack_ = nullptr;
                unpack_ = nullptr;
                direct_ = false;
                device_latency_ = seconds{0.0};
                wide_device_id_.clear();

                mark_closed();
                apartment_.leave();
            }

            [[nodiscard]] stream_info info() const override
            {
                stream_info out = base_info();
                out.output_channels = capture_ ? 0 : channels_;
                out.input_channels = capture_ ? channels_ : 0;
                out.output_layout = capture_ ? channel_layout::unspecified : layout_for(channels_);
                out.block_frames = period_frames_ ? period_frames_ : buffer_frames_;

                const seconds latency = frames_to_time(buffer_frames_, rate()) + device_latency_;
                out.output_latency = capture_ ? seconds{0.0} : latency;
                out.input_latency = capture_ ? latency : seconds{0.0};

                out.exclusive = exclusive_;
                return out;
            }

        private:
            EDataFlow enumeration_flow() const noexcept
            {
                switch (request_.direction)
                {
                case stream_direction::input:
                    return eCapture;
                case stream_direction::duplex:
                    return eAll;
                case stream_direction::output:
                default:
                    return eRender;
                }
            }

            /// Resolves the request's `device_selector` against the endpoints of this direction.
            ///
            /// The match itself is `device_selector::matches`, the same function `find_device` uses,
            /// so an endpoint a device picker showed as the match is the endpoint that opens here.
            std::expected<void, error> open_device()
            {
                const EDataFlow flow = capture_ ? eCapture : eRender;

                if (request_.device.by != device_selector::match::system_default)
                {
                    com_ptr<IMMDeviceCollection> collection;
                    if (SUCCEEDED(enumerator_->EnumAudioEndpoints(
                            flow, DEVICE_STATE_ACTIVE, collection.put())) &&
                        collection)
                    {
                        UINT count = 0;
                        if (SUCCEEDED(collection->GetCount(&count)))
                        {
                            for (UINT index = 0; index < count && !device_; ++index)
                            {
                                com_ptr<IMMDevice> candidate;
                                if (FAILED(collection->Item(index, candidate.put())) || !candidate)
                                    continue;

                                device_info described;
                                described.id = wide_to_utf8(endpoint_id(candidate.get()));
                                described.name = friendly_name(candidate.get());

                                if (request_.device.matches(described))
                                    device_ = candidate;
                            }
                        }
                    }

                    // An explicit request that cannot be honoured is an error, not a silent
                    // downgrade to the default endpoint.
                    if (!device_)
                        return failure(error_code::no_device);
                }

                if (!device_)
                {
                    if (FAILED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, device_.put())) ||
                        !device_)
                    {
                        return failure(error_code::no_device);
                    }
                }

                wide_device_id_ = endpoint_id(device_.get());
                identify(wide_to_utf8(wide_device_id_), friendly_name(device_.get()));
                return {};
            }

            std::expected<void, error> activate_client()
            {
                if (FAILED(device_->Activate(
                        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.put_void())) ||
                    !client_)
                {
                    return failure(error_code::platform_error);
                }
                return {};
            }

            /// Opens the stream and records what the device actually agreed to.
            ///
            /// In shared mode a rate or channel mismatch is resolved by asking the audio engine to
            /// convert (`AUTOCONVERTPCM`), so the caller keeps the format it asked for rather than
            /// silently receiving the device mix rate. Only if that is refused does the stream fall
            /// back to a format the device already wants, and only when `allow_format_fallback`
            /// permits it.
            std::expected<void, error> negotiate_format()
            {
                if (const auto activated = activate_client(); !activated)
                    return std::unexpected(activated.error());

                REFERENCE_TIME default_period = 0;
                REFERENCE_TIME minimum_period = 0;
                (void)client_->GetDevicePeriod(&default_period, &minimum_period);

                exclusive_ = request_.exclusive;
                const AUDCLNT_SHAREMODE share_mode =
                    exclusive_ ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

                format_ptr desired =
                    make_format(request_.sample_rate, requested_channels(), sample_format::float32);
                if (!desired)
                    return failure(error_code::platform_error);

                const REFERENCE_TIME requested_duration =
                    request_.block_frames
                        ? hns_from_frames(request_.block_frames, request_.sample_rate)
                        : (exclusive_ ? minimum_period : default_period);

                const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

                HRESULT hr = try_initialize(share_mode, flags, requested_duration, desired.get());
                if (SUCCEEDED(hr))
                {
                    active_format_ = std::move(desired);
                    return finish_negotiation();
                }

                if (!request_.allow_format_fallback)
                {
                    // The caller asked for exactly this format and did not get it. Say what the
                    // device would have taken instead, which is the only actionable part.
                    error refused = make_error(error_from_hresult(hr), backend_kind::wasapi);
                    if (refused.code == error_code::format_unsupported)
                    {
                        if (const format_ptr offered = mix_format())
                        {
                            refused.offered_sample_rate = offered->nSamplesPerSec;
                            refused.offered_channels = offered->nChannels;
                        }
                    }
                    return std::unexpected(refused);
                }

                if (!exclusive_)
                {
                    // Let the audio engine resample and remix on our behalf.
                    const DWORD convert_flags = flags |
                                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

                    hr = try_initialize(share_mode, convert_flags, requested_duration, desired.get());
                    if (SUCCEEDED(hr))
                    {
                        active_format_ = std::move(desired);
                        return finish_negotiation();
                    }
                }

                // Last resort: take a format the device already wants, in whatever layout that is.
                format_ptr fallback = exclusive_ ? device_native_format() : mix_format();
                if (!fallback || format_of(fallback.get()) == sample_format::unknown)
                    return failure(error_code::format_unsupported);

                const REFERENCE_TIME fallback_duration =
                    request_.block_frames
                        ? hns_from_frames(request_.block_frames, fallback->nSamplesPerSec)
                        : (exclusive_ ? minimum_period : default_period);

                hr = try_initialize(share_mode, flags, fallback_duration, fallback.get());
                if (FAILED(hr))
                {
                    error refused = make_error(error_from_hresult(hr), backend_kind::wasapi);
                    refused.offered_sample_rate = fallback->nSamplesPerSec;
                    refused.offered_channels = fallback->nChannels;
                    return std::unexpected(refused);
                }

                active_format_ = std::move(fallback);
                return finish_negotiation();
            }

            /// Runs `IAudioClient::Initialize`, retrying once on the exclusive-mode alignment
            /// error with the buffer size the driver reports it actually wants.
            HRESULT try_initialize(
                AUDCLNT_SHAREMODE share_mode,
                DWORD flags,
                REFERENCE_TIME duration,
                const WAVEFORMATEX *format)
            {
                const REFERENCE_TIME periodicity =
                    share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE ? duration : 0;

                HRESULT hr =
                    client_->Initialize(share_mode, flags, duration, periodicity, format, nullptr);

                if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
                {
                    UINT32 aligned = 0;
                    if (SUCCEEDED(client_->GetBufferSize(&aligned)) && aligned > 0)
                    {
                        const REFERENCE_TIME aligned_duration =
                            hns_from_frames(aligned, format->nSamplesPerSec);

                        // The client is unusable after this error and must be recreated.
                        if (activate_client())
                        {
                            hr = client_->Initialize(
                                share_mode,
                                flags,
                                aligned_duration,
                                share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE ? aligned_duration : 0,
                                format,
                                nullptr);
                        }
                    }
                }

                if (FAILED(hr))
                {
                    // Leave a clean client behind for the next negotiation attempt.
                    (void)activate_client();
                }

                return hr;
            }

            format_ptr mix_format()
            {
                WAVEFORMATEX *raw = nullptr;
                if (FAILED(client_->GetMixFormat(&raw)) || !raw)
                    return nullptr;
                return format_ptr(raw);
            }

            bool supported_exclusive(const WAVEFORMATEX *format) noexcept
            {
                return format &&
                       client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, format, nullptr) == S_OK;
            }

            /// In exclusive mode the engine performs no conversion, so the only formats that can
            /// work are ones the driver reports as directly supported. Every layout the module can
            /// convert is probed, in descending order of fidelity - which is what makes exclusive
            /// mode reachable at all on interfaces that offer no float format.
            format_ptr device_native_format()
            {
                if (format_ptr candidate = mix_format();
                    candidate && supported_exclusive(candidate.get()))
                {
                    return candidate;
                }

                static constexpr sample_format layouts[] = {
                    sample_format::float32,
                    sample_format::int32,
                    sample_format::int24,
                    sample_format::int16,
                };

                // The shape the caller asked for, before anything else.
                for (const sample_format layout : layouts)
                {
                    format_ptr probe =
                        make_format(request_.sample_rate, requested_channels(), layout);
                    if (probe && supported_exclusive(probe.get()))
                        return probe;
                }

                static constexpr sample_rate_t rates[] = {48000, 44100, 96000, 88200, 192000};
                static constexpr channel_count channel_counts[] = {2, 1, 4, 6, 8};

                for (const sample_rate_t rate : rates)
                {
                    for (const channel_count channels : channel_counts)
                    {
                        for (const sample_format layout : layouts)
                        {
                            format_ptr probe = make_format(rate, channels, layout);
                            if (probe && supported_exclusive(probe.get()))
                                return probe;
                        }
                    }
                }

                return nullptr;
            }

            /// Records what was agreed and resolves the conversion once, so the render thread never
            /// asks what format it is in.
            std::expected<void, error> finish_negotiation()
            {
                device_layout_ = format_of(active_format_.get());
                if (device_layout_ == sample_format::unknown)
                    return failure(error_code::format_unsupported);

                direct_ = is_native_float(device_layout_, device_order);
                pack_ = pack_for(device_layout_, device_order);
                unpack_ = unpack_for(device_layout_, device_order);
                if (!pack_ || !unpack_)
                    return failure(error_code::format_unsupported);

                set_rate(active_format_->nSamplesPerSec);
                channels_ = active_format_->nChannels;

                if (FAILED(client_->GetBufferSize(&buffer_frames_)) || buffer_frames_ == 0)
                    return failure(error_code::platform_error);

                if (exclusive_)
                {
                    // In exclusive mode the device hands the whole buffer over once per period, so
                    // the buffer *is* the block. The engine's period is not involved and asking for
                    // it yields the shared-mode figure, which is a different number entirely.
                    period_frames_ = buffer_frames_;
                }
                else
                {
                    REFERENCE_TIME default_period = 0;
                    REFERENCE_TIME minimum_period = 0;
                    if (SUCCEEDED(client_->GetDevicePeriod(&default_period, &minimum_period)))
                        period_frames_ = frames_from_hns(default_period, rate());

                    if (period_frames_ == 0 || period_frames_ > buffer_frames_)
                        period_frames_ = buffer_frames_;
                }

                REFERENCE_TIME latency = 0;
                if (SUCCEEDED(client_->GetStreamLatency(&latency)) && latency > 0)
                    device_latency_ = seconds{static_cast<double>(latency) / 10000000.0};

                return {};
            }

            channel_count requested_channels() const noexcept
            {
                const channel_count channels =
                    capture_ ? request_.input_channels : request_.output_channels;
                return channels ? channels : 2;
            }

            std::expected<void, error> create_services()
            {
                event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!event_)
                    return failure(error_code::platform_error);

                if (const HRESULT hr = client_->SetEventHandle(event_); FAILED(hr))
                    return failure(error_from_hresult(hr));

                if (capture_)
                {
                    if (FAILED(client_->GetService(
                            __uuidof(IAudioCaptureClient), capture_client_.put_void())) ||
                        !capture_client_)
                    {
                        return failure(error_code::platform_error);
                    }
                }
                else
                {
                    if (FAILED(client_->GetService(
                            __uuidof(IAudioRenderClient), render_client_.put_void())) ||
                        !render_client_)
                    {
                        return failure(error_code::platform_error);
                    }
                }

                // One float buffer serves both jobs the driver's own buffer cannot: holding the
                // block while it is converted, and standing in for a packet the device flagged as
                // silent. A float32 output stream needs neither and allocates nothing.
                if (capture_ || !direct_)
                    scratch_.assign(static_cast<std::size_t>(buffer_frames_) * channels_, 0.0f);

                return {};
            }

            void register_notifications()
            {
                auto *client =
                    new (std::nothrow) notification_client(publisher(), wide_device_id_);
                if (!client)
                    return;

                if (SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(client)))
                    notification_client_ = client;
                else
                    client->Release();
            }

            void unregister_notifications() noexcept
            {
                // Retired before it is unregistered, so a notification already inside the client
                // when we get here queues nothing rather than racing the rest of teardown.
                if (const auto &publisher = this->publisher())
                    publisher->retire();

                if (notification_client_)
                {
                    if (enumerator_)
                        (void)enumerator_->UnregisterEndpointNotificationCallback(notification_client_);
                    notification_client_->Release();
                    notification_client_ = nullptr;
                }
            }

            void run() noexcept
            {
                const com_apartment apartment = com_apartment::enter();

                // MMCSS is what keeps the render thread ahead of ordinary work under load. Without
                // it the stream glitches whenever the machine is busy.
                DWORD task_index = 0;
                HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

                std::uint32_t consecutive_timeouts = 0;

                while (!stopping_.load(std::memory_order_acquire))
                {
                    const DWORD wait = WaitForSingleObject(event_, 2000);

                    if (stopping_.load(std::memory_order_acquire))
                        break;

                    if (wait == WAIT_OBJECT_0)
                    {
                        consecutive_timeouts = 0;

                        const auto pumped = capture_ ? pump_capture() : pump_output();
                        if (!pumped)
                        {
                            fail_stream(pumped.error().code);
                            break;
                        }

                        continue;
                    }

                    if (wait == WAIT_TIMEOUT)
                    {
                        // The device stopped clocking us. Anything it should have played is gone.
                        stats_.add_xrun();

                        if (++consecutive_timeouts >= 2)
                        {
                            fail_stream(error_code::device_lost);
                            break;
                        }

                        continue;
                    }

                    fail_stream(error_code::platform_error);
                    break;
                }

                if (mmcss)
                    AvRevertMmThreadCharacteristics(mmcss);
            }

            std::expected<void, error> pump_output() noexcept
            {
                UINT32 remaining = buffer_frames_;

                if (!exclusive_)
                {
                    // Shared mode refills only the part of the buffer the engine has drained.
                    // Exclusive mode must not ask: `GetCurrentPadding` reports an exclusive render
                    // buffer as entirely full, so consulting it here left the stream permanently
                    // convinced there was no room and it never called the renderer at all.
                    UINT32 padding = 0;
                    if (const HRESULT hr = client_->GetCurrentPadding(&padding); FAILED(hr))
                        return failure(error_from_hresult(hr));

                    if (padding >= buffer_frames_)
                        return {};

                    remaining = buffer_frames_ - padding;
                }

                const sample_rate_t stream_rate = rate();

                // Hand the callback consistent, period-sized blocks instead of whatever happens to
                // be free, which can be a handful of frames and makes per-call overhead dominate.
                while (remaining > 0)
                {
                    const UINT32 chunk = std::min<UINT32>(remaining, period_frames_);
                    const auto samples = static_cast<std::size_t>(chunk) * channels_;

                    BYTE *data = nullptr;
                    if (const HRESULT hr = render_client_->GetBuffer(chunk, &data); FAILED(hr))
                        return failure(error_from_hresult(hr));

                    if (!data)
                        return failure(error_code::platform_error);

                    if (direct_)
                    {
                        // The device's own buffer *is* a float32 block, so the renderer writes
                        // straight into it and nothing is copied.
                        dispatcher_.dispatch(
                            std::span<sample>(reinterpret_cast<sample *>(data), samples),
                            {},
                            chunk,
                            channels_,
                            0,
                            stream_rate);
                    }
                    else if (samples <= scratch_.size())
                    {
                        dispatcher_.dispatch(
                            std::span<sample>(scratch_.data(), samples),
                            {},
                            chunk,
                            channels_,
                            0,
                            stream_rate);

                        pack_(reinterpret_cast<std::byte *>(data), scratch_.data(), samples, 1);
                    }
                    else
                    {
                        // Cannot happen: the scratch buffer is the whole endpoint buffer, and a
                        // chunk is a period of it. Releasing silence beats writing past the end.
                        (void)render_client_->ReleaseBuffer(chunk, AUDCLNT_BUFFERFLAGS_SILENT);
                        return failure(error_code::platform_error);
                    }

                    if (const HRESULT hr = render_client_->ReleaseBuffer(chunk, 0); FAILED(hr))
                        return failure(error_from_hresult(hr));

                    remaining -= chunk;
                }

                return {};
            }

            std::expected<void, error> pump_capture() noexcept
            {
                const sample_rate_t stream_rate = rate();

                for (;;)
                {
                    UINT32 packet = 0;
                    if (const HRESULT hr = capture_client_->GetNextPacketSize(&packet); FAILED(hr))
                        return failure(error_from_hresult(hr));

                    if (packet == 0)
                        return {};

                    BYTE *data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;

                    const HRESULT hr =
                        capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                    if (hr == AUDCLNT_S_BUFFER_EMPTY)
                        return {};
                    if (FAILED(hr))
                        return failure(error_from_hresult(hr));

                    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                        stats_.add_xrun();

                    const auto samples = static_cast<std::size_t>(frames) * channels_;
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data;

                    std::span<const sample> input;

                    if (samples > scratch_.size() && !(direct_ && !silent))
                    {
                        // No room to substitute or convert; the packet is dropped rather than
                        // half-delivered, and counted so the caller can see that it was.
                        stats_.add_xrun();
                    }
                    else if (silent)
                    {
                        std::fill_n(scratch_.begin(), samples, sample{0});
                        input = std::span<const sample>(scratch_.data(), samples);
                    }
                    else if (direct_)
                    {
                        input = std::span<const sample>(reinterpret_cast<const sample *>(data), samples);
                    }
                    else
                    {
                        unpack_(
                            scratch_.data(),
                            reinterpret_cast<const std::byte *>(data),
                            samples,
                            1);
                        input = std::span<const sample>(scratch_.data(), samples);
                    }

                    if (!input.empty())
                        dispatcher_.dispatch({}, input, frames, 0, channels_, stream_rate);

                    if (const HRESULT release = capture_client_->ReleaseBuffer(frames); FAILED(release))
                        return failure(error_from_hresult(release));
                }
            }

            com_apartment apartment_;

            com_ptr<IMMDeviceEnumerator> enumerator_;
            com_ptr<IMMDevice> device_;
            com_ptr<IAudioClient> client_;
            com_ptr<IAudioRenderClient> render_client_;
            com_ptr<IAudioCaptureClient> capture_client_;

            notification_client *notification_client_ = nullptr;

            format_ptr active_format_;
            sample_format device_layout_ = sample_format::unknown;
            pack_fn pack_ = nullptr;
            unpack_fn unpack_ = nullptr;
            bool direct_ = false;

            std::vector<sample> scratch_;

            bool capture_ = false;
            bool exclusive_ = false;
            std::atomic<bool> stopping_{false};

            UINT32 buffer_frames_ = 0;
            UINT32 period_frames_ = 0;
            channel_count channels_ = 0;
            seconds device_latency_{0.0};

            std::wstring wide_device_id_;

            HANDLE event_ = nullptr;
            std::thread thread_;
        };

    } // namespace

    std::unique_ptr<backend> create_wasapi_backend_win32(const open_request &request) noexcept
    {
        return std::make_unique<wasapi_backend_win32>(request);
    }

} // namespace catalyst::audio::detail

#endif // _WIN32
