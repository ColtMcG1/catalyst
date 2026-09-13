/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file blob.cpp
 * @brief The allocating blob path.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/blob.hpp>

#include <new>

namespace catalyst::resource
{
    namespace
    {
        /// Owns the vector the bytes came from; released with the blob.
        void release_vector(std::span<std::byte> /*bytes*/, void *context) noexcept
        {
            delete static_cast<std::vector<std::byte> *>(context);
        }
    } // namespace

    blob blob::adopt(std::vector<std::byte> &&storage)
    {
        if (storage.empty())
            return blob{};

        // Heap-box the vector so the span stays valid however the blob is moved. One allocation per
        // read on top of the read itself; the mapped and borrowed paths pay neither.
        auto *owned = new std::vector<std::byte>(std::move(storage));
        return blob{std::span<std::byte>{owned->data(), owned->size()}, &release_vector, owned};
    }

} // namespace catalyst::resource
