/**
 * @file asio_loader.h
 * @brief The ASIO 2.x ABI, declared rather than included, and the driver loader that reaches it
 * without the COM runtime.
 * @details A real ASIO driver does not export `ASIOInit`, `ASIOStart` and the rest - those are
 * convenience wrappers the Steinberg SDK compiles into the *host*, which forward to the driver's
 * `IASIO` interface. So the SDK is not needed to talk to a driver, only to spell its interface, and
 * that is what this header does: the vtable layout of `IASIO`, in Catalyst's naming, with the field
 * layouts of the structures it passes.
 *
 * The driver itself is reached by loading its DLL and calling `DllGetClassObject` directly, which
 * is what `CoCreateInstance` would have done after a registry lookup and a marshalling decision
 * neither side wants. What is *not* hand-rolled is reference counting: `IASIO` derives from an
 * interface with the standard `AddRef`/`Release` prefix, so it is held by the same @ref
 * catalyst::audio::detail::win32::com_ptr that holds WASAPI's endpoints.
 *
 * Failures come back as `std::expected`, like every other seam in the module, rather than as the
 * exceptions this file used to throw and its caller used to flatten into one `platform_error`.
 * License: MIT (see LICENSE).
 */

#pragma once

#if defined(_WIN32)

#  include "../win32/detail_win32.hpp"

#  include <catalyst/audio/error.hpp>

#  include <cstdint>
#  include <expected>
#  include <string>
#  include <vector>

namespace catalyst::audio::asio
{

    // --------------------------------------------------------------------------------------------
    // The ASIO 2.x ABI
    // --------------------------------------------------------------------------------------------

    using asio_bool = std::int32_t;

    /** @brief A driver result. Zero is `ASE_OK`; every non-zero value is a failure. */
    using asio_error = std::int32_t;

    using asio_sample_rate = double;

    /** @brief Steinberg's `ASIOSampleType` values, which drivers report per channel. */
    enum class asio_sample_type : std::int32_t
    {
        unknown = -1,

        int16_msb = 0,
        int24_msb = 1,
        int32_msb = 2,
        float32_msb = 3,
        float64_msb = 4,

        int32_msb16 = 8,
        int32_msb18 = 9,
        int32_msb20 = 10,
        int32_msb24 = 11,

        int16_lsb = 16,
        int24_lsb = 17,
        int32_lsb = 18,
        float32_lsb = 19,
        float64_lsb = 20,

        int32_lsb16 = 24,
        int32_lsb18 = 25,
        int32_lsb20 = 26,
        int32_lsb24 = 27,
    };

    struct asio_driver_info
    {
        std::int32_t asio_version;
        std::int32_t driver_version;
        char name[32];
        char error_message[124];
        void *sys_ref;
    };

    struct asio_channel_info
    {
        std::int32_t channel;
        asio_bool is_input;
        asio_bool is_active;
        std::int32_t channel_group;
        asio_sample_type sample_type;
        char name[32];
    };

    struct asio_time;

    struct asio_callbacks
    {
        void (*buffer_switch)(std::int32_t double_buffer_index, std::int32_t direct_process);
        void (*sample_rate_did_change)(asio_sample_rate sample_rate);
        std::int32_t (*asio_message)(std::int32_t selector, std::int32_t value, void *message, double *opt);
        asio_time *(*buffer_switch_time_info)(
            asio_time *params,
            std::int32_t double_buffer_index,
            std::int32_t direct_process);
    };

    struct asio_buffer_info
    {
        asio_bool is_input;
        std::int32_t channel_num;
        void *buffers[2];
    };

    /** @brief The selectors `asio_callbacks::asio_message` is called with that we answer. */
    enum class asio_message_selector : std::int32_t
    {
        selector_supported = 1,
        engine_version = 2,
        reset_request = 3,
        buffer_size_change = 4,
        reset_needed = 5,
        latencies_changed = 6,
    };

    // --------------------------------------------------------------------------------------------
    // The COM ABI, without the COM runtime
    // --------------------------------------------------------------------------------------------

    using hresult = long;
    using ulong = unsigned long;

    struct iunknown
    {
        virtual hresult __stdcall QueryInterface(const GUID &riid, void **object) = 0;
        virtual ulong __stdcall AddRef() = 0;
        virtual ulong __stdcall Release() = 0;
    };

    struct iclass_factory : iunknown
    {
        virtual hresult __stdcall CreateInstance(iunknown *outer, const GUID &riid, void **object) = 0;
        virtual hresult __stdcall LockServer(BOOL lock) = 0;
    };

    /** @brief `IASIO`. The declaration order *is* the vtable order and must match Steinberg's. */
    struct iasio : iunknown
    {
        virtual asio_bool __stdcall init(void *sys_handle) = 0;
        virtual void __stdcall get_driver_name(char *name) = 0;
        virtual std::int32_t __stdcall get_driver_version() = 0;
        virtual void __stdcall get_error_message(char *string) = 0;

        virtual asio_error __stdcall start() = 0;
        virtual asio_error __stdcall stop() = 0;
        virtual asio_error __stdcall get_channels(
            std::int32_t *num_input_channels, std::int32_t *num_output_channels) = 0;
        virtual asio_error __stdcall get_latencies(
            std::int32_t *input_latency, std::int32_t *output_latency) = 0;
        virtual asio_error __stdcall get_buffer_size(
            std::int32_t *min_size,
            std::int32_t *max_size,
            std::int32_t *preferred_size,
            std::int32_t *granularity) = 0;
        virtual asio_error __stdcall can_sample_rate(asio_sample_rate sample_rate) = 0;
        virtual asio_error __stdcall get_sample_rate(asio_sample_rate *sample_rate) = 0;
        virtual asio_error __stdcall set_sample_rate(asio_sample_rate sample_rate) = 0;

        virtual asio_error __stdcall get_clock_sources(void *clocks, std::int32_t *num_sources) = 0;
        virtual asio_error __stdcall set_clock_source(std::int32_t reference) = 0;
        virtual asio_error __stdcall get_sample_position(void *sample_position, void *time_stamp) = 0;
        virtual asio_error __stdcall get_channel_info(asio_channel_info *info) = 0;
        virtual asio_error __stdcall create_buffers(
            asio_buffer_info *buffer_infos,
            std::int32_t num_channels,
            std::int32_t buffer_size,
            asio_callbacks *callbacks) = 0;
        virtual asio_error __stdcall dispose_buffers() = 0;
        virtual asio_error __stdcall control_panel() = 0;
        virtual asio_error __stdcall future(std::int32_t selector, void *opt) = 0;
        virtual asio_error __stdcall output_ready() = 0;
    };

    /** @brief `IID_IUnknown`, which is what a driver's class factory is asked for. */
    inline constexpr GUID iid_iunknown = {
        0x00000000,
        0x0000,
        0x0000,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46},
    };

    // --------------------------------------------------------------------------------------------
    // Installed drivers
    // --------------------------------------------------------------------------------------------

    /** @brief One driver as the registry describes it, before anything has been loaded. */
    struct installed_driver
    {
        std::wstring name;
        std::wstring dll_path;
        GUID clsid{};
    };

    /**
     * @brief Every driver registered under the conventional ASIO keys.
     * @details Both the native and the WOW6432 view are read, because a 32-bit host on a 64-bit
     * system sees its drivers only through the latter. Entries without a DLL path or a parseable
     * CLSID are skipped: they cannot be loaded, so listing them would only offer the caller a
     * device that is certain to fail to open.
     */
    [[nodiscard]] std::vector<installed_driver> enumerate_installed_drivers();

    /**
     * @class driver
     * @brief One loaded ASIO driver: its module, and one reference to its `IASIO`.
     * @details Ownership is ordered - the interface is released before the module that defined its
     * vtable is unloaded - which is the whole reason this is a class rather than two members of the
     * backend.
     */
    class driver
    {
    public:
        driver() = default;
        ~driver();

        driver(const driver &) = delete;
        driver &operator=(const driver &) = delete;

        driver(driver &&other) noexcept;
        driver &operator=(driver &&other) noexcept;

        /**
         * @brief Loads @p installed and instantiates its class, without the COM runtime.
         * @return Nothing, or why the driver could not be reached: `no_device` when the DLL is
         * missing or is not an ASIO driver at all, `platform_error` when it is one but refused to
         * instantiate.
         */
        [[nodiscard]] std::expected<void, error_code> open(const installed_driver &installed) noexcept;

        /** @brief Releases the interface, then unloads the module. Safe when never opened. */
        void close() noexcept;

        [[nodiscard]] bool is_open() const noexcept { return static_cast<bool>(instance_); }

        [[nodiscard]] iasio *get() const noexcept { return instance_.get(); }
        iasio *operator->() const noexcept { return instance_.get(); }

    private:
        using dll_get_class_object_fn =
            hresult(WINAPI *)(const GUID &clsid, const GUID &iid, void **object);

        HMODULE module_ = nullptr;
        detail::win32::com_ptr<iasio> instance_;
    };

} // namespace catalyst::audio::asio

#endif // _WIN32
