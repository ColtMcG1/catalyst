/**
 * @file detail_win32.hpp
 * @brief The audio module's Windows-only vocabulary, shared by the WASAPI and ASIO backends: COM
 * apartment and pointer ownership, task-allocator memory, GUID text, and the mapping from the
 * HRESULTs the audio APIs return onto `error_code`.
 * @details WASAPI is written against the COM runtime and ASIO deliberately is not - its driver is
 * reached by loading the DLL and calling `DllGetClassObject`, so no class is registered and no
 * proxy is marshalled. That difference is real, but it is a difference in *how an interface is
 * obtained*, not in what an interface is: both sides end up holding a pointer whose first three
 * vtable slots are `QueryInterface`, `AddRef` and `Release`. @ref com_ptr is written against that
 * much and nothing more, so one reference-counting primitive serves both, and the ASIO side no
 * longer hand-rolls its own `Release()` calls.
 *
 * The UTF-8 conversion routines that used to live here are one implementation in
 * src/win32/strings.hpp, shared with the platform and input backends, and are re-exported into this
 * namespace so the backends keep spelling them `win32::wide_to_utf8`.
 * License: MIT (see LICENSE).
 */

#pragma once

#if defined(_WIN32)

#  include <win32/strings.hpp>
#  include <win32/windows_lean.hpp>

#  include <audioclient.h>
#  include <objbase.h>

#  include <catalyst/audio/error.hpp>

#  include <cstddef>
#  include <cstdint>
#  include <memory>
#  include <optional>
#  include <string>
#  include <string_view>
#  include <utility>

namespace catalyst::audio::detail::win32
{

    using ::catalyst::detail::win32::utf8_to_wide;
    using ::catalyst::detail::win32::wide_to_utf8;

    // --------------------------------------------------------------------------------------------
    // Reference-counted interface ownership
    // --------------------------------------------------------------------------------------------

    /**
     * @class com_ptr
     * @brief Owns one reference to any interface that exposes `AddRef` and `Release`.
     * @details Constrained to that pair rather than to `IUnknown` on purpose, so the ASIO loader's
     * hand-declared `iasio` - which has the same vtable prefix but is not the SDK's `IUnknown` and
     * never passes through the COM runtime - is held by the same type as an `IMMDevice`.
     *
     * A raw pointer is never adopted implicitly: an interface arrives either through @ref put, the
     * out-parameter form every COM-shaped call wants, or through @ref adopt, which says at the call
     * site that a reference is being taken over rather than shared.
     */
    template <typename Interface>
    class com_ptr
    {
    public:
        com_ptr() noexcept = default;
        com_ptr(std::nullptr_t) noexcept {}

        ~com_ptr() { reset(); }

        com_ptr(const com_ptr &other) noexcept : pointer_(other.pointer_)
        {
            if (pointer_)
                pointer_->AddRef();
        }

        com_ptr(com_ptr &&other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}

        com_ptr &operator=(const com_ptr &other) noexcept
        {
            com_ptr copy(other);
            swap(copy);
            return *this;
        }

        com_ptr &operator=(com_ptr &&other) noexcept
        {
            com_ptr moved(std::move(other));
            swap(moved);
            return *this;
        }

        com_ptr &operator=(std::nullptr_t) noexcept
        {
            reset();
            return *this;
        }

        /** @brief Takes over a reference the caller already holds, without adding another. */
        [[nodiscard]] static com_ptr adopt(Interface *raw) noexcept
        {
            com_ptr owner;
            owner.pointer_ = raw;
            return owner;
        }

        void reset() noexcept
        {
            if (auto *held = std::exchange(pointer_, nullptr))
                held->Release();
        }

        void swap(com_ptr &other) noexcept { std::swap(pointer_, other.pointer_); }

        [[nodiscard]] Interface *get() const noexcept { return pointer_; }
        Interface *operator->() const noexcept { return pointer_; }
        explicit operator bool() const noexcept { return pointer_ != nullptr; }

        /** @brief Releases what is held and yields the slot a callee fills with a new reference. */
        [[nodiscard]] Interface **put() noexcept
        {
            reset();
            return &pointer_;
        }

        /** @brief @ref put for the `void**` out-parameter that `IID_PPV_ARGS` would have supplied. */
        [[nodiscard]] void **put_void() noexcept { return reinterpret_cast<void **>(put()); }

    private:
        Interface *pointer_ = nullptr;
    };

    /**
     * @class com_apartment
     * @brief RAII for `CoInitializeEx`, which must be balanced on the exact thread that called it.
     * @details Default-constructed it holds nothing, so a backend can keep one as a member without
     * entering COM on whichever thread happened to construct the backend; @ref enter is the only
     * way in. `RPC_E_CHANGED_MODE` means the thread is already in an apartment of another kind,
     * which is usable but not ours to leave - hence @ref usable and `entered_` being separate.
     */
    class com_apartment
    {
    public:
        com_apartment() noexcept = default;

        ~com_apartment() { leave(); }

        com_apartment(const com_apartment &) = delete;
        com_apartment &operator=(const com_apartment &) = delete;

        com_apartment(com_apartment &&other) noexcept
            : entered_(std::exchange(other.entered_, false)),
              usable_(std::exchange(other.usable_, false))
        {
        }

        com_apartment &operator=(com_apartment &&other) noexcept
        {
            if (this != &other)
            {
                leave();
                entered_ = std::exchange(other.entered_, false);
                usable_ = std::exchange(other.usable_, false);
            }
            return *this;
        }

        /** @brief Joins the multi-threaded apartment on the calling thread. */
        [[nodiscard]] static com_apartment enter() noexcept
        {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            com_apartment apartment;
            apartment.entered_ = SUCCEEDED(hr);
            apartment.usable_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
            return apartment;
        }

        /** @brief Whether COM can be called on this thread, however it came to be initialized. */
        [[nodiscard]] bool usable() const noexcept { return usable_; }

        /** @brief Leaves the apartment if this object entered it. Idempotent. */
        void leave() noexcept
        {
            if (std::exchange(entered_, false))
                CoUninitialize();
            usable_ = false;
        }

    private:
        bool entered_ = false;
        bool usable_ = false;
    };

    /** @brief Frees memory the task allocator handed out, whatever it was declared to point at. */
    struct co_task_free
    {
        void operator()(void *memory) const noexcept { CoTaskMemFree(memory); }
    };

    /** @brief Owns one `CoTaskMemAlloc` block - a `WAVEFORMATEX`, an endpoint id, a device name. */
    template <typename T>
    using co_task_ptr = std::unique_ptr<T, co_task_free>;

    // --------------------------------------------------------------------------------------------
    // GUID text
    // --------------------------------------------------------------------------------------------

    /**
     * @brief Formats a GUID as `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}`.
     * @details Hand-rolled rather than `StringFromGUID2` so it can be used off the COM runtime,
     * which is the whole point of the ASIO loader, and so that it is the exact inverse of
     * @ref try_parse_guid - the two are what make an ASIO driver's CLSID usable as a `device_info`
     * identifier that survives a round trip through a caller's configuration file.
     */
    [[nodiscard]] inline std::string guid_to_string(const GUID &guid)
    {
        static constexpr char digits[] = "0123456789ABCDEF";

        std::string out;
        out.reserve(38);
        out.push_back('{');

        const auto push_byte = [&out](std::uint8_t value) {
            out.push_back(digits[(value >> 4) & 0x0F]);
            out.push_back(digits[value & 0x0F]);
        };

        const auto push_u32 = [&push_byte](std::uint32_t value) {
            push_byte(static_cast<std::uint8_t>((value >> 24) & 0xFF));
            push_byte(static_cast<std::uint8_t>((value >> 16) & 0xFF));
            push_byte(static_cast<std::uint8_t>((value >> 8) & 0xFF));
            push_byte(static_cast<std::uint8_t>(value & 0xFF));
        };

        const auto push_u16 = [&push_byte](std::uint16_t value) {
            push_byte(static_cast<std::uint8_t>((value >> 8) & 0xFF));
            push_byte(static_cast<std::uint8_t>(value & 0xFF));
        };

        push_u32(guid.Data1);
        out.push_back('-');
        push_u16(guid.Data2);
        out.push_back('-');
        push_u16(guid.Data3);
        out.push_back('-');
        push_byte(guid.Data4[0]);
        push_byte(guid.Data4[1]);
        out.push_back('-');
        for (std::size_t i = 2; i < 8; ++i)
            push_byte(guid.Data4[i]);
        out.push_back('}');

        return out;
    }

    namespace detail_guid
    {

        [[nodiscard]] constexpr bool is_hex(wchar_t c) noexcept
        {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
        }

        [[nodiscard]] constexpr std::uint8_t hex_value(wchar_t c) noexcept
        {
            if (c >= L'0' && c <= L'9')
                return static_cast<std::uint8_t>(c - L'0');
            if (c >= L'a' && c <= L'f')
                return static_cast<std::uint8_t>(10 + (c - L'a'));
            return static_cast<std::uint8_t>(10 + (c - L'A'));
        }

        /// Parses exactly @p digits hex characters. The exact width is the point: a GUID field of
        /// the wrong length is a malformed GUID, not a short one.
        [[nodiscard]] constexpr std::optional<std::uint32_t> parse_hex(
            std::wstring_view text, std::size_t digits) noexcept
        {
            if (text.size() != digits || digits > 8)
                return std::nullopt;

            std::uint32_t value = 0;
            for (const wchar_t c : text)
            {
                if (!is_hex(c))
                    return std::nullopt;
                value = (value << 4) | hex_value(c);
            }
            return value;
        }

    } // namespace detail_guid

    /**
     * @brief Parses `{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}`, braces optional.
     * @return The GUID, or nothing if the text is not one. Never throws and never partially fills.
     */
    [[nodiscard]] inline std::optional<GUID> try_parse_guid(std::wstring_view text) noexcept
    {
        using detail_guid::parse_hex;

        if (!text.empty() && text.front() == L'{')
        {
            if (text.size() < 2 || text.back() != L'}')
                return std::nullopt;
            text = text.substr(1, text.size() - 2);
        }

        // 8-4-4-4-12, and nothing else: the field widths are fixed, so the separators are too.
        if (text.size() != 36 ||
            text[8] != L'-' || text[13] != L'-' || text[18] != L'-' || text[23] != L'-')
        {
            return std::nullopt;
        }

        const auto data1 = parse_hex(text.substr(0, 8), 8);
        const auto data2 = parse_hex(text.substr(9, 4), 4);
        const auto data3 = parse_hex(text.substr(14, 4), 4);
        if (!data1 || !data2 || !data3)
            return std::nullopt;

        GUID guid{};
        guid.Data1 = *data1;
        guid.Data2 = static_cast<std::uint16_t>(*data2);
        guid.Data3 = static_cast<std::uint16_t>(*data3);

        // The last two fields are a run of eight bytes, split only by a hyphen.
        const std::wstring_view clock_seq = text.substr(19, 4);
        const std::wstring_view node = text.substr(24, 12);

        for (std::size_t i = 0; i < 8; ++i)
        {
            const std::wstring_view field = i < 2 ? clock_seq : node;
            const std::size_t offset = i < 2 ? i * 2 : (i - 2) * 2;

            const auto byte = parse_hex(field.substr(offset, 2), 2);
            if (!byte)
                return std::nullopt;
            guid.Data4[i] = static_cast<std::uint8_t>(*byte);
        }

        return guid;
    }

    // --------------------------------------------------------------------------------------------
    // Errors
    // --------------------------------------------------------------------------------------------

    /// Translates the HRESULTs the audio APIs actually return into the module's error codes, so
    /// callers can distinguish "someone else owns the device" from "the device vanished" without
    /// depending on Windows headers. The backend wraps the result in an `error`, adding itself.
    [[nodiscard]] inline error_code error_from_hresult(HRESULT hr) noexcept
    {
        switch (hr)
        {
        case S_OK:
            return error_code::none;
        case AUDCLNT_E_DEVICE_INVALIDATED:
            return error_code::device_lost;
        case AUDCLNT_E_DEVICE_IN_USE:
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
            return error_code::device_busy;
        case AUDCLNT_E_UNSUPPORTED_FORMAT:
            return error_code::format_unsupported;
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
            return error_code::no_device;
        case E_OUTOFMEMORY:
            return error_code::platform_error;
        default:
            return error_code::platform_error;
        }
    }

} // namespace catalyst::audio::detail::win32

#endif // _WIN32
