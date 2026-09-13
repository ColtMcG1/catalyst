/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Public queue API: validates a submission and forwards it to the active backend.
 * @details Validation lives here rather than in each backend so every backend gets the same answers
 * to the same mistakes, and so a backend may assume by the time it is called that the lists exist,
 * are closed, belong to this device, and were recorded for a kind this queue can run.
 */

#include <catalyst/rendering/queue.hpp>

#include "detail_backend.hpp"
#include "detail_log.hpp"
#include "detail_sync.hpp"

namespace catalyst::rendering
{

    namespace
    {
        /**
         * Checks everything a backend is entitled to assume about a submission.
         * @details Returns the first problem found rather than accumulating them: a caller
         * submitting a list from another device has one bug, not five.
         */
        std::expected<void, error> validate(const queue &q, const submit_info &info)
        {
            if (!q)
                return std::unexpected(make_error(error_code::invalid_argument, "submit"));
            if (!detail::is_device_valid(q.owner().id()))
                return std::unexpected(make_error(error_code::invalid_argument, "submit"));
            if (detail::is_device_lost(q.owner().id()))
                return std::unexpected(make_error(error_code::device_lost, "submit"));

            for (const command_list &cl : info.lists)
            {
                if (!cl || !detail::is_command_list_valid(cl.id()))
                    return std::unexpected(make_error(error_code::invalid_argument, "submit"));
                if (detail::is_recording(cl.id()))
                    return std::unexpected(make_error(error_code::invalid_argument, "submit"));

                const command_list_desc desc = detail::get_command_list_desc(cl.id());
                if (desc.queue != q.kind())
                {
                    // Worth a line rather than a bare code: the mismatch is between two decisions
                    // made in different places, and this is the one rule whose violation the driver
                    // would otherwise punish with undefined behaviour rather than an error. A
                    // command buffer belongs to the family its pool came from; there is no queue it
                    // can be "promoted" to.
                    logging::error<detail::render_log>(
                        "submit: a list recorded for the {} queue cannot be submitted to the {} queue",
                        to_string(desc.queue), to_string(q.kind()));
                    return std::unexpected(make_error(error_code::invalid_argument, "submit"));
                }
            }

            for (const timeline_point &point : info.wait)
            {
                // An invalid point is "no dependency", so a caller can pass one unconditionally.
                if (point.valid() && point.owner() != q.owner())
                    return std::unexpected(make_error(error_code::invalid_argument, "submit"));
            }

            return {};
        }
    } // namespace

    queue get_queue(const device &dev, queue_kind kind) noexcept
    {
        if (!dev)
            return {};
        return queue{dev, kind};
    }

    queue_info get_queue_info(const queue &q) noexcept
    {
        if (!q)
            return {};
        const detail::shared_guard guard;
        return detail::get_queue_info(q.owner().id(), q.kind());
    }

    std::expected<timeline_point, error> submit(const queue &q, const submit_info &info)
    {
        // One exclusive guard covering validation and the submission itself. Not two, and not a
        // shared one upgraded to an exclusive one: `std::shared_mutex` has no upgrade, and
        // validating under one lock and submitting under another would leave a window in which a
        // list this call just approved is destroyed by another thread.
        const detail::exclusive_guard guard;

        if (auto ok = validate(q, info); !ok)
            return std::unexpected(ok.error());

        const auto value = detail::submit(q.owner().id(), q.kind(), info.lists, info.wait);
        if (!value)
            return std::unexpected(value.error());

        return timeline_point{q.owner(), q.kind(), *value};
    }

    std::expected<timeline_point, error> submit(const queue &q, const command_list &cl)
    {
        return submit(q, submit_info{.lists = {&cl, 1}, .wait = {}});
    }

    timeline_point last_submitted(const queue &q) noexcept
    {
        if (!q)
            return {};
        const detail::shared_guard guard;
        return timeline_point{q.owner(), q.kind(), detail::queue_last_submitted(q.owner().id(), q.kind())};
    }

    timeline_point completed(const queue &q) noexcept
    {
        if (!q)
            return {};
        const detail::shared_guard guard;
        return timeline_point{q.owner(), q.kind(), detail::queue_completed(q.owner().id(), q.kind())};
    }

    std::expected<void, error> wait_idle(const queue &q)
    {
        if (!q)
            return std::unexpected(make_error(error_code::invalid_argument, "wait_idle"));
        return last_submitted(q).wait();
    }

} // namespace catalyst::rendering
