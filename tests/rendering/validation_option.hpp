/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Opt-in Vulkan validation for the rendering tests, driven by `CATALYST_RENDERING_VALIDATION`.
 * @details The tiers that touch synchronisation - pools reset while the GPU might still be reading
 * them, transfers submitted on a queue family other than the one that will read the result - are
 * exactly the ones whose mistakes a functional test cannot see. The bytes come out right; the
 * validation layer is what says the program had no right to expect that.
 *
 * So it is switchable rather than always on: the layers are a developer-machine dependency and cost
 * a large multiple of the runtime, and a test suite that failed on a machine without them installed
 * would be worse than one that quietly does less. Set `CATALYST_RENDERING_VALIDATION=1` and any
 * complaint the layer makes arrives on the console through the module's own logging, at the level
 * the layer assigned it.
 */

#pragma once

#include <catalyst/logging/logging.hpp>
#include <catalyst/logging/sinks/console.hpp>
#include <catalyst/rendering/device.hpp>

#include <cstdlib>
#include <string_view>

namespace catalyst::tests
{

    /** True when `CATALYST_RENDERING_VALIDATION` is set to something other than "0". */
    inline bool validation_requested() noexcept
    {
#if defined(_MSC_VER)
        std::size_t length = 0;
        char buffer[8]{};
        if (getenv_s(&length, buffer, sizeof(buffer), "CATALYST_RENDERING_VALIDATION") != 0 || length == 0)
            return false;
        return std::string_view{buffer} != "0";
#else
        const char *value = std::getenv("CATALYST_RENDERING_VALIDATION");
        return value != nullptr && std::string_view{value} != "0";
#endif
    }

    /**
     * Installs a console sink once, when validation was asked for. Without a sink the backend's
     * diagnostics - the layer's messages included - are routed nowhere, which would make the whole
     * exercise silent.
     */
    inline void install_validation_sink()
    {
        static const bool installed = [] {
            if (validation_requested())
                catalyst::logging::default_logger().add_sink(catalyst::logging::console_sink{});
            return true;
        }();
        (void)installed;
    }

    /** `desc` with validation turned on if it was asked for. */
    inline catalyst::rendering::device_desc with_validation(catalyst::rendering::device_desc desc)
    {
        install_validation_sink();
        desc.enable_validation = validation_requested();
        return desc;
    }

} // namespace catalyst::tests
