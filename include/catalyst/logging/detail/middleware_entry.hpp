/**
 * @file middleware_entry.hpp
 * @brief The router's type-erased view of a registered middleware.
 * @details The same trick as sink_entry: the callable is erased once, at registration, so that
 * everything the router does with the chain afterwards is ordinary code compiled once.
 *
 * Entries are held by `shared_ptr` in a copy-on-write list, which is what makes removal safe without
 * any waiting. Removing a middleware builds a list without it, but a dispatch already running holds
 * a `shared_ptr` to the old list, so both the entry and the callable inside it stay alive until that
 * dispatch is finished with them. The `active` flag is what stops the same dispatch from starting it
 * again on the way through.
 *
 * The chain runner erases itself too. A middleware is called with the event, an opaque context, and
 * a function pointer that resumes the chain from that context - so `next` can be a small object with
 * no idea what a middleware list is, and the runner can live in router.cpp.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>
#include <catalyst/logging/middleware.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace catalyst::logging::detail
{

    /// How a middleware hands the event on: the chain's position, and the runner to hand it back to.
    using resume_fn = void (*)(void *, log_event &);

    /// The erased middleware, in the one shape both registration forms are wrapped into.
    using middleware_fn = std::move_only_function<void(log_event &, void *, resume_fn)>;

    /**
     * @struct middleware_entry
     * @brief One registered middleware: its id, where it sits in the chain, and how to call it.
     */
    struct middleware_entry
    {
        middleware_id id = no_middleware;

        /// Descending: the highest priority is the outermost layer of the chain.
        int priority = 0;

        /// Cleared by `remove_middleware`. A dispatch that sees it false steps over the entry.
        std::atomic<bool> active{true};

        middleware_fn invoke{};

        middleware_entry(middleware_id id, int priority, middleware_fn invoke)
            : id(id), priority(priority), invoke(std::move(invoke))
        {
        }
    };

    /**
     * The router's middleware chain. Copy-on-write, so a dispatch can hold a snapshot without a
     * lock, and entries are shared so a removal cannot pull one out from under a dispatch.
     */
    using middleware_list = std::vector<std::shared_ptr<middleware_entry>>;

} // namespace catalyst::logging::detail
