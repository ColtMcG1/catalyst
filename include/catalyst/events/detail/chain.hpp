#pragma once

#include "registry.hpp"

#include <cstddef>

namespace catalyst::events::detail
{

    /**
     * @struct chain_ctx
     * @brief The position of a synchronous dispatch within its middleware chain.
     */
    struct chain_ctx
    {
        const sync_middleware_list *middleware;
        const sync_listener_list *listeners;
        std::size_t index;
    };

    /**
     * @struct async_chain_ctx
     * @brief The position of an asynchronous dispatch within its middleware chain.
     */
    struct async_chain_ctx
    {
        const async_middleware_list *middleware;
        const async_listener_list *listeners;
        std::size_t index;
    };

    inline bool live(const slot_base &s) noexcept { return s.active.load(std::memory_order_acquire); }

    // Runs middleware [index, end) as an onion around the listeners. Each
    // layer gets a fresh context pointing at the next layer, so calling next
    // recurses into this function with index + 1.
    inline void run_chain(void *raw, void *ev)
    {
        auto &c = *static_cast<chain_ctx *>(raw);
        if (c.middleware)
        {
            for (std::size_t i = c.index; i < c.middleware->size(); ++i)
            {
                auto &mw = *(*c.middleware)[i];
                if (!live(mw))
                    continue;
                chain_ctx rest{c.middleware, c.listeners, i + 1};
                mw.fn(ev, &rest, &run_chain);
                return;
            }
        }

        if (c.listeners)
            for (const auto &s : *c.listeners)
                if (live(*s))
                    s->fn(ev);
    }

    inline task<void> run_chain_async(void *raw, void *ev)
    {
        auto &c = *static_cast<async_chain_ctx *>(raw);
        if (c.middleware)
        {
            for (std::size_t i = c.index; i < c.middleware->size(); ++i)
            {
                auto &mw = *(*c.middleware)[i];
                if (!live(mw))
                    continue;
                async_chain_ctx rest{c.middleware, c.listeners, i + 1};
                co_await mw.fn(ev, &rest, &run_chain_async);
                co_return;
            }
        }

        if (c.listeners)
            for (const auto &s : *c.listeners)
                if (live(*s))
                    co_await s->fn(ev);
    }

} // namespace catalyst::events::detail
