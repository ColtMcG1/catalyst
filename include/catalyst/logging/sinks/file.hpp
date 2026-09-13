/**
 * @file file.hpp
 * @brief The same lines as the console, appended to a file.
 * @details The sink that outlives the process, which is the point of it: whatever is in the file
 * when the window closes is what there is to go on afterwards. So lines at error and above are
 * flushed as they are written, rather than sitting in a buffer the crash takes with it.
 *
 *     router.emplace_sink<file_sink>("app.log");
 *
 * `emplace_sink` rather than `add_sink`, because the sink holds a mutex and so cannot be moved into
 * the router after the fact.
 *
 * It declares `reentrant` on the strength of its own lock, but that lock is held across a write to
 * the filesystem, on the thread that logged. A frame loop wants this behind an `async_sink`.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>
#include <catalyst/logging/format.hpp>
#include <catalyst/logging/sink.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>

namespace catalyst::logging
{

    /**
     * @class file_sink
     * @brief The same lines as the console, appended to a file.
     * @details Lines are flushed on every write at error and above, and on `flush()`. Holds its own
     * lock, so it declares `reentrant` - but it is still a filesystem write on the logging thread,
     * which is what `async_sink` exists for.
     */
    class file_sink : public sink_contract<sink_threading::reentrant>
    {
    public:
        /**
         * @brief Opens `path` for appending.
         * @param path The file to append to; it is created when it does not exist.
         * @param format Which columns to print.
         * @throws std::runtime_error When the file cannot be opened.
         */
        explicit file_sink(const std::filesystem::path &path, line_format format = {});

        void write(const log_event &event);

        /// Takes the rendered line from `cache`, so sinks configured alike format the event once.
        void write(const log_event &event, line_cache &cache);

        void flush();

    private:
        line_format format_;
        std::mutex mutex_;
        std::ofstream file_;
    };

} // namespace catalyst::logging
