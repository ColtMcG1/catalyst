/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief GPU buffers: linear blocks of device memory used for vertex, index, uniform, storage and indirect data, plus a
 * typed `structured_buffer<T>` convenience wrapper.
 * @details A `buffer` is an opaque handle; its size, usage and CPU-visibility are fixed at creation. Data moves in and out
 * through `write_buffer` / `read_buffer` – the backend takes care of staging for GPU-only memory. Buffers are bound to a
 * command list with `set_vertex_buffer`, `set_index_buffer`, `set_uniform_buffer` or `set_storage_buffer`
 * (see command.hpp).
 */

#pragma once

#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/types.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace catalyst::rendering
{

    /**
     * @enum buffer_usage
     * @brief Bit flags declaring how a buffer will be bound. Combine with `|`.
     */
    enum class buffer_usage : std::uint16_t
    {
        none = 0,
        vertex = 1u << 0,
        index = 1u << 1,
        uniform = 1u << 2,
        storage = 1u << 3,
        indirect = 1u << 4,
        transfer_src = 1u << 5,
        transfer_dst = 1u << 6,
    };

    template <>
    inline constexpr bool is_flags_enum_v<buffer_usage> = true;

    /**
     * @enum memory_access
     * @brief Where the memory lives and which side may touch it directly.
     */
    enum class memory_access : std::uint8_t
    {
        /** Device-local. Fastest for the GPU; `write_buffer` goes through a staging copy, `read_buffer` is not allowed. */
        gpu_only,
        /** Host-visible upload heap. `write_buffer` is a direct memcpy; suited to per-frame uniform / dynamic data. */
        cpu_to_gpu,
        /** Host-visible readback heap. `read_buffer` is allowed; the GPU writes, the CPU reads. */
        gpu_to_cpu,
    };

    /**
     * @struct buffer_desc
     * @brief Creation parameters for a buffer.
     */
    struct buffer_desc
    {
        std::size_t size_bytes = 0;
        buffer_usage usage = buffer_usage::none;
        memory_access access = memory_access::gpu_only;
        /** Element stride for structured / storage buffers; 0 when the buffer is raw. */
        std::uint32_t stride_bytes = 0;
        /** Optional label shown in graphics debuggers. Not owned; may be null. */
        const char *debug_name = nullptr;
    };

    struct buffer_tag
    {
    };

    /**
     * @brief Handle to a GPU buffer. See `create_buffer`.
     */
    using buffer = resource_handle<buffer_tag>;

    /**
     * @brief Creates a buffer, optionally uploading `initial_data` (which must not exceed `desc.size_bytes`).
     * Returns an invalid handle when `dev` is invalid, `desc.size_bytes` is 0 or the initial data is too large.
     */
    [[nodiscard]] buffer create_buffer(const device &dev, const buffer_desc &desc,
                                       std::span<const std::byte> initial_data = {});

    /**
     * @brief Releases the buffer and resets `b` to an invalid handle. Safe to call on an invalid handle.
     */
    void destroy_buffer(buffer &b) noexcept;

    [[nodiscard]] bool is_valid(const buffer &b) noexcept;

    [[nodiscard]] buffer_desc get_buffer_desc(const buffer &b) noexcept;

    /** @brief Convenience: `get_buffer_desc(b).size_bytes`, or 0 for an invalid handle. */
    [[nodiscard]] std::size_t buffer_size(const buffer &b) noexcept;

    /**
     * @brief Copies `data` into the buffer at `offset`. Allowed for `gpu_only` and `cpu_to_gpu` buffers.
     * @return False if the range is out of bounds, the handle is invalid or the buffer is `gpu_to_cpu`.
     * @details **Does not block.** A `gpu_only` buffer that is not host-visible is written through
     * the staging ring and a copy submitted on the copy queue, and later submissions wait for that
     * copy on the GPU - so a draw that reads the buffer sees the new bytes without the caller
     * ordering anything. It used to submit and then wait on a fence, which drained the queue.
     * `transfer_batch` in transfer.hpp is the form that reports *when*, and the one to use for more
     * than a couple of writes: it submits once for the whole batch.
     */
    bool write_buffer(const buffer &b, std::size_t offset, std::span<const std::byte> data);

    /**
     * @brief Copies `out.size()` bytes from the buffer at `offset` into `out`. Only allowed for `gpu_to_cpu` buffers.
     * @return False if the range is out of bounds, the handle is invalid or the buffer is not readable.
     * @details **Blocks the calling thread** until every queue has finished what it was given, because
     * it hands back bytes and so has nowhere else to put the waiting. `download` in transfer.hpp is
     * the non-blocking form: it returns a `readback` that can be polled, waited on or `co_await`ed,
     * and it can be ordered against one `timeline_point` rather than against everything.
     */
    bool read_buffer(const buffer &b, std::size_t offset, std::span<std::byte> out);

    /**
     * @class structured_buffer
     * @brief A `buffer` whose contents are an array of trivially-copyable `T`. Wraps the byte-level API in element-level
     * calls and remembers the element count. It does not own the buffer any more than a plain handle does; call
     * `destroy()` (or `destroy_buffer(handle())`) when finished.
     */
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    class structured_buffer
    {
    public:
        using value_type = T;

        constexpr structured_buffer() noexcept = default;
        constexpr structured_buffer(buffer b, std::size_t count) noexcept : buffer_(b), count_(count) {}

        [[nodiscard]] constexpr const buffer &handle() const noexcept { return buffer_; }
        [[nodiscard]] constexpr std::size_t count() const noexcept { return count_; }
        [[nodiscard]] constexpr std::size_t size_bytes() const noexcept { return count_ * sizeof(T); }
        [[nodiscard]] static constexpr std::uint32_t stride_bytes() noexcept { return static_cast<std::uint32_t>(sizeof(T)); }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return static_cast<bool>(buffer_); }

        /** @brief Writes `elements` starting at element index `first`. */
        bool write(std::span<const T> elements, std::size_t first = 0)
        {
            if (first > count_ || elements.size() > count_ - first)
                return false;
            return write_buffer(buffer_, first * sizeof(T), std::as_bytes(elements));
        }

        /** @brief Reads `out.size()` elements starting at element index `first`. Requires `memory_access::gpu_to_cpu`. */
        bool read(std::span<T> out, std::size_t first = 0)
        {
            if (first > count_ || out.size() > count_ - first)
                return false;
            return read_buffer(buffer_, first * sizeof(T), std::as_writable_bytes(out));
        }

        void destroy() noexcept
        {
            destroy_buffer(buffer_);
            count_ = 0;
        }

    private:
        buffer buffer_{};
        std::size_t count_ = 0;
    };

    /**
     * @brief Creates a buffer sized for `count` elements of `T` with `stride_bytes = sizeof(T)`.
     * `initial` may hold fewer elements than `count`; the remainder is left uninitialised.
     */
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] structured_buffer<T> create_structured_buffer(const device &dev, std::size_t count, buffer_usage usage,
                                                                memory_access access = memory_access::gpu_only,
                                                                std::span<const T> initial = {},
                                                                const char *debug_name = nullptr)
    {
        if (count == 0 || initial.size() > count)
            return {};

        buffer_desc desc;
        desc.size_bytes = count * sizeof(T);
        desc.usage = usage;
        desc.access = access;
        desc.stride_bytes = static_cast<std::uint32_t>(sizeof(T));
        desc.debug_name = debug_name;

        buffer b = create_buffer(dev, desc, std::as_bytes(initial));
        if (!b)
            return {};
        return structured_buffer<T>{b, count};
    }

} // namespace catalyst::rendering
