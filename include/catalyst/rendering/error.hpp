/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief The failure value every fallible rendering operation returns through `std::expected`.
 * @details Losing a device to a driver update, being handed a swapchain the compositor has already
 * invalidated, or asking an adapter for a format it does not implement are ordinary outcomes for a
 * program that talks to a GPU - not exceptional ones - so failures are values here, the way
 * `catalyst::audio` and `resource::json` report theirs.
 *
 * What this replaces is the reason it exists. Until now a rendering failure was an invalid handle
 * or a `false`, and the reason went to `stderr` through a varargs `report()` that no caller could
 * read. Three of those returns were the same value for outcomes needing three different responses:
 * `acquire_next_image` returned an invalid texture whether the window was minimised (skip the
 * frame), the swapchain was out of date (resize, then carry on) or the device was gone (tear
 * everything down and rebuild). @ref error_code::not_ready, @ref error_code::swapchain_out_of_date
 * and @ref error_code::device_lost are those three, told apart.
 *
 * An @ref error is a code plus the little that makes the code actionable. The code alone is enough
 * to branch on; the rest is what a log line needs to be worth reading, and it is why
 * @ref backend_result_name is here - `VK_ERROR_DEVICE_LOST` tells a reader what happened where
 * `-4` does not. Nothing in the struct allocates or owns anything: the two string members point at
 * backend literals with static lifetime. Only @ref error::message allocates, and only when a caller
 * asks for a sentence.
 *
 * There is no `backend` field, unlike `audio::error`. Audio chooses its backend at run time and so
 * must say which one failed; rendering is compiled against exactly one, so @ref backend already
 * answers the question and @ref error::message prefixes it for free.
 */

#pragma once

#include <catalyst/rendering/types.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace catalyst::rendering
{

    /**
     * @enum error_code
     * @brief What went wrong.
     * @details Enumerators are appended, never reordered or reused: a saved log or a bug report may
     * name one by value.
     */
    enum class error_code : std::uint8_t
    {
        /** @brief No failure. Never carried by a returned @ref error; present so the struct has a
         * meaningful default. */
        none = 0,

        /** @brief The requested backend was not compiled into this build. Compare @ref backend to
         * find out which one was. */
        backend_unavailable,

        /** @brief No adapter matched the request, or the machine has none this backend can drive.
         * A `prefer_discrete_adapter` build on a laptop with the GPU disabled ends up here. */
        no_adapter,

        /**
         * @brief The device was removed or reset: a driver update, a TDR, an external GPU
         * unplugged, or a hang the driver recovered from by throwing the context away.
         * @details Every handle created from that device is dead. Nothing is recoverable in place;
         * rebuild from `create_device` down. Also published as a @ref device_lost_event.
         */
        device_lost,

        /**
         * @brief The window's surface is gone - the window was destroyed under the swapchain.
         * @details Distinct from @ref device_lost: the device is fine and everything not attached
         * to that window survives.
         */
        surface_lost,

        /**
         * @brief The swapchain no longer matches its surface and must be recreated before it will
         * present again, most often because the window was resized.
         * @details The correct response is `resize_swapchain` with the window's current client
         * size, then carry on - not to treat the frame as an error. Also published as a
         * @ref swapchain_out_of_date_event.
         */
        swapchain_out_of_date,

        /**
         * @brief Nothing is available right now, and nothing is wrong.
         * @details A minimised window has no back buffer to hand out; a non-blocking wait found its
         * work still running. Try again next frame. This is the code that keeps "not yet" from
         * having to look like a failure.
         */
        not_ready,

        /** @brief The adapter ran out of video memory. */
        out_of_device_memory,

        /** @brief The process ran out of host memory serving a graphics allocation. */
        out_of_host_memory,

        /**
         * @brief The description contains values no backend could honour: a zero-sized buffer, a
         * swapchain with no images, a slot index past `max_texture_slots`.
         * @details A programming error, reported rather than asserted so a release build fails
         * predictably.
         */
        invalid_argument,

        /** @brief The adapter does not implement the requested @ref format for the requested use.
         * */
        unsupported_format,

        /**
         * @brief The backend understands what was asked and cannot serve it - a `queue_type` this
         * adapter exposes no family for, say.
         * @details Not a programming error and not a driver failure; a capability gap. Query first
         * where a query exists.
         */
        unsupported_operation,

        /**
         * @brief Shader bytecode was rejected: malformed, or the wrong dialect for this backend.
         * @details Check `native_bytecode_format(backend())` before blaming the shader - handing
         * SPIR-V to D3D12 lands here.
         */
        shader_invalid,

        /** @brief The driver refused to build a pipeline from an otherwise valid description. */
        pipeline_creation_failed,

        /**
         * @brief A bounded wait expired before the work it named completed.
         * @details Carried by `timeline_point::wait_for` from Tier 2 on. A timeout is not device
         * loss: the work may still complete. Waiting again is legitimate.
         */
        timeout,

        /**
         * @brief The staging ring has no room left for this transfer, because everything in it is
         * still being read by the GPU.
         * @details Back-pressure, not a failure: the ring is a fixed budget set by
         * `device_desc::staging_ring_bytes`, and this is what it says when a caller is queueing
         * uploads faster than the copy queue drains them. The responses that work are to wait on
         * an earlier transfer's @ref timeline_point, to submit the batch already accumulated and
         * start another, or to raise the budget at device creation. Splitting one oversized upload
         * is the fix when a *single* transfer is larger than the whole ring, which is the one case
         * waiting will never clear.
         */
        staging_exhausted,

        /** @brief The graphics API returned a failure with no better mapping. @ref
         * error::backend_result_name says which. */
        platform_error,
    };

    /**
     * @brief A short, stable description of a code. Never empty.
     * @details A sentence fragment, lowercase, that reads correctly after `"vulkan: "` - so a
     * caller can log `"{}"` on an @ref error and get a whole message.
     */
    [[nodiscard]] constexpr std::string_view name(error_code code) noexcept
    {
        switch (code)
        {
        case error_code::none:
            return "no error";
        case error_code::backend_unavailable:
            return "requested backend is not in this build";
        case error_code::no_adapter:
            return "no suitable graphics adapter";
        case error_code::device_lost:
            return "device was lost";
        case error_code::surface_lost:
            return "window surface was lost";
        case error_code::swapchain_out_of_date:
            return "swapchain is out of date and must be recreated";
        case error_code::not_ready:
            return "not ready yet";
        case error_code::out_of_device_memory:
            return "out of device memory";
        case error_code::out_of_host_memory:
            return "out of host memory";
        case error_code::invalid_argument:
            return "description is invalid";
        case error_code::unsupported_format:
            return "adapter does not support this format";
        case error_code::unsupported_operation:
            return "backend does not support this operation";
        case error_code::shader_invalid:
            return "shader bytecode was rejected";
        case error_code::pipeline_creation_failed:
            return "pipeline could not be created";
        case error_code::timeout:
            return "wait timed out";
        case error_code::staging_exhausted:
            return "staging ring is full";
        case error_code::platform_error:
            return "graphics API failure";
        }
        return "unknown rendering error";
    }

    /**
     * @brief True for the codes that mean "nothing is wrong, come back later".
     * @details Worth a helper because the frame loop tests it every frame: none of
     * @ref error_code::not_ready, @ref error_code::swapchain_out_of_date, @ref error_code::timeout
     * or @ref error_code::staging_exhausted should reach a log at error level, and all four are
     * handled by continuing rather than by unwinding.
     *
     * "Come back later" does not promise that later will do. A single transfer larger than the whole
     * staging ring reports @ref error_code::staging_exhausted every time it is retried; the ceiling
     * is in `get_staging_info`.
     */
    [[nodiscard]] constexpr bool is_transient(error_code code) noexcept
    {
        return code == error_code::not_ready || code == error_code::swapchain_out_of_date ||
               code == error_code::timeout || code == error_code::staging_exhausted;
    }

    /**
     * @brief True for the codes after which every handle from the device is dead.
     */
    [[nodiscard]] constexpr bool is_fatal(error_code code) noexcept
    {
        return code == error_code::device_lost;
    }

    /**
     * @struct error
     * @brief The failure value of every fallible operation in the module.
     * @details Compare on @ref code to branch; call @ref message to report. The context members are
     * filled in where the backend knows them and left at their defaults where it does not, so a
     * reader should treat a zero or a null as "not known" rather than as a value.
     *
     * Trivially copyable and allocation-free. @ref backend_result_name and @ref operation point at
     * literals owned by the backend, which outlive any @ref error built from them; never store a
     * copy of the pointer expecting to own it, and never build one from a `std::string`.
     */
    struct error
    {
        /** @brief What went wrong. */
        error_code code = error_code::none;

        /**
         * @brief The graphics API's own result, as it returned it - a `VkResult` or an `HRESULT`.
         * @details Zero when the failure was detected by the module rather than by the driver
         * (@ref error_code::invalid_argument, say). A `VkResult` of 0 is `VK_SUCCESS`, so zero is
         * never a meaningful failure value on either backend.
         */
        std::int32_t backend_result = 0;

        /**
         * @brief That result spelled the way the API spells it - `"VK_ERROR_DEVICE_LOST"`,
         * `"DXGI_ERROR_DEVICE_REMOVED"`. Null when unknown.
         * @details The reason this is here and not left to the reader: a bug report saying
         * `VK_ERROR_DEVICE_LOST` is searchable, and one saying `-4` is not.
         */
        const char *backend_result_name = nullptr;

        /**
         * @brief The API call that failed - `"vkCreateGraphicsPipelines"`. Null when the failure
         * was not one call's fault.
         */
        const char *operation = nullptr;

        /**
         * @brief The error as a sentence, e.g.
         * `"vulkan: device was lost (vkQueueSubmit returned VK_ERROR_DEVICE_LOST)"`.
         * @return A freshly allocated string. This is the only thing in the type that allocates,
         * and only when a caller asks for it.
         */
        [[nodiscard]] std::string message() const;

        [[nodiscard]] friend bool operator==(const error &, const error &) noexcept = default;
    };

    /** @brief Builds an @ref error carrying only a code, for the failures with nothing to add. */
    [[nodiscard]] constexpr error make_error(error_code code) noexcept
    {
        return error{code, 0, nullptr, nullptr};
    }

    /** @brief Builds an @ref error for a failure the module detected itself, naming the call. */
    [[nodiscard]] constexpr error make_error(error_code code, const char *operation) noexcept
    {
        return error{code, 0, nullptr, operation};
    }

} // namespace catalyst::rendering

/** @brief Formats a code as its description, so `logging::error<c>("{}", err.code)` works. */
template <>
struct std::formatter<catalyst::rendering::error_code> : std::formatter<std::string_view>
{
    template <typename Context>
    auto format(catalyst::rendering::error_code code, Context &ctx) const
    {
        return std::formatter<std::string_view>::format(catalyst::rendering::name(code), ctx);
    }
};

/**
 * @brief Formats an error as its full message, so `logging::critical<c>("{}", r.error())` works.
 * @details This is the one call sites want: it names the backend and the failing call, where the
 * code on its own would not.
 */
template <>
struct std::formatter<catalyst::rendering::error> : std::formatter<std::string>
{
    template <typename Context>
    auto format(const catalyst::rendering::error &err, Context &ctx) const
    {
        return std::formatter<std::string>::format(err.message(), ctx);
    }
};
