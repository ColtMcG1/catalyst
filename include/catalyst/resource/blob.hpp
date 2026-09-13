/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file blob.hpp
 * @brief @ref catalyst::resource::blob -- the bytes a @ref catalyst::resource::source hands back,
 * and the one place the asset system talks about ownership of raw memory.
 * @details A `blob` is a move-only span of bytes plus the knowledge of how to release them. It
 * exists because the two sources that matter release their bytes differently and a loader should
 * not have to care which it got: a file source allocates and frees, a pack source memory-maps a
 * region it later unmaps, and a future source may hand out a view into an archive it keeps resident
 * with no per-read allocation at all.
 *
 * So `blob` is not `std::vector<std::byte>`. A vector would force the mapped and resident cases to
 * copy, which for a 200 MB texture pack is the whole cost of loading it. The deleter is a plain
 * function pointer and a `void *` rather than a `std::function` so the type stays trivially movable
 * and costs nothing to pass around.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace catalyst::resource
{

    /**
     * @class blob
     * @brief Owning, move-only view of a contiguous byte range obtained from a @ref source.
     */
    class blob
    {
    public:
        /** @brief How a blob releases its bytes. Called once, with the blob's @ref data and the
         * `context` it was constructed with. Never called for an empty blob. */
        using releaser = void (*)(std::span<std::byte> bytes, void *context) noexcept;

        /** @brief The empty blob. Owns nothing, releases nothing. */
        blob() noexcept = default;

        /**
         * @brief Adopts @p bytes, to be released by @p release.
         * @param bytes The range. May be empty, in which case @p release is never called.
         * @param release How to give the range back. Null means the bytes outlive the blob and
         *        need no release -- the right choice for a source holding an archive resident.
         * @param context Passed back to @p release verbatim.
         */
        blob(std::span<std::byte> bytes, releaser release, void *context = nullptr) noexcept
            : bytes_(bytes), release_(release), context_(context)
        {
        }

        /** @brief Takes ownership of a `std::vector`'s storage. The allocating path, used by the
         * file source and by every test. */
        [[nodiscard]] static blob adopt(std::vector<std::byte> &&storage);

        /** @brief A blob over bytes that outlive it -- a string literal, a static table, a region a
         * source keeps mapped for its own lifetime. Releases nothing. */
        [[nodiscard]] static blob borrow(std::span<std::byte> bytes) noexcept { return blob{bytes, nullptr, nullptr}; }

        blob(const blob &) = delete;
        blob &operator=(const blob &) = delete;

        blob(blob &&other) noexcept
            : bytes_(std::exchange(other.bytes_, {})),
              release_(std::exchange(other.release_, nullptr)),
              context_(std::exchange(other.context_, nullptr))
        {
        }

        blob &operator=(blob &&other) noexcept
        {
            if (this != &other)
            {
                reset();
                bytes_ = std::exchange(other.bytes_, {});
                release_ = std::exchange(other.release_, nullptr);
                context_ = std::exchange(other.context_, nullptr);
            }
            return *this;
        }

        ~blob() { reset(); }

        /** @brief Releases the bytes and leaves the blob empty. Idempotent. */
        void reset() noexcept
        {
            if (release_ && !bytes_.empty())
                release_(bytes_, context_);
            bytes_ = {};
            release_ = nullptr;
            context_ = nullptr;
        }

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
        [[nodiscard]] std::span<std::byte> mutable_bytes() noexcept { return bytes_; }
        [[nodiscard]] const std::byte *data() const noexcept { return bytes_.data(); }
        [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
        [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

        /**
         * @brief The bytes as text, for handing to the JSON / CSV parsers without a copy.
         * @details Says nothing about encoding or about there being a terminator; it is a
         * reinterpretation of the same range. Both parsers take `string_view` and neither needs a
         * NUL, which is the reason this is safe.
         */
        [[nodiscard]] std::string_view text() const noexcept
        {
            return std::string_view{reinterpret_cast<const char *>(bytes_.data()), bytes_.size()};
        }

    private:
        std::span<std::byte> bytes_{};
        releaser release_ = nullptr;
        void *context_ = nullptr;
    };

} // namespace catalyst::resource
