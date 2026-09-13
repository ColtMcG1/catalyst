#pragma once

#include "detail/registry.hpp"
#include "tag.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace catalyst::events
{

    class bus;

    /**
     * @class token
     * @brief Copyable, non-owning handle to a registration. Letting it go out of
     * scope leaves the registration in place; see scoped_token for RAII.
     * @note The token does not automatically unregister; use scoped_token for RAII management.
     * @note Tokens may outlive the bus; they simply become invalid.
     */
    class token
    {
    public:
        token() = default;

        // True while the registration is live and the bus still exists.
        bool valid() const noexcept
        {
            auto s = slot_.lock();
            return s && s->active.load(std::memory_order_acquire);
        }

        explicit operator bool() const noexcept { return valid(); }

        // Unregisters. A no-op if already removed or the bus is gone.
        void remove()
        {
            auto st = owner_.lock();
            if (!st)
                return;
            std::lock_guard lock(st->mutex);
            st->remove(kind_, type_, id_);
        }

    private:
        friend class bus;

        token(const std::shared_ptr<detail::state> &owner,
              const std::shared_ptr<detail::slot_base> &s,
              detail::kind k,
              event_key type) noexcept
            : owner_(owner), slot_(s), kind_(k), type_(type), id_(s->id) {}

        std::weak_ptr<detail::state> owner_;
        std::weak_ptr<detail::slot_base> slot_;
        detail::kind kind_{detail::kind::sync_listener};
        event_key type_{};
        std::size_t id_{0};
    };

    /**
     * @class scoped_token
     * @brief Move-only owner of a registration: removes it on destruction.
     * @note Use this when you want RAII-style management of event registrations.
     */
    class scoped_token
    {
    public:
        scoped_token() = default;
        scoped_token(token t) noexcept : token_(std::move(t)) {}

        scoped_token(const scoped_token &) = delete;
        scoped_token &operator=(const scoped_token &) = delete;

        scoped_token(scoped_token &&other) noexcept : token_(std::exchange(other.token_, token{})) {}

        scoped_token &operator=(scoped_token &&other) noexcept
        {
            if (this != &other)
            {
                reset();
                token_ = std::exchange(other.token_, token{});
            }
            return *this;
        }

        scoped_token &operator=(token t) noexcept
        {
            reset();
            token_ = std::move(t);
            return *this;
        }

        ~scoped_token() { reset(); }

        bool valid() const noexcept { return token_.valid(); }
        explicit operator bool() const noexcept { return valid(); }

        // Unregisters now and empties this handle.
        void reset()
        {
            token_.remove();
            token_ = token{};
        }

        // Gives up ownership without unregistering.
        token release() noexcept { return std::exchange(token_, token{}); }

        const token &get() const noexcept { return token_; }

    private:
        token token_;
    };

} // namespace catalyst::events
