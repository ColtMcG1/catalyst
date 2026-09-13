/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file rendering.hpp
 * @brief Umbrella header for the Catalyst Rendering library. Including it pulls in the whole public API; individual
 * headers can be included instead to keep compile times down.
 * @details The rendering module exposes a thin, explicit, backend-agnostic layer over Vulkan, D3D12 and Metal. It is
 * organised around one `device` from which every other resource is created, opaque handles for those resources, and
 * `command_list`s that record work for `submit`. See the individual headers for details:
 *   - types.hpp      handles, formats, flag helpers, geometry structs
 *   - error.hpp      error_code and the `error` every fallible call reports through `std::expected`
 *   - events.hpp     device loss and swapchain invalidation, published on a `catalyst::events::bus`
 *   - device.hpp     device creation and adapter queries
 *   - timeline.hpp   `timeline_point`, the moment a submission completes, and `pump`
 *   - queue.hpp      the graphics / compute / copy engines, and `submit`
 *   - buffer.hpp     buffers and `structured_buffer<T>`
 *   - shader.hpp     shader modules from bytecode
 *   - texture.hpp    textures and samplers
 *   - pipeline.hpp   graphics / compute pipeline state objects
 *   - swapchain.hpp  presentable back buffers for a platform window
 *   - command.hpp    command lists, command pools and render passes
 *   - frame.hpp      `frame_ring`: N frames in flight and the pools belonging to each
 *   - transfer.hpp   asynchronous uploads and downloads over the staging ring
 *
 * A minimal frame looks like:
 * @code
 *   using namespace catalyst::rendering;
 *   device dev = create_device();
 *   swapchain sc = create_swapchain(dev, {.window = platform::get_native_handle(w), .extent = {1280, 720}});
 *   command_list cl = create_command_list(dev);
 *
 *   texture back_buffer = acquire_next_image(sc);
 *   begin_recording(cl);
 *   const color_attachment color{.target = back_buffer, .clear = {0.1f, 0.1f, 0.1f, 1.0f}};
 *   begin_render_pass(cl, {.color_attachments = std::span{&color, 1}});
 *   // set_pipeline / set_vertex_buffer / draw ...
 *   end_render_pass(cl);
 *   end_recording(cl);
 *   const timeline_point done = submit(get_queue(dev), cl).value();
 *   present(sc);
 *   pump(dev);                 // resume coroutines, retire what `done` released
 * @endcode
 */

#pragma once

#include <catalyst/rendering/types.hpp>
#include <catalyst/rendering/error.hpp>
#include <catalyst/rendering/events.hpp>
#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/timeline.hpp>
#include <catalyst/rendering/buffer.hpp>
#include <catalyst/rendering/shader.hpp>
#include <catalyst/rendering/texture.hpp>
#include <catalyst/rendering/pipeline.hpp>
#include <catalyst/rendering/swapchain.hpp>
#include <catalyst/rendering/command.hpp>
#include <catalyst/rendering/queue.hpp>
#include <catalyst/rendering/frame.hpp>
#include <catalyst/rendering/transfer.hpp>

/**
 * @namespace catalyst::rendering
 * @brief All types and functions of the Catalyst Rendering library.
 */
namespace catalyst::rendering
{

    /**
     * @fn module_name
     * @brief Returns the name of the compiled-in rendering backend ("vulkan", "d3d12", "metal" or "null").
     */
    const char *module_name();

    /**
     * @fn backend
     * @brief Returns the compiled-in rendering backend as an enumerator.
     */
    [[nodiscard]] backend_kind backend() noexcept;

} // namespace catalyst::rendering
