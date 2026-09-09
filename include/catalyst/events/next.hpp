#pragma once

#include "task.hpp"

namespace catalyst::events
{

    namespace detail
    {
        /**
         * @struct chain_access
         * @brief Tag that gates construction of next and async_next.
         * @note Only the bus builds chain links; requiring this tag keeps that clear
         *       without a friend declaration that would have to reach across headers.
         */
        struct chain_access
        {
            explicit chain_access() = default;
        };
    }

    /**
     * @class next
     * @brief Represents the next step in the event chain for synchronous events.
     * @tparam Event The type of event.
     */
    template <typename Event>
    class next
    {
    public:
        next(detail::chain_access, void *ctx, void (*resume)(void *, void *)) noexcept
            : ctx_(ctx), resume_(resume) {}

        /**
         * @fn operator()
         * @brief Invokes the next step in the event chain with the given event.
         * @param e The event to pass to the next step.
         */
        void operator()(Event &e) const { resume_(ctx_, &e); }

    private:
        void *ctx_;                      ///< The context pointer for the next step.
        void (*resume_)(void *, void *); ///< The function pointer to resume the next step.
    };

    /**
     * @class async_next
     * @brief Represents the next step in the event chain for asynchronous events.
     * @tparam Event The type of event.
     */
    template <typename Event>
    class async_next
    {
    public:
        async_next(detail::chain_access, void *ctx, task<void> (*resume)(void *, void *)) noexcept
            : ctx_(ctx), resume_(resume) {}

        /**
         * @fn operator()
         * @brief Invokes the next step in the event chain with the given event.
         * @param e The event to pass to the next step.
         */
        task<void> operator()(Event &e) const { return resume_(ctx_, &e); }

    private:
        void *ctx_;                            ///< The context pointer for the next step.
        task<void> (*resume_)(void *, void *); ///< The function pointer to resume the next step.
    };

} // namespace catalyst::events
