#pragma once

#include <coroutine>
#include <exception>
#include <utility>
#include <type_traits>

namespace catalyst::events
{
    template <typename T = void>
    class task;

    namespace detail
    {
        // Holds the co_returned value. Split out so that task<void> is well formed:
        // a promise may declare return_value or return_void, never both, and a
        // member of type void is not allowed at all.
        template <typename T>
        struct task_result
        {
            T value_{};

            template <typename U>
            void return_value(U &&value) noexcept(std::is_nothrow_assignable_v<T &, U &&>)
            {
                value_ = std::forward<U>(value);
            }

            T take() noexcept(std::is_nothrow_move_constructible_v<T>) { return std::move(value_); }
        };

        template <>
        struct task_result<void>
        {
            void return_void() noexcept {}
            void take() noexcept {}
        };
    } // namespace detail

    template <typename T>
    class task
    {
    public:
        struct promise_type : detail::task_result<T>
        {
            std::exception_ptr exception_;
            std::coroutine_handle<> continuation_;

            task get_return_object() noexcept
            {
                return task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }

            auto final_suspend() noexcept
            {
                struct awaiter
                {
                    std::coroutine_handle<> continuation;
                    bool await_ready() const noexcept { return false; }

                    // Symmetric transfer: hand control straight to whoever awaited us
                    // instead of nesting a resume() call on the stack.
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<>) const noexcept
                    {
                        return continuation ? continuation : std::noop_coroutine();
                    }

                    void await_resume() const noexcept {}
                };
                return awaiter{continuation_};
            }

            void unhandled_exception() { exception_ = std::current_exception(); }
        };

        using handle_type = std::coroutine_handle<promise_type>;

        explicit task(handle_type h = nullptr) : h_(h) {}
        task(task &&other) noexcept : h_(std::exchange(other.h_, {})) {}
        task &operator=(task &&other) noexcept
        {
            if (this != &other)
            {
                if (h_)
                    h_.destroy();
                h_ = std::exchange(other.h_, {});
            }
            return *this;
        }
        ~task()
        {
            if (h_)
                h_.destroy();
        }

        bool valid() const noexcept { return h_ != nullptr; }
        bool done() const noexcept { return !h_ || h_.done(); }

        auto operator co_await() noexcept
        {
            struct awaiter
            {
                handle_type h_;
                bool await_ready() const noexcept { return !h_ || h_.done(); }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) const noexcept
                {
                    h_.promise().continuation_ = continuation;
                    return h_;
                }

                T await_resume()
                {
                    if (h_.promise().exception_)
                        std::rethrow_exception(h_.promise().exception_);
                    return h_.promise().take();
                }
            };
            return awaiter{h_};
        }

        void start()
        {
            if (h_)
                h_.resume();
        }

        // Runs the task to completion on the calling thread, assuming nothing in it
        // suspends on an external event, and surfaces any exception it threw.
        T get()
        {
            start();
            if (h_.promise().exception_)
                std::rethrow_exception(h_.promise().exception_);
            return h_.promise().take();
        }

    private:
        handle_type h_;
    };

} // namespace catalyst::events
