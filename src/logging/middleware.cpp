/**
 * @file middleware.cpp
 * @brief Implements `scoped_middleware`. The chain itself is run in router.cpp.
 * @details The counterpart of sink.cpp, and here for the same reason: the guard needs the router's
 * definition, which middleware.hpp does not have and should not need.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/logging/middleware.hpp>

#include <catalyst/logging/router.hpp>

#include <utility>

namespace catalyst::logging
{

    scoped_middleware::scoped_middleware(scoped_middleware &&other) noexcept
        : router_(std::exchange(other.router_, nullptr)), id_(std::exchange(other.id_, no_middleware))
    {
    }

    scoped_middleware &scoped_middleware::operator=(scoped_middleware &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            router_ = std::exchange(other.router_, nullptr);
            id_ = std::exchange(other.id_, no_middleware);
        }
        return *this;
    }

    scoped_middleware::~scoped_middleware()
    {
        reset();
    }

    void scoped_middleware::reset() noexcept
    {
        if (router_ != nullptr && id_ != no_middleware)
            router_->remove_middleware(id_);
        router_ = nullptr;
        id_ = no_middleware;
    }

    middleware_id scoped_middleware::release() noexcept
    {
        router_ = nullptr;
        return std::exchange(id_, no_middleware);
    }

} // namespace catalyst::logging
