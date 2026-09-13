/**
 * @file audio.hpp
 * @brief Umbrella header for the catalyst::audio module.
 * @details Including this pulls in the whole module: the vocabulary, the error type, backend and
 * device enumeration, the real-time block and renderer, both stream types, and the events. A
 * program that only needs part of it can include that part instead - code that just renders wants
 * block.hpp, a device picker wants device.hpp, and a test wants offline.hpp.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/audio/backend.hpp>
#include <catalyst/audio/block.hpp>
#include <catalyst/audio/command.hpp>
#include <catalyst/audio/device.hpp>
#include <catalyst/audio/error.hpp>
#include <catalyst/audio/events.hpp>
#include <catalyst/audio/mixer.hpp>
#include <catalyst/audio/offline.hpp>
#include <catalyst/audio/ring.hpp>
#include <catalyst/audio/sound.hpp>
#include <catalyst/audio/stream.hpp>
#include <catalyst/audio/types.hpp>

/**
 * @namespace catalyst::audio
 * @brief Audio devices and the real-time thread that feeds them.
 * @details A program opens a @ref catalyst::audio::stream against a device, hands it a
 * @ref catalyst::audio::renderer, and starts it. From then on the driver calls the renderer on its
 * own thread, once per block, under a hard deadline - that thread and its rules are the module's
 * defining constraint, and block.hpp states them.
 *
 * Everything else exists to keep that thread simple. Devices are enumerated by free functions
 * before anything is open. Failures are @ref catalyst::audio::error values rather than exceptions,
 * because losing a device is ordinary. Device topology changes are queued as they happen and
 * published to a `catalyst::events::bus` from `pump()`, on the caller's thread, so no listener ever
 * runs somewhere surprising. And @ref catalyst::audio::offline_stream renders the same code with no
 * hardware at all, one block at a time, on the calling thread - which is what makes the audio path
 * testable in CI.
 *
 *     auto render = [&synth](audio::render_block &block) noexcept { synth.fill(block); };
 *
 *     audio::stream_config cfg;
 *     cfg.sample_rate = 48000;
 *     cfg.output_channels = 2;
 *
 *     auto stream = audio::stream::open(cfg, render);
 *     if (!stream)
 *     {
 *         log::critical("{}", stream.error());
 *         return;
 *     }
 *
 *     stream->start();
 *     while (running)
 *         stream->pump();
 */
namespace catalyst::audio
{

    /**
     * @fn module_name
     * @brief Returns the name of this module as a string. This can be used for logging, debugging,
     * or any situation where you want to identify the module by name.
     * @return A string literal representing the name of this module.
     */
    const char *module_name();

} // namespace catalyst::audio
