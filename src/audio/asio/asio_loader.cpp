/**
 * @file asio_loader.cpp
 * @brief Registry enumeration of installed ASIO drivers, and the DLL-and-class-factory dance that
 * reaches one without the COM runtime.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "asio_loader.h"

#if defined(_WIN32)

#  include <winreg.h>

#  include <utility>

namespace catalyst::audio::asio
{

    namespace
    {

        /// Reads one REG_SZ value, or an empty string if it is absent or is not a string.
        std::wstring read_registry_string(HKEY root, const wchar_t *subkey, const wchar_t *value)
        {
            DWORD bytes = 0;
            if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
                bytes == 0)
            {
                return {};
            }

            std::wstring text(bytes / sizeof(wchar_t), L'\0');
            if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, text.data(), &bytes) != ERROR_SUCCESS)
                return {};

            // RegGetValueW counts the terminator; a std::wstring does not want it.
            if (const auto terminator = text.find(L'\0'); terminator != std::wstring::npos)
                text.resize(terminator);

            return text;
        }

        void enumerate_asio_key(HKEY root, const wchar_t *key_path, std::vector<installed_driver> &out)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, key_path, 0, KEY_READ, &key) != ERROR_SUCCESS)
                return;

            for (DWORD index = 0;; ++index)
            {
                wchar_t name[256] = {};
                auto length = static_cast<DWORD>(std::size(name));

                const LSTATUS status =
                    RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
                if (status == ERROR_NO_MORE_ITEMS)
                    break;
                if (status != ERROR_SUCCESS)
                    continue;

                const std::wstring subkey = std::wstring(key_path) + L'\\' + std::wstring(name, length);

                installed_driver installed;
                installed.name.assign(name, length);
                installed.dll_path = read_registry_string(root, subkey.c_str(), L"DLL");

                const auto clsid = detail::win32::try_parse_guid(
                    read_registry_string(root, subkey.c_str(), L"CLSID"));

                // An entry without a loadable DLL and a real CLSID is not a device a caller could
                // ever open, so it is not offered as one.
                if (installed.dll_path.empty() || !clsid)
                    continue;

                installed.clsid = *clsid;
                out.push_back(std::move(installed));
            }

            RegCloseKey(key);
        }

    } // namespace

    std::vector<installed_driver> enumerate_installed_drivers()
    {
        std::vector<installed_driver> drivers;
        enumerate_asio_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", drivers);
        enumerate_asio_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\ASIO", drivers);
        return drivers;
    }

    driver::~driver() { close(); }

    driver::driver(driver &&other) noexcept
        : module_(std::exchange(other.module_, nullptr)), instance_(std::move(other.instance_))
    {
    }

    driver &driver::operator=(driver &&other) noexcept
    {
        if (this != &other)
        {
            close();
            module_ = std::exchange(other.module_, nullptr);
            instance_ = std::move(other.instance_);
        }
        return *this;
    }

    void driver::close() noexcept
    {
        // Ordered: the interface's vtable lives in the module, so the reference goes first.
        instance_.reset();

        if (auto *module = std::exchange(module_, nullptr))
            FreeLibrary(module);
    }

    std::expected<void, error_code> driver::open(const installed_driver &installed) noexcept
    {
        close();

        module_ = LoadLibraryW(installed.dll_path.c_str());
        if (!module_)
            return std::unexpected(error_code::no_device);

        const auto get_class_object =
            reinterpret_cast<dll_get_class_object_fn>(GetProcAddress(module_, "DllGetClassObject"));
        if (!get_class_object)
        {
            close();
            return std::unexpected(error_code::no_device);
        }

        detail::win32::com_ptr<iclass_factory> factory;
        if (get_class_object(installed.clsid, iid_iunknown, factory.put_void()) < 0 || !factory)
        {
            close();
            return std::unexpected(error_code::platform_error);
        }

        // Drivers hand back their `IASIO` for IID_IUnknown - the interface is what the class *is*,
        // and asking for IID_IASIO would need the SDK's IID, which is the dependency being avoided.
        if (factory->CreateInstance(nullptr, iid_iunknown, instance_.put_void()) < 0 || !instance_)
        {
            close();
            return std::unexpected(error_code::platform_error);
        }

        return {};
    }

} // namespace catalyst::audio::asio

#endif // _WIN32
