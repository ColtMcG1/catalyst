#pragma once

#include "../tag.hpp"
#include "../task.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

namespace catalyst::events::detail
{

    /**
     * @enum kind
     * @brief Identifies which registry a registration lives in, so that a token can
     * remove it without knowing its event type at compile time.
     */
    enum class kind
    {
        sync_listener,
        async_listener,
        sync_middleware,
        async_middleware
    };

    using sync_listener_fn = std::move_only_function<void(const void *)>;
    using async_listener_fn = std::move_only_function<task<void>(const void *)>;
    using sync_middleware_fn = std::move_only_function<void(void *, void *, void (*)(void *, void *))>;
    using async_middleware_fn = std::move_only_function<task<void>(void *, void *, task<void> (*)(void *, void *))>;

    /**
     * @struct slot_base
     * @brief The type-erased part of a registration, shared with tokens.
     */
    struct slot_base
    {
        std::size_t id;
        int priority;
        std::atomic<bool> active{true};
    };

    /**
     * @struct slot
     * @brief A registration together with the callable it holds.
     * @tparam Fn The erased callable type for this registry.
     */
    template <typename Fn>
    struct slot : slot_base
    {
        Fn fn;

        slot(std::size_t id, int priority, Fn fn) : slot_base{id, priority}, fn(std::move(fn)) {}
    };

    /**
     * @struct registry
     * @brief One table per callable kind, keyed by event type.
     * @tparam Fn The erased callable type stored in this registry.
     * @note Lists are immutable once published: a change builds a new list and swaps
     *       the pointer, so a dispatch holding the old one is never invalidated. All
     *       members assume the owning state's mutex is held.
     */
    template <typename Fn>
    struct registry
    {
        using slot_type = slot<Fn>;
        using list = std::vector<std::shared_ptr<slot_type>>;

        std::unordered_map<event_key, std::shared_ptr<const list>> live;

        std::shared_ptr<const list> snapshot(event_key type) const
        {
            auto it = live.find(type);
            return it == live.end() ? nullptr : it->second;
        }

        // Stable insert: after every entry of equal or higher priority.
        void add(event_key type, std::shared_ptr<slot_type> s)
        {
            auto &current = live[type];
            auto updated = current ? std::make_shared<list>(*current) : std::make_shared<list>();
            auto pos = std::ranges::find_if(*updated, [&](const auto &x)
                                            { return x->priority < s->priority; });
            updated->insert(pos, std::move(s));
            current = std::move(updated);
        }

        void remove(event_key type, std::size_t id)
        {
            auto it = live.find(type);
            if (it == live.end())
                return;

            const list &current = *it->second;
            auto pos = std::ranges::find(current, id, [](const auto &x)
                                         { return x->id; });
            if (pos == current.end())
                return;

            (*pos)->active.store(false, std::memory_order_release);

            if (current.size() == 1)
            {
                live.erase(it);
                return;
            }

            auto updated = std::make_shared<list>(current);
            updated->erase(updated->begin() + (pos - current.begin()));
            it->second = std::move(updated);
        }
    };

    using sync_listener_list = registry<sync_listener_fn>::list;
    using async_listener_list = registry<async_listener_fn>::list;
    using sync_middleware_list = registry<sync_middleware_fn>::list;
    using async_middleware_list = registry<async_middleware_fn>::list;

    /**
     * @struct state
     * @brief The bus's registries and lock, shared between the bus and its tokens so
     * a token can tell whether the bus still exists.
     */
    struct state
    {
        std::mutex mutex;
        std::atomic<std::size_t> next_id{0};

        registry<sync_listener_fn> sync_listeners;
        registry<async_listener_fn> async_listeners;
        registry<sync_middleware_fn> sync_middleware;
        registry<async_middleware_fn> async_middleware;

        void remove(kind k, event_key type, std::size_t id)
        {
            switch (k)
            {
            case kind::sync_listener:
                sync_listeners.remove(type, id);
                break;
            case kind::async_listener:
                async_listeners.remove(type, id);
                break;
            case kind::sync_middleware:
                sync_middleware.remove(type, id);
                break;
            case kind::async_middleware:
                async_middleware.remove(type, id);
                break;
            }
        }
    };

} // namespace catalyst::events::detail
