/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Public device API; validates handles and forwards to the active backend.
 */

#include <catalyst/rendering/device.hpp>

#include "detail_backend.hpp"
#include "detail_sync.hpp"

namespace catalyst::rendering
{

    device create_device(const device_desc &desc)
    {
        const detail::exclusive_guard guard;
        return device{detail::create_device(desc)};
    }

    void destroy_device(device &d) noexcept
    {
        if (!d)
            return;
        {
            const detail::exclusive_guard guard;
            detail::destroy_device(d.id());
        }
        d = device{};
    }

    bool is_valid(const device &d) noexcept
    {
        if (!d)
            return false;
        const detail::shared_guard guard;
        return detail::is_device_valid(d.id());
    }

    bool is_device_lost(const device &d) noexcept
    {
        // An invalid handle is not a lost device; it is no device. Saying "lost" would send a
        // caller down the rebuild path for what is actually a bug in their handle bookkeeping.
        if (!d)
            return false;
        const detail::shared_guard guard;
        return detail::is_device_lost(d.id());
    }

    device_info get_device_info(const device &d) noexcept
    {
        if (!d)
            return {};
        const detail::shared_guard guard;
        return detail::get_device_info(d.id());
    }

    void wait_idle(const device &d) noexcept
    {
        if (!d)
            return;
        // No guard: this blocks for as long as the GPU takes, and holding the module lock across it
        // would stop every other thread creating a resource for exactly that long. The backend
        // takes and drops the lock around the blocking part itself; see detail_backend.hpp.
        detail::wait_idle(d.id());
    }

} // namespace catalyst::rendering
