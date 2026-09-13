/**
 * @file block.hpp
 * @brief One block of audio, and the callable a stream renders it with.
 * @details These two types are the real-time boundary of the module. Everything on the other side
 * of it - opening devices, enumerating endpoints, publishing events - happens on threads the caller
 * owns. Everything on this side happens on a thread the driver owns, under a hard deadline, and the
 * rules are not stylistic:
 *
 * **A renderer must not allocate, lock, block, or do I/O.** No `new`, no `std::vector` growth, no
 * `std::mutex`, no file or socket, no logging, and nothing that might page. Missing the deadline
 * does not produce a slow frame the way it would on the render thread - it produces an audible
 * click, counted as an xrun in @ref stream_stats. A renderer that needs to talk to the rest of the
 * program does it through a lock-free queue it owns, not by reaching back into the engine.
 *
 * **A renderer must not throw.** @ref renderer only accepts callables declared `noexcept`, and that
 * is a hard requirement rather than advice: there is no safe way to unwind out of a driver
 * callback, so the constraint is checked at compile time instead of hoped for at run time.
 *
 * **A renderer must not outlive nothing, and must not be outlived.** @ref renderer is a
 * non-owning reference - two pointers, no allocation - so the callable it names has to stay alive
 * and at a fixed address for as long as the stream does. That was already true of the old
 * `void *user` state pointer; the difference is that it is now the type saying so, and that
 * binding a temporary is a compile error rather than a dangling pointer discovered at 48 kHz.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/audio/types.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace catalyst::audio
{

    /**
     * @struct render_block
     * @brief The one argument a renderer is handed for each block of audio.
     * @details Buffers are interleaved: frame 0's channels, then frame 1's, and so on. They are
     * spans rather than a pointer and a count because the pair could disagree and a span cannot,
     * and because it makes the bounds available to an assertion in a debug build.
     *
     * @ref output is **undefined on entry**, not zeroed. A renderer fills every sample of it or
     * calls @ref silence first; anything else plays whatever the driver's buffer happened to hold,
     * which is usually the last block again.
     */
    struct render_block
    {
        /**
         * @brief Interleaved output, `frames * output_channels` samples.
         * @details Empty for an input-only stream. Undefined on entry - see the struct comment.
         */
        std::span<sample> output;

        /**
         * @brief Interleaved capture, `frames * input_channels` samples.
         * @details Empty unless the stream was opened for @ref stream_direction::input or
         * @ref stream_direction::duplex.
         */
        std::span<const sample> input;

        /**
         * @brief Frames in this block.
         * @details Varies between calls. Never assume @ref stream_info::block_frames - that is the
         * nominal size the device agreed to, and a device is free to hand over a short block at any
         * time, particularly the first and last of a run.
         */
        std::uint32_t frames = 0;

        /** @brief Channels in @ref output. Zero for an input-only stream. */
        channel_count output_channels = 0;

        /** @brief Channels in @ref input. Zero for an output-only stream. */
        channel_count input_channels = 0;

        /** @brief The negotiated rate, which may differ from the one that was requested. */
        sample_rate_t sample_rate = 0;

        /**
         * @brief Frames delivered on this stream before this block.
         * @details Monotonic for the life of the stream, and the correct clock for oscillators,
         * envelopes and anything else that needs to know where it is. Exact, unlike a float
         * accumulator, and 64-bit, unlike a frame counter that would wrap in a day.
         */
        frame_count position = 0;

        /** @brief Where this block starts, as a duration since the stream opened. */
        [[nodiscard]] constexpr seconds time() const noexcept
        {
            return frames_to_time(position, sample_rate);
        }

        /** @brief How long this block lasts. */
        [[nodiscard]] constexpr seconds duration() const noexcept
        {
            return frames_to_time(frames, sample_rate);
        }

        /**
         * @brief The output samples of one frame, one per channel.
         * @param frame Frame index within this block, less than @ref frames.
         * @details Lets a renderer write `for (auto f : ...) for (auto &s : block.output_frame(f))`
         * instead of computing `f * channels + c` by hand, which is where interleaving bugs live.
         */
        [[nodiscard]] constexpr std::span<sample> output_frame(std::uint32_t frame) noexcept
        {
            const std::size_t offset = static_cast<std::size_t>(frame) * output_channels;
            if (offset + output_channels > output.size())
                return {};
            return output.subspan(offset, output_channels);
        }

        /**
         * @brief The captured samples of one frame, one per channel.
         * @param frame Frame index within this block, less than @ref frames.
         */
        [[nodiscard]] constexpr std::span<const sample> input_frame(std::uint32_t frame) const noexcept
        {
            const std::size_t offset = static_cast<std::size_t>(frame) * input_channels;
            if (offset + input_channels > input.size())
                return {};
            return input.subspan(offset, input_channels);
        }

        /** @brief Writes zeros over the whole output buffer. The correct way to render nothing. */
        constexpr void silence() noexcept { std::fill(output.begin(), output.end(), sample{0}); }
    };

    /**
     * @concept renderable
     * @brief A callable a stream can render with: invocable on a `render_block &`, and `noexcept`.
     * @tparam F The candidate type.
     * @details The `noexcept` half is the part that does work. A callable that can throw is not a
     * renderer, whatever else it does, because there is nothing on the other side of a driver
     * callback to catch it - so it fails to compile here rather than terminating the process later.
     */
    template <typename F>
    concept renderable = std::is_nothrow_invocable_r_v<void, F &, render_block &>;

    /**
     * @class renderer
     * @brief A non-owning reference to the callable that fills a stream's blocks.
     * @details Two pointers wide, trivially copyable, and it allocates nothing - so it costs the
     * same on the real-time path as the raw function pointer it replaces, while letting the call
     * site pass a lambda that captures instead of casting a `void *` back to its own type by hand.
     *
     * It refers to the callable rather than owning it, which means the callable has to outlive the
     * stream and stay put:
     *
     *     auto render = [&tone](audio::render_block &block) noexcept { ... };
     *     auto stream = audio::stream::open(cfg, render);   // `render` outlives `stream`
     *
     * Binding a temporary is a compile error, not a warning, because the alternative is a dangling
     * pointer that only misbehaves once the driver thread starts. A capture-free lambda or a plain
     * function has nothing to dangle, so those may be passed directly.
     */
    class renderer
    {
    public:
        /** @brief The type of a plain render function. */
        using function_type = void (*)(render_block &) noexcept;

        /** @brief An empty renderer. A stream given one renders silence. */
        constexpr renderer() noexcept = default;

        /**
         * @brief Refers to @p callable, which must outlive the stream it is given to.
         * @tparam F Any type satisfying @ref renderable.
         * @param callable The renderer's state and code. Not copied.
         */
        template <typename F>
            requires renderable<F> && (!std::same_as<std::remove_cv_t<F>, renderer>)
        renderer(F &callable) noexcept
            : object_(const_cast<void *>(static_cast<const void *>(std::addressof(callable)))),
              invoke_([](void *object, render_block &block) noexcept
                      { (*static_cast<F *>(object))(block); })
        {
        }

        /**
         * @brief Refusing a temporary, so a lambda that captures cannot be passed inline and dangle.
         * @details Name the callable first and pass it by name. A stateless lambda or a function
         * pointer is not affected - it converts to @ref function_type and takes the overload below.
         */
        template <typename F>
            requires renderable<std::remove_reference_t<F>> && (!std::is_lvalue_reference_v<F>)
        renderer(F &&) = delete;

        /**
         * @brief Refers to a plain function, which has no lifetime to get wrong.
         * @param function The render function, or null for an empty renderer.
         */
        renderer(function_type function) noexcept
            : object_(reinterpret_cast<void *>(function)),
              invoke_(function ? &invoke_function : nullptr)
        {
        }

        /** @brief True if this renderer names a callable. */
        [[nodiscard]] constexpr bool valid() const noexcept { return invoke_ != nullptr; }

        /** @brief True if this renderer names a callable. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

        /**
         * @brief Renders one block. Real-time thread.
         * @param block The block to fill.
         * @note A no-op when the renderer is empty; the caller is responsible for silencing the
         * output in that case. @ref render_block::silence does it.
         */
        void operator()(render_block &block) const noexcept
        {
            if (invoke_)
                invoke_(object_, block);
        }

    private:
        static void invoke_function(void *object, render_block &block) noexcept
        {
            reinterpret_cast<function_type>(object)(block);
        }

        void *object_ = nullptr;
        void (*invoke_)(void *, render_block &) noexcept = nullptr;
    };

} // namespace catalyst::audio
