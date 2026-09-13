/**
 * @file device.cpp
 * @brief Endpoint enumeration and selection, with no stream involved.
 * @details Each of these constructs a backend, asks it, and throws it away. That is cheap for every
 * backend the module has - enumeration opens no device and starts no thread - and it is what lets a
 * device picker exist before the program has decided what to play.
 * License: MIT (see LICENSE).
 */

#include "detail_backend.hpp"

#include <catalyst/audio/device.hpp>

#include <algorithm>

namespace catalyst::audio
{

    std::expected<std::vector<device_info>, error> devices()
    {
        return devices(backend_kind::automatic);
    }

    std::expected<std::vector<device_info>, error> devices(backend_kind backend)
    {
        const backend_kind resolved = detail::resolve(backend);

        // The offline renderer has no endpoints rather than no answer: it is a real thing to render
        // with, it simply is not a device. Reporting `backend_unavailable` would say the opposite.
        if (resolved == backend_kind::offline)
            return std::vector<device_info>{};

        auto enumerator = detail::create_enumerator(resolved);
        if (!enumerator)
            return std::unexpected(enumerator.error());

        return (*enumerator)->enumerate_devices();
    }

    std::expected<device_info, error> default_device(stream_direction direction, backend_kind backend)
    {
        auto list = devices(backend);
        if (!list)
            return std::unexpected(list.error());

        if (const device_info *found = find_device(*list, device_selector::system_default(), direction))
            return *found;

        // Some backends report no default at all - ASIO has no notion of one - so fall back to the
        // first endpoint that could actually serve the direction rather than failing outright.
        const auto usable = std::ranges::find_if(
            *list, [direction](const device_info &device) { return device.supports(direction); });

        if (usable != list->end())
            return *usable;

        return std::unexpected(make_error(error_code::no_device, detail::resolve(backend)));
    }

    const device_info *find_device(
        const std::vector<device_info> &list,
        const device_selector &selector,
        stream_direction direction) noexcept
    {
        for (const device_info &device : list)
        {
            if (device.supports(direction) && selector.matches(device))
                return &device;
        }

        return nullptr;
    }

} // namespace catalyst::audio
