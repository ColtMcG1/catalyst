/**
 * @file file.cpp
 * @brief Implements `file_sink`.
 * @details The line is rendered before the lock is taken, because another sink may already have it
 * in the shared cache and, if not, the formatting is the same work whoever ends up doing it - and
 * doing it inside the lock would make every other logging thread wait for it.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/logging/sinks/file.hpp>

#include <stdexcept>
#include <string>

namespace catalyst::logging
{

    namespace
    {
        /// A file gets no escape sequences, whatever the console is doing.
        const line_style plain{};
    } // namespace

    file_sink::file_sink(const std::filesystem::path &path, line_format format)
        : format_(format), file_(path, std::ios::app)
    {
        if (!file_)
            throw std::runtime_error("catalyst::logging: cannot open " + path.string());
    }

    void file_sink::write(const log_event &event)
    {
        line_cache cache(event);
        write(event, cache);
    }

    void file_sink::write(const log_event &event, line_cache &cache)
    {
        // Rendered before the lock is taken: another sink may already have this line, and if not,
        // the formatting is the same work whoever ends up doing it.
        const std::string &line = cache.line(format_, plain);
        std::scoped_lock lock(mutex_);
        file_ << line << '\n';
        // Anything this bad may be the last thing written before the process goes.
        if (event.level >= log_level::error)
            file_.flush();
    }

    void file_sink::flush()
    {
        std::scoped_lock lock(mutex_);
        file_.flush();
    }

} // namespace catalyst::logging
