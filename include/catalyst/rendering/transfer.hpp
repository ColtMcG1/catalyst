/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Asynchronous transfers: `transfer_batch` for getting data onto the GPU, `readback` for
 * getting it off, and the staging ring both are drawn from.
 * @details This is the tier that removes the module's largest hidden stall. Writing to a GPU-only
 * buffer used to mean: copy into a staging buffer, submit a command buffer, and *wait on a fence
 * before returning to the caller*. One texture upload in the middle of a frame therefore drained
 * the queue, and loading a level was N such round trips in series. The API had no way to say so,
 * because it had no way to talk about time at all - which is what Tier 2 fixed and what this tier
 * spends.
 *
 * What replaces it:
 *
 *   - **A staging ring, not a staging buffer.** One host-visible allocation, sized by
 *     `device_desc::staging_ring_bytes`, carved up front-to-back and recycled behind the copy
 *     queue's timeline. Successive transfers overlap instead of taking turns. When the ring is full
 *     of data the GPU has not read yet, the transfer fails with @ref error_code::staging_exhausted
 *     rather than quietly blocking - back-pressure a caller can see and respond to.
 *   - **A batch, not a submission per copy.** @ref transfer_batch accumulates any number of uploads
 *     and submits them once. Loading a level is one `vkQueueSubmit`, not one per mesh.
 *   - **A @ref timeline_point, not a wait.** `submit` hands back the point the data becomes
 *     resident at. `co_await` it, hand it to another `submit` as a GPU-side dependency, or ignore
 *     it - see the ordering note below.
 *
 * **Ordering is automatic, and cheap.** Transfers run on the copy queue, which has no order
 * relative to the graphics or compute queues unless one is imposed. Rather than make every caller
 * remember that, the device records the point of its most recent transfer and every later
 * submission on any queue waits on it - on the GPU, with no CPU involvement, and skipped entirely
 * once the transfer has already completed. So `write_buffer` followed by a draw that reads the
 * buffer is correct with no explicit synchronisation, exactly as it was when the write blocked, and
 * a caller who wants finer ordering than "after all outstanding transfers" gets it by passing the
 * transfer's point to `submit_info::wait` and nothing more.
 *
 * **What still blocks, and why.** `read_buffer` does: it hands back bytes, so it has nowhere to put
 * the waiting. @ref download is the non-blocking form and the one to reach for - it returns a
 * @ref readback that can be polled, waited on, or `co_await`ed.
 *
 * @code
 *   // Streaming a level while the frame loop keeps drawing.
 *   events::task<void> load_level(device dev, level_data data)
 *   {
 *       auto batch = transfer_batch::begin(dev).value();
 *       for (const mesh_data &m : data.meshes)
 *           batch.upload(m.target, 0, m.bytes).value();
 *
 *       const timeline_point resident = batch.submit().value();
 *       co_await resident;                    // resumes inside `pump`, on the caller's thread
 *
 *       logging::info<app>("level resident: {} meshes", data.meshes.size());
 *   }
 * @endcode
 *
 * **Threads.** A `transfer_batch` belongs to the thread that began it, like a `command_pool` -
 * several threads may each hold their own batch, and they draw from the same ring under the
 * module's own lock. @ref readback may be polled and waited on from any thread.
 */

#pragma once

#include <catalyst/rendering/buffer.hpp>
#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/error.hpp>
#include <catalyst/rendering/texture.hpp>
#include <catalyst/rendering/timeline.hpp>
#include <catalyst/rendering/types.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace catalyst::rendering
{

    /**
     * @struct staging_info
     * @brief What the device's staging ring is and how much of it is in use.
     * @details For diagnostics and for sizing: a caller that keeps hitting
     * @ref error_code::staging_exhausted wants to know whether the ring is too small for one
     * transfer or simply behind.
     */
    struct staging_info
    {
        /** @brief Total size of the ring, as resolved from `device_desc::staging_ring_bytes`. */
        std::uint64_t capacity_bytes = 0;

        /** @brief Bytes staged but not yet released, i.e. still being read by the copy queue or
         * waiting in an unsubmitted batch. */
        std::uint64_t in_use_bytes = 0;

        /** @brief The largest single transfer this ring can ever carry. A transfer bigger than this
         * fails no matter how long the caller waits, and has to be split. */
        std::uint64_t largest_transfer_bytes = 0;
    };

    /** @brief The state of `dev`'s staging ring. Zeroes for an invalid device. */
    [[nodiscard]] staging_info get_staging_info(const device &dev) noexcept;

    /**
     * @class transfer_batch
     * @brief A set of uploads accumulated on the CPU and submitted to the copy queue as one.
     * @details Move-only. Destroying an unsubmitted batch discards it and returns its staging space
     * to the ring, so an early return out of a load function leaks nothing.
     *
     * Each `upload` copies its bytes into the staging ring immediately - the caller's buffer need
     * not outlive the call - and records a copy command. Nothing reaches the GPU until @ref submit.
     */
    class transfer_batch
    {
    public:
        /**
         * @brief Opens a batch on `dev`.
         * @return @ref error_code::invalid_argument for an invalid device;
         * @ref error_code::device_lost if it has been lost.
         */
        [[nodiscard]] static std::expected<transfer_batch, error> begin(const device &dev);

        transfer_batch() noexcept = default;
        transfer_batch(const transfer_batch &) = delete;
        transfer_batch &operator=(const transfer_batch &) = delete;
        transfer_batch(transfer_batch &&other) noexcept;
        transfer_batch &operator=(transfer_batch &&other) noexcept;
        ~transfer_batch();

        /**
         * @brief Stages `data` and records a copy of it into `dst` at `offset_bytes`.
         * @return @ref error_code::invalid_argument if the range is out of bounds or `dst` belongs
         * to another device; @ref error_code::staging_exhausted if the ring has no room.
         * @details The copy is recorded, not performed. `dst` holds the new bytes from the
         * @ref timeline_point @ref submit returns onwards.
         *
         * With one exception, which is a shortcut rather than a rule: a buffer whose memory is
         * host-visible - anything `memory_access::cpu_to_gpu`, and everything on a unified-memory
         * adapter, where `device_info::unified_memory` is true - is written directly and never
         * touches the ring or the GPU at all. That is strictly cheaper, and it means such a write
         * is visible immediately rather than at the returned point. It also means it is the
         * caller's business not to overwrite bytes an earlier frame is still reading, exactly as it
         * was before this tier existed: the GPU-side ordering described above covers the staged
         * path, and there is nothing to order when no copy is submitted.
         */
        std::expected<void, error> upload(const buffer &dst, std::size_t offset_bytes,
                                          std::span<const std::byte> data);

        /**
         * @brief Stages `data` and records a copy of it into mip 0, layer 0 of `dst`.
         * @details `data` must be tightly packed in the texture's format and exactly cover the top
         * mip; a short or long span is @ref error_code::invalid_argument. Mip generation is not
         * part of this tier.
         */
        std::expected<void, error> upload(const texture &dst, std::span<const std::byte> data);

        /**
         * @brief Submits everything accumulated so far to the copy queue.
         * @return The point the whole batch becomes resident at, or an invalid point for an empty
         * batch - which is the honest answer, since nothing had to happen.
         * @details Does not block. The batch is empty afterwards and may be filled again.
         */
        [[nodiscard]] std::expected<timeline_point, error> submit();

        /** @brief Throws the batch away and returns its staging space. Called by the destructor. */
        void discard() noexcept;

        /** @brief Bytes staged into this batch and not yet submitted. */
        [[nodiscard]] std::size_t staged_bytes() const noexcept;

        /** @brief How many copies have been recorded and not yet submitted. */
        [[nodiscard]] std::size_t size() const noexcept;

        [[nodiscard]] bool valid() const noexcept { return id_ != 0; }
        [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

        [[nodiscard]] device owner() const noexcept { return device_; }

    private:
        explicit transfer_batch(device owner, resource_id id) noexcept : device_(owner), id_(id) {}

        device device_{};
        resource_id id_ = 0;
    };

    /**
     * @brief The one-shot form of @ref transfer_batch: stage, record, submit, in one call.
     * @details Right for a single upload; wrong for a hundred, which want one batch and one
     * submission. Does not block.
     */
    [[nodiscard]] std::expected<timeline_point, error> upload(const device &dev, const buffer &dst,
                                                              std::size_t offset_bytes,
                                                              std::span<const std::byte> data);

    /** @brief @ref upload for the top mip of a texture. */
    [[nodiscard]] std::expected<timeline_point, error> upload(const device &dev, const texture &dst,
                                                              std::span<const std::byte> data);

    /**
     * @class readback
     * @brief Bytes on their way from the GPU to the CPU, and the point they arrive at.
     * @details Move-only, and it owns the host-visible buffer the copy lands in - so it must be
     * kept alive until the bytes have been read, and its destructor defers the release until the
     * GPU is done regardless.
     *
     * Three ways to use one, in ascending order of how much the caller has to do:
     *
     * @code
     *   auto rb = download(dev, results, 0, size).value();
     *
     *   if (rb.is_complete()) { auto bytes = rb.bytes(); }        // poll
     *   rb.wait().value();                                        // block
     *   auto bytes = (co_await rb).value();                       // resume in `pump`
     * @endcode
     */
    class readback
    {
    public:
        readback() noexcept = default;
        readback(const readback &) = delete;
        readback &operator=(const readback &) = delete;
        readback(readback &&other) noexcept;
        readback &operator=(readback &&other) noexcept;
        ~readback();

        /** @brief The point the copy completes at. */
        [[nodiscard]] timeline_point point() const noexcept { return point_; }

        /** @brief Whether the bytes have arrived. Never blocks. */
        [[nodiscard]] bool is_complete() const noexcept { return point_.is_complete(); }

        /** @brief Blocks the calling thread until the bytes have arrived. */
        std::expected<void, error> wait() const noexcept { return point_.wait(); }

        /**
         * @brief The bytes.
         * @return @ref error_code::not_ready when the copy has not completed;
         * @ref error_code::device_lost when the device died first.
         * @details The span points into the readback's own mapped memory and stays valid until the
         * readback is destroyed or moved from.
         */
        [[nodiscard]] std::expected<std::span<const std::byte>, error> bytes() const;

        /** @brief Suspends until the bytes have arrived, then yields them; resumes in `pump`. */
        [[nodiscard]] auto operator co_await() const noexcept
        {
            struct awaiter
            {
                const readback *self;
                timeline_point point;

                /** The handle this awaiter put in the park list, while it is still in there. */
                std::coroutine_handle<> parked{};

                [[nodiscard]] bool await_ready() const noexcept { return point.is_complete(); }
                [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation)
                {
                    if (!detail::park(point, continuation))
                        return false;

                    parked = continuation;
                    return true;
                }
                [[nodiscard]] std::expected<std::span<const std::byte>, error> await_resume()
                {
                    // `pump` took the entry out of the list before resuming us.
                    parked = {};
                    return self->bytes();
                }

                // See timeline_point::operator co_await: dropping a parked task would otherwise
                // leave `pump` holding a handle to a freed frame.
                ~awaiter()
                {
                    if (parked)
                        detail::unpark(parked);
                }
            };
            return awaiter{this, point_};
        }

        [[nodiscard]] std::size_t size_bytes() const noexcept { return size_; }
        [[nodiscard]] bool valid() const noexcept { return id_ != 0; }
        [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    private:
        friend std::expected<readback, error> download(const device &, const buffer &, std::size_t, std::size_t,
                                                       std::span<const timeline_point>);

        readback(device owner, resource_id id, std::size_t size, timeline_point point) noexcept
            : device_(owner), point_(point), id_(id), size_(size)
        {
        }

        void release() noexcept;

        device device_{};
        timeline_point point_{};
        resource_id id_ = 0;
        std::size_t size_ = 0;
    };

    /**
     * @brief Copies `size_bytes` from `src` into host memory, without blocking.
     * @param after Points the copy must not start before. Leave empty for "after everything
     * currently submitted to this device", which is conservative and always correct; name the point
     * that produced the data to order the copy precisely instead.
     * @return @ref error_code::invalid_argument for an out-of-range request or a buffer without
     * `buffer_usage::transfer_src`.
     * @details The counterpart of `read_buffer`, which does the same thing and blocks. Prefer this
     * one anywhere the answer is not needed immediately - a screenshot, a picking query, a
     * statistics readback - and it will cost the frame nothing.
     */
    [[nodiscard]] std::expected<readback, error> download(const device &dev, const buffer &src,
                                                          std::size_t offset_bytes, std::size_t size_bytes,
                                                          std::span<const timeline_point> after = {});

} // namespace catalyst::rendering
