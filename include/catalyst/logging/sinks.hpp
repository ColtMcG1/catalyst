/**
 * @file sinks.hpp
 * @brief Every sink the module comes with.
 * @details A convenience over the files in sinks/, for the one place in a program that decides where
 * the log goes. Code that only defines a sink of its own wants sink.hpp, which is the contract
 * without any of the implementations behind it.
 *
 * What is here, and when each one is the answer:
 *
 *   - `console_sink`     one line per event on an ostream, coloured when that means something
 *   - `file_sink`        the same lines appended to a file, flushed as errors are written
 *   - `ring_buffer_sink` the last N events, for a GUI panel to draw each frame
 *   - `callback_sink`    a `std::function`, for one-off sinks in tests and tools
 *   - `async_sink`       any of the above, written by a worker thread instead of the logging thread
 *   - `queued_sink`      any of the above, written by the thread that owns it, when it asks
 *
 * The first four write on the thread that logged. The last two exist because that is often the
 * wrong thread: `async_sink` when the writing is merely slow, `queued_sink` when it has to happen
 * somewhere particular.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/logging/sink.hpp>
#include <catalyst/logging/sinks/async.hpp>
#include <catalyst/logging/sinks/callback.hpp>
#include <catalyst/logging/sinks/console.hpp>
#include <catalyst/logging/sinks/file.hpp>
#include <catalyst/logging/sinks/queued.hpp>
#include <catalyst/logging/sinks/ring_buffer.hpp>
