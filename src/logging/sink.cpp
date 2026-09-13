/**
 * @file sink.cpp
 * @brief Implements `scoped_sink`, the only part of sink.hpp that is not a type-level declaration.
 * @details It lives here rather than in router.cpp because the guard is part of what a sink is -
 * the answer to "this sink is going away, stop logging to it" - and because it needs the router's
 * definition, which sink.hpp deliberately does not have.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/logging/sink.hpp>

#include <catalyst/logging/router.hpp>

#include <utility>

namespace catalyst::logging
{

    scoped_sink::scoped_sink(scoped_sink &&other) noexcept
        : router_(std::exchange(other.router_, nullptr)), id_(std::exchange(other.id_, no_sink))
    {
    }

    scoped_sink &scoped_sink::operator=(scoped_sink &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            router_ = std::exchange(other.router_, nullptr);
            id_ = std::exchange(other.id_, no_sink);
        }
        return *this;
    }

    scoped_sink::~scoped_sink()
    {
        reset();
    }

    void scoped_sink::reset() noexcept
    {
        if (router_ != nullptr && id_ != no_sink)
            router_->remove_sink(id_);
        router_ = nullptr;
        id_ = no_sink;
    }

    sink_id scoped_sink::release() noexcept
    {
        router_ = nullptr;
        return std::exchange(id_, no_sink);
    }

} // namespace catalyst::logging
