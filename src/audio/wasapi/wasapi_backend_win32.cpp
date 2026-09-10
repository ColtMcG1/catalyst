/**
 * @file wasapi_backend_win32.cpp
 * @brief WASAPI output and capture backend for Windows. Enumerates endpoints by their stable
 * endpoint identifier, negotiates a format and reports what was actually agreed, runs its render
 * thread under MMCSS "Pro Audio" scheduling, detects device invalidation instead of silently
 * going quiet, and queues endpoint topology changes for `stream::pump()` to publish.
 *
 * Duplex is not implemented here: WASAPI render and capture are independent clients with
 * independent clocks, so a correct implementation needs an asynchronous ring buffer and drift
 * compensation. Rather than ship a version that glitches, this backend reports
 * `error_code::unsupported_operation` for `stream_direction::duplex`.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../detail_backend.hpp"

#if defined(_WIN32)

#include "../detail_render.hpp"
#include "../win32/detail_win32.hpp"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <wrl/client.h>

#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace catalyst::audio::detail
{

    namespace
    {
        using Microsoft::WRL::ComPtr;
        using win32::error_from_hresult;
        using win32::utf8_to_wide;
        using win32::wide_to_utf8;

        struct co_taskmem_free
        {
            void operator()(WAVEFORMATEX *p) const noexcept
            {
                if (p)
                    CoTaskMemFree(p);
            }
        };

        using format_ptr = std::unique_ptr<WAVEFORMATEX, co_taskmem_free>;

        /// Shared state between the backend and its COM notification object.
        ///
        /// Held by `shared_ptr` on both sides so a notification already in flight when the backend
        /// tears down cannot dereference freed memory: teardown clears `active` and drops its
        /// reference, and the notification object keeps the state alive for as long as it needs.
        struct notify_state
        {
            std::atomic<bool> active{true};
            notice_queue *notices = nullptr;
            std::atomic<uint64_t> changes{0};
            std::wstring watched_device_id;
            stream_direction direction = stream_direction::output;

            void fire(device_notice::kind what, std::wstring_view device_id) noexcept
            {
                changes.fetch_add(1, std::memory_order_relaxed);

                if (!active.load(std::memory_order_acquire) || !notices)
                    return;

                // Queued rather than delivered: this is a COM notification thread, and nothing the
                // application wrote should be made to run on it. `stream::pump()` publishes.
                device_notice notice;
                notice.what = what;
                notice.device_id = wide_to_utf8(device_id);
                notice.direction = direction;
                notice.time = audio_clock::now();
                notices->push(std::move(notice));
            }
        };

        /// Minimal `IMMNotificationClient`. Talks only to `notify_state`, never to the backend.
        class notification_client final : public IMMNotificationClient
        {
        public:
            explicit notification_client(std::shared_ptr<notify_state> state)
                : state_(std::move(state)) {}

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

            HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR device_id) override
            {
                (void)flow;
                if (role == eConsole && state_)
                    state_->fire(device_notice::kind::default_changed, device_id ? device_id : L"");
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR device_id) override
            {
                if (state_)
                    state_->fire(device_notice::kind::added, device_id ? device_id : L"");
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR device_id) override
            {
                if (state_)
                    state_->fire(device_notice::kind::removed, device_id ? device_id : L"");
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR device_id, DWORD new_state) override
            {
                if (!state_)
                    return S_OK;

                const std::wstring id = device_id ? device_id : L"";
                const bool is_active_stream = !state_->watched_device_id.empty() &&
                                              id == state_->watched_device_id;

                if (new_state != DEVICE_STATE_ACTIVE)
                {
                    state_->fire(
                        is_active_stream ? device_notice::kind::lost : device_notice::kind::removed,
                        id);
                }
                else
                {
                    state_->fire(device_notice::kind::added, id);
                }

                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
            {
                return S_OK;
            }

        private:
            ~notification_client() = default;

            std::atomic<ULONG> refs_{1};
            std::shared_ptr<notify_state> state_;
        };

        DWORD channel_mask_for(uint32_t channels) noexcept
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

        bool is_float32_format(const WAVEFORMATEX *format) noexcept
        {
            if (!format)
                return false;

            if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
                return format->wBitsPerSample == 32;

            if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
            {
                const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
                return ext->Format.wBitsPerSample == 32 &&
                       ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            }

            return false;
        }

        format_ptr clone_format(const WAVEFORMATEX *source)
        {
            if (!source)
                return nullptr;

            const size_t bytes = sizeof(WAVEFORMATEX) + source->cbSize;
            auto *copy = static_cast<WAVEFORMATEX *>(CoTaskMemAlloc(bytes));
            if (!copy)
                return nullptr;

            std::memcpy(copy, source, bytes);
            return format_ptr(copy);
        }

        format_ptr make_float32_format(uint32_t sample_rate, uint32_t channels)
        {
            auto *ext = static_cast<WAVEFORMATEXTENSIBLE *>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
            if (!ext)
                return nullptr;

            std::memset(ext, 0, sizeof(WAVEFORMATEXTENSIBLE));

            ext->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            ext->Format.nChannels = static_cast<WORD>(channels);
            ext->Format.nSamplesPerSec = sample_rate;
            ext->Format.wBitsPerSample = 32;
            ext->Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
            ext->Format.nAvgBytesPerSec = sample_rate * ext->Format.nBlockAlign;
            ext->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

            ext->Samples.wValidBitsPerSample = 32;
            ext->dwChannelMask = channel_mask_for(channels);
            ext->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

            return format_ptr(reinterpret_cast<WAVEFORMATEX *>(ext));
        }

        REFERENCE_TIME hns_from_frames(uint32_t frames, uint32_t sample_rate) noexcept
        {
            if (sample_rate == 0 || frames == 0)
                return 0;

            const double seconds = static_cast<double>(frames) / static_cast<double>(sample_rate);
            const auto hns = static_cast<REFERENCE_TIME>(seconds * 10000000.0 + 0.5);
            return hns > 0 ? hns : 1;
        }

        uint32_t frames_from_hns(REFERENCE_TIME hns, uint32_t sample_rate) noexcept
        {
            if (hns <= 0 || sample_rate == 0)
                return 0;

            const double seconds = static_cast<double>(hns) / 10000000.0;
            return static_cast<uint32_t>(seconds * static_cast<double>(sample_rate) + 0.5);
        }

        /// RAII for `CoInitializeEx`, which must be balanced on the exact thread that called it.
        class com_scope
        {
        public:
            com_scope() noexcept
            {
                const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                // RPC_E_CHANGED_MODE means COM is already up in another mode; usable, not ours.
                owned_ = SUCCEEDED(hr);
                ok_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
            }

            ~com_scope()
            {
                if (owned_)
                    CoUninitialize();
            }

            com_scope(const com_scope &) = delete;
            com_scope &operator=(const com_scope &) = delete;

            bool ok() const noexcept { return ok_; }

        private:
            bool owned_ = false;
            bool ok_ = false;
        };

        class wasapi_backend_win32 final : public backend
        {
        public:
            explicit wasapi_backend_win32(open_request request)
                : request_(std::move(request)), dispatcher_(request_.render, stats_) {}

            ~wasapi_backend_win32() override { close(); }

            [[nodiscard]] backend_kind kind() const noexcept override { return backend_kind::wasapi; }

            [[nodiscard]] std::expected<std::vector<device_info>, error> enumerate_devices() const override
            {
                com_scope com;
                if (!com.ok())
                    return failure(error_code::platform_error);

                ComPtr<IMMDeviceEnumerator> enumerator;
                if (FAILED(CoCreateInstance(
                        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
                {
                    return failure(error_code::platform_error);
                }

                const EDataFlow flow = enumeration_flow();

                ComPtr<IMMDeviceCollection> collection;
                if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) ||
                    !collection)
                {
                    return failure(error_code::platform_error);
                }

                UINT count = 0;
                if (FAILED(collection->GetCount(&count)))
                    return failure(error_code::platform_error);

                const std::wstring default_render = default_device_id(enumerator.Get(), eRender);
                const std::wstring default_capture = default_device_id(enumerator.Get(), eCapture);

                std::vector<device_info> devices;
                devices.reserve(count);

                for (UINT i = 0; i < count; ++i)
                {
                    ComPtr<IMMDevice> device;
                    if (FAILED(collection->Item(i, &device)) || !device)
                        continue;

                    device_info info;
                    info.backend = backend_kind::wasapi;

                    LPWSTR raw_id = nullptr;
                    if (FAILED(device->GetId(&raw_id)) || !raw_id)
                        continue;

                    const std::wstring wide_id = raw_id;
                    info.id = wide_to_utf8(wide_id);
                    CoTaskMemFree(raw_id);

                    info.name = friendly_name(device.Get());
                    if (info.name.empty())
                        info.name = info.id;

                    const EDataFlow device_flow = flow_of(device.Get());
                    info.is_default = (device_flow == eRender && wide_id == default_render) ||
                                      (device_flow == eCapture && wide_id == default_capture);

                    // Activating the client is the only reliable way to learn the endpoint's
                    // channel count and rate. Enumeration is not a hot path.
                    ComPtr<IAudioClient> client;
                    if (SUCCEEDED(device->Activate(
                            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) &&
                        client)
                    {
                        WAVEFORMATEX *mix = nullptr;
                        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix)
                        {
                            info.default_sample_rate = mix->nSamplesPerSec;
                            if (device_flow == eCapture)
                                info.max_input_channels = mix->nChannels;
                            else
                                info.max_output_channels = mix->nChannels;
                            CoTaskMemFree(mix);
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

                const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE)
                    return failure(error_code::platform_error);
                owns_com_ = SUCCEEDED(com_hr);

                capture_ = request_.direction == stream_direction::input;

                if (FAILED(CoCreateInstance(
                        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_))))
                {
                    return failure(error_code::platform_error);
                }

                if (const auto opened = open_device(); !opened)
                    return std::unexpected(opened.error());

                if (const auto negotiated = negotiate_format(); !negotiated)
                    return std::unexpected(negotiated.error());

                if (const auto services = create_services(); !services)
                    return std::unexpected(services.error());

                register_notifications();

                dispatcher_.rewind();
                stats_.reset();
                opened_ = true;
                return {};
            }

            std::expected<void, error> start() override
            {
                if (!opened_ || running_)
                    return {};

                stopping_.store(false, std::memory_order_release);
                stream_error_.store(error_code::none, std::memory_order_relaxed);

                // Fill the buffer before the clock starts, or the first period is silence.
                if (!capture_)
                {
                    if (const auto primed = pump_output(); !primed)
                        return std::unexpected(primed.error());
                }

                if (const HRESULT hr = client_->Start(); FAILED(hr))
                    return failure(error_from_hresult(hr));

                running_ = true;

                try
                {
                    thread_ = std::thread([this] { run(); });
                }
                catch (...)
                {
                    (void)client_->Stop();
                    running_ = false;
                    return failure(error_code::thread_failure);
                }

                return {};
            }

            void stop() noexcept override
            {
                if (!running_)
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

                running_ = false;
            }

            void close() noexcept override
            {
                stop();
                unregister_notifications();

                capture_client_.Reset();
                render_client_.Reset();
                client_.Reset();
                device_.Reset();
                enumerator_.Reset();

                active_format_.reset();
                silence_.clear();
                silence_.shrink_to_fit();

                if (event_)
                {
                    CloseHandle(event_);
                    event_ = nullptr;
                }

                buffer_frames_ = 0;
                period_frames_ = 0;
                sample_rate_ = 0;
                channels_ = 0;
                device_id_.clear();
                device_name_.clear();
                device_latency_ = seconds{0.0};
                opened_ = false;

                if (owns_com_)
                {
                    CoUninitialize();
                    owns_com_ = false;
                }
            }

            [[nodiscard]] bool is_running() const noexcept override { return running_; }

            [[nodiscard]] stream_info info() const override
            {
                stream_info out;
                out.backend = backend_kind::wasapi;
                out.direction = request_.direction;
                out.sample_rate = sample_rate_;
                out.output_channels = capture_ ? 0 : channels_;
                out.input_channels = capture_ ? channels_ : 0;
                out.output_layout = capture_ ? channel_layout::unspecified : layout_for(channels_);
                out.block_frames = period_frames_ ? period_frames_ : buffer_frames_;

                const seconds latency = frames_to_time(buffer_frames_, sample_rate_) + device_latency_;
                out.output_latency = capture_ ? seconds{0.0} : latency;
                out.input_latency = capture_ ? latency : seconds{0.0};

                out.exclusive = exclusive_;
                out.device_id = device_id_;
                out.device_name = device_name_;
                return out;
            }

            [[nodiscard]] stream_stats stats() const noexcept override
            {
                stream_stats out = stats_.snapshot();
                if (notify_)
                    out.device_changes = notify_->changes.load(std::memory_order_relaxed);
                return out;
            }

            void reset_stats() noexcept override { stats_.reset(); }

            /// The failure the render thread found, if any, clearing it so it is reported once.
            [[nodiscard]] std::optional<error> take_failure() noexcept override
            {
                const error_code code = stream_error_.exchange(error_code::none, std::memory_order_relaxed);
                if (code == error_code::none)
                    return std::nullopt;
                return make_error(code, backend_kind::wasapi);
            }

            [[nodiscard]] std::uint64_t take_xruns() noexcept override { return stats_.take_xruns(); }

        private:
            /// Every failure from this backend names WASAPI, so the caller's log line does too.
            [[nodiscard]] static std::unexpected<error> failure(error_code code) noexcept
            {
                return std::unexpected(make_error(code, backend_kind::wasapi));
            }

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

            static EDataFlow flow_of(IMMDevice *device) noexcept
            {
                ComPtr<IMMEndpoint> endpoint;
                if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&endpoint))) && endpoint)
                {
                    EDataFlow flow = eRender;
                    if (SUCCEEDED(endpoint->GetDataFlow(&flow)))
                        return flow;
                }
                return eRender;
            }

            static std::wstring default_device_id(IMMDeviceEnumerator *enumerator, EDataFlow flow)
            {
                ComPtr<IMMDevice> device;
                if (!enumerator ||
                    FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device)) || !device)
                {
                    return {};
                }

                LPWSTR raw = nullptr;
                if (FAILED(device->GetId(&raw)) || !raw)
                    return {};

                std::wstring id = raw;
                CoTaskMemFree(raw);
                return id;
            }

            static std::string friendly_name(IMMDevice *device)
            {
                ComPtr<IPropertyStore> properties;
                if (!device || FAILED(device->OpenPropertyStore(STGM_READ, &properties)) || !properties)
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

            /// Resolves the request's `device_selector` against the endpoints of this direction.
            ///
            /// The match itself is `device_selector::matches`, the same function `find_device` uses,
            /// so an endpoint a device picker showed as the match is the endpoint that opens here.
            std::expected<void, error> open_device()
            {
                const EDataFlow flow = capture_ ? eCapture : eRender;

                if (request_.device.by != device_selector::match::system_default)
                {
                    ComPtr<IMMDeviceCollection> collection;
                    if (SUCCEEDED(enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) &&
                        collection)
                    {
                        UINT count = 0;
                        if (SUCCEEDED(collection->GetCount(&count)))
                        {
                            for (UINT i = 0; i < count && !device_; ++i)
                            {
                                ComPtr<IMMDevice> candidate;
                                if (FAILED(collection->Item(i, &candidate)) || !candidate)
                                    continue;

                                LPWSTR raw_id = nullptr;
                                if (FAILED(candidate->GetId(&raw_id)) || !raw_id)
                                    continue;

                                device_info described;
                                described.id = wide_to_utf8(raw_id);
                                CoTaskMemFree(raw_id);
                                described.name = friendly_name(candidate.Get());

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
                    if (FAILED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, &device_)) || !device_)
                        return failure(error_code::no_device);
                }

                LPWSTR raw = nullptr;
                if (SUCCEEDED(device_->GetId(&raw)) && raw)
                {
                    wide_device_id_ = raw;
                    device_id_ = wide_to_utf8(wide_device_id_);
                    CoTaskMemFree(raw);
                }

                device_name_ = friendly_name(device_.Get());
                if (device_name_.empty())
                    device_name_ = device_id_;

                return {};
            }

            std::expected<void, error> activate_client()
            {
                client_.Reset();
                if (FAILED(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_)) ||
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
            /// back to the mix format, and only when `allow_format_fallback` permits it.
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

                format_ptr desired = make_float32_format(request_.sample_rate, requested_channels());
                if (!desired)
                    return failure(error_code::platform_error);

                const REFERENCE_TIME requested_duration =
                    request_.block_frames
                        ? hns_from_frames(request_.block_frames, request_.sample_rate)
                        : (exclusive_ ? minimum_period : default_period);

                DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

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

                // Last resort: take the format the device already wants.
                format_ptr fallback = exclusive_ ? device_native_format() : mix_format();
                if (!fallback || !is_float32_format(fallback.get()))
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

                HRESULT hr = client_->Initialize(share_mode, flags, duration, periodicity, format, nullptr);

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
                WAVEFORMATEX *mix = nullptr;
                if (FAILED(client_->GetMixFormat(&mix)) || !mix)
                    return nullptr;
                return format_ptr(mix);
            }

            /// In exclusive mode the engine performs no conversion, so the only formats that can
            /// work are ones the driver reports as directly supported.
            format_ptr device_native_format()
            {
                format_ptr candidate = mix_format();
                if (candidate &&
                    client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, candidate.get(), nullptr) == S_OK)
                {
                    return candidate;
                }

                // Probe the common float32 layouts the caller might be able to use.
                static constexpr uint32_t rates[] = {48000, 44100, 96000, 88200, 192000};
                static constexpr uint32_t channel_counts[] = {2, 1, 4, 6, 8};

                for (const uint32_t rate : rates)
                {
                    for (const uint32_t channels : channel_counts)
                    {
                        format_ptr probe = make_float32_format(rate, channels);
                        if (probe &&
                            client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, probe.get(), nullptr) == S_OK)
                        {
                            return probe;
                        }
                    }
                }

                return nullptr;
            }

            std::expected<void, error> finish_negotiation()
            {
                if (!is_float32_format(active_format_.get()))
                    return failure(error_code::format_unsupported);

                sample_rate_ = active_format_->nSamplesPerSec;
                channels_ = active_format_->nChannels;

                if (FAILED(client_->GetBufferSize(&buffer_frames_)) || buffer_frames_ == 0)
                    return failure(error_code::platform_error);

                REFERENCE_TIME default_period = 0;
                REFERENCE_TIME minimum_period = 0;
                if (SUCCEEDED(client_->GetDevicePeriod(&default_period, &minimum_period)))
                    period_frames_ = frames_from_hns(default_period, sample_rate_);

                if (period_frames_ == 0 || period_frames_ > buffer_frames_)
                    period_frames_ = buffer_frames_;

                REFERENCE_TIME latency = 0;
                if (SUCCEEDED(client_->GetStreamLatency(&latency)) && latency > 0)
                    device_latency_ = seconds{static_cast<double>(latency) / 10000000.0};

                return {};
            }

            channel_count requested_channels() const noexcept
            {
                const channel_count channels = capture_ ? request_.input_channels : request_.output_channels;
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
                    if (FAILED(client_->GetService(IID_PPV_ARGS(&capture_client_))) || !capture_client_)
                        return failure(error_code::platform_error);

                    // Backing store for packets the device flags as silent.
                    silence_.assign(static_cast<size_t>(buffer_frames_) * channels_, 0.0f);
                }
                else
                {
                    if (FAILED(client_->GetService(IID_PPV_ARGS(&render_client_))) || !render_client_)
                        return failure(error_code::platform_error);
                }

                return {};
            }

            void register_notifications()
            {
                notify_ = std::make_shared<notify_state>();
                notify_->notices = request_.notices;
                notify_->watched_device_id = wide_device_id_;
                notify_->direction = request_.direction;

                notification_client *client = new (std::nothrow) notification_client(notify_);
                if (!client)
                    return;

                if (SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(client)))
                    notification_client_ = client;
                else
                    client->Release();
            }

            void unregister_notifications() noexcept
            {
                if (notify_)
                    notify_->active.store(false, std::memory_order_release);

                if (notification_client_)
                {
                    if (enumerator_)
                        (void)enumerator_->UnregisterEndpointNotificationCallback(notification_client_);
                    notification_client_->Release();
                    notification_client_ = nullptr;
                }

                notify_.reset();
            }

            void run() noexcept
            {
                const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                const bool owns_com = SUCCEEDED(com_hr);

                // MMCSS is what keeps the render thread ahead of ordinary work under load. Without
                // it the stream glitches whenever the machine is busy.
                DWORD task_index = 0;
                HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

                uint32_t consecutive_timeouts = 0;

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

                if (owns_com)
                    CoUninitialize();
            }

            /// Records why the stream died, once. `stream::pump()` collects it from take_failure().
            void fail_stream(error_code code) noexcept
            {
                auto expected = error_code::none;
                if (!stream_error_.compare_exchange_strong(expected, code, std::memory_order_relaxed))
                    return;

                if (notify_ && code == error_code::device_lost)
                    notify_->fire(device_notice::kind::lost, wide_device_id_);
            }

            std::expected<void, error> pump_output() noexcept
            {
                UINT32 padding = 0;
                if (const HRESULT hr = client_->GetCurrentPadding(&padding); FAILED(hr))
                    return failure(error_from_hresult(hr));

                if (padding >= buffer_frames_)
                    return {};

                UINT32 remaining = buffer_frames_ - padding;

                // Hand the callback consistent, period-sized blocks instead of whatever happens to
                // be free, which can be a handful of frames and makes per-call overhead dominate.
                while (remaining > 0)
                {
                    const UINT32 chunk = std::min<UINT32>(remaining, period_frames_);

                    BYTE *data = nullptr;
                    if (const HRESULT hr = render_client_->GetBuffer(chunk, &data); FAILED(hr))
                        return failure(error_from_hresult(hr));

                    if (!data)
                        return failure(error_code::platform_error);

                    dispatcher_.dispatch(
                        std::span<sample>(reinterpret_cast<sample *>(data),
                                          static_cast<std::size_t>(chunk) * channels_),
                        {},
                        chunk,
                        channels_,
                        0,
                        sample_rate_);

                    if (const HRESULT hr = render_client_->ReleaseBuffer(chunk, 0); FAILED(hr))
                        return failure(error_from_hresult(hr));

                    remaining -= chunk;
                }

                return {};
            }

            std::expected<void, error> pump_capture() noexcept
            {
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

                    const HRESULT hr = capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                    if (hr == AUDCLNT_S_BUFFER_EMPTY)
                        return {};
                    if (FAILED(hr))
                        return failure(error_from_hresult(hr));

                    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                        stats_.add_xrun();

                    const std::size_t needed = static_cast<std::size_t>(frames) * channels_;
                    std::span<const sample> input(reinterpret_cast<const sample *>(data), needed);

                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data)
                    {
                        input = silence_.size() >= needed
                                    ? std::span<const sample>(silence_.data(), needed)
                                    : std::span<const sample>{};
                    }

                    if (!input.empty())
                        dispatcher_.dispatch({}, input, frames, 0, channels_, sample_rate_);

                    if (const HRESULT release = capture_client_->ReleaseBuffer(frames); FAILED(release))
                        return failure(error_from_hresult(release));
                }
            }

            open_request request_{};
            stats_block stats_;
            render_dispatcher dispatcher_;

            bool owns_com_ = false;
            bool opened_ = false;
            bool running_ = false;
            bool capture_ = false;
            bool exclusive_ = false;
            std::atomic<bool> stopping_{false};
            std::atomic<error_code> stream_error_{error_code::none};

            ComPtr<IMMDeviceEnumerator> enumerator_;
            ComPtr<IMMDevice> device_;
            ComPtr<IAudioClient> client_;
            ComPtr<IAudioRenderClient> render_client_;
            ComPtr<IAudioCaptureClient> capture_client_;

            notification_client *notification_client_ = nullptr;
            std::shared_ptr<notify_state> notify_;

            format_ptr active_format_;
            std::vector<sample> silence_;

            UINT32 buffer_frames_ = 0;
            UINT32 period_frames_ = 0;
            sample_rate_t sample_rate_ = 0;
            channel_count channels_ = 0;
            seconds device_latency_{0.0};

            std::wstring wide_device_id_;
            std::string device_id_;
            std::string device_name_;

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
