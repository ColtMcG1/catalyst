/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Public transfer API: `transfer_batch`, `readback`, and the two one-shot helpers.
 * @details Thin over the backend, as everything in this layer is, with one thing worth saying about
 * the guards. Staging into the ring mutates state the whole device shares - the ring's head, its
 * in-flight list - so every call that stages takes the module lock exclusively, and two threads each
 * filling their own batch take turns for the length of a `memcpy` rather than for the length of a
 * transfer. Nothing here blocks while holding it.
 */

#include <catalyst/rendering/transfer.hpp>

#include "detail_backend.hpp"
#include "detail_sync.hpp"

#include <utility>

namespace catalyst::rendering
{

    staging_info get_staging_info(const device &dev) noexcept
    {
        if (!dev)
            return {};
        const detail::shared_guard guard;
        return detail::get_staging_info(dev.id());
    }

    // -------------------------------------------------------------------------
    // transfer_batch
    // -------------------------------------------------------------------------

    std::expected<transfer_batch, error> transfer_batch::begin(const device &dev)
    {
        if (!dev)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::begin"));

        const detail::exclusive_guard guard;
        if (!detail::is_device_valid(dev.id()))
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::begin"));
        if (detail::is_device_lost(dev.id()))
            return std::unexpected(make_error(error_code::device_lost, "transfer_batch::begin"));

        const resource_id id = detail::begin_transfer_batch(dev.id());
        if (id == 0)
            return std::unexpected(make_error(error_code::out_of_host_memory, "transfer_batch::begin"));
        return transfer_batch{dev, id};
    }

    transfer_batch::transfer_batch(transfer_batch &&other) noexcept
        : device_(other.device_), id_(std::exchange(other.id_, 0))
    {
        other.device_ = {};
    }

    transfer_batch &transfer_batch::operator=(transfer_batch &&other) noexcept
    {
        if (this == &other)
            return *this;
        discard();
        device_ = other.device_;
        id_ = std::exchange(other.id_, 0);
        other.device_ = {};
        return *this;
    }

    transfer_batch::~transfer_batch()
    {
        discard();
    }

    void transfer_batch::discard() noexcept
    {
        if (id_ == 0)
            return;
        {
            const detail::exclusive_guard guard;
            detail::discard_transfer_batch(id_);
        }
        id_ = 0;
        device_ = {};
    }

    std::expected<void, error> transfer_batch::upload(const buffer &dst, std::size_t offset_bytes,
                                                      std::span<const std::byte> data)
    {
        if (id_ == 0 || !dst)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));
        if (data.empty())
            return {};

        const detail::exclusive_guard guard;
        return detail::transfer_upload_buffer(id_, dst.id(), offset_bytes, data);
    }

    std::expected<void, error> transfer_batch::upload(const texture &dst, std::span<const std::byte> data)
    {
        if (id_ == 0 || !dst)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));
        if (data.empty())
            return {};

        const detail::exclusive_guard guard;
        return detail::transfer_upload_texture(id_, dst.id(), data);
    }

    std::expected<timeline_point, error> transfer_batch::submit()
    {
        if (id_ == 0)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::submit"));

        const detail::exclusive_guard guard;
        const auto value = detail::submit_transfer_batch(id_);
        if (!value)
            return std::unexpected(value.error());

        // Zero is the empty batch: nothing was submitted, so the honest point is the "no work" one,
        // which reports itself complete and costs a caller no branch.
        return timeline_point{device_, queue_kind::copy, *value};
    }

    std::size_t transfer_batch::staged_bytes() const noexcept
    {
        if (id_ == 0)
            return 0;
        const detail::shared_guard guard;
        return detail::transfer_batch_staged_bytes(id_);
    }

    std::size_t transfer_batch::size() const noexcept
    {
        if (id_ == 0)
            return 0;
        const detail::shared_guard guard;
        return detail::transfer_batch_size(id_);
    }

    // -------------------------------------------------------------------------
    // One-shot uploads
    // -------------------------------------------------------------------------

    std::expected<timeline_point, error> upload(const device &dev, const buffer &dst, std::size_t offset_bytes,
                                                std::span<const std::byte> data)
    {
        auto batch = transfer_batch::begin(dev);
        if (!batch)
            return std::unexpected(batch.error());
        if (auto ok = batch->upload(dst, offset_bytes, data); !ok)
            return std::unexpected(ok.error());
        return batch->submit();
    }

    std::expected<timeline_point, error> upload(const device &dev, const texture &dst,
                                                std::span<const std::byte> data)
    {
        auto batch = transfer_batch::begin(dev);
        if (!batch)
            return std::unexpected(batch.error());
        if (auto ok = batch->upload(dst, data); !ok)
            return std::unexpected(ok.error());
        return batch->submit();
    }

    // -------------------------------------------------------------------------
    // readback
    // -------------------------------------------------------------------------

    readback::readback(readback &&other) noexcept
        : device_(other.device_), point_(other.point_), id_(std::exchange(other.id_, 0)),
          size_(std::exchange(other.size_, 0))
    {
        other.device_ = {};
        other.point_ = {};
    }

    readback &readback::operator=(readback &&other) noexcept
    {
        if (this == &other)
            return *this;
        release();
        device_ = other.device_;
        point_ = other.point_;
        id_ = std::exchange(other.id_, 0);
        size_ = std::exchange(other.size_, 0);
        other.device_ = {};
        other.point_ = {};
        return *this;
    }

    readback::~readback()
    {
        release();
    }

    void readback::release() noexcept
    {
        if (id_ == 0)
            return;
        {
            // The backend defers the actual free until the copy has completed, so destroying a
            // readback that has not arrived yet is legal and does not block.
            const detail::exclusive_guard guard;
            detail::destroy_download(id_);
        }
        id_ = 0;
        size_ = 0;
        device_ = {};
        point_ = {};
    }

    std::expected<std::span<const std::byte>, error> readback::bytes() const
    {
        if (id_ == 0)
            return std::unexpected(make_error(error_code::invalid_argument, "readback::bytes"));

        // Asked before the guard is taken, because it takes one of its own: a shared lock is not
        // reentrant, and re-taking it here would deadlock against a waiting writer.
        if (!point_.is_complete())
            return std::unexpected(make_error(error_code::not_ready, "readback::bytes"));

        const detail::shared_guard guard;
        // `is_complete` reports true for a lost device too, so this is where the two are told
        // apart - the bytes never arrived and never will.
        if (device_ && detail::is_device_lost(device_.id()))
            return std::unexpected(make_error(error_code::device_lost, "readback::bytes"));
        return detail::download_bytes(id_);
    }

    std::expected<readback, error> download(const device &dev, const buffer &src, std::size_t offset_bytes,
                                            std::size_t size_bytes, std::span<const timeline_point> after)
    {
        if (!dev || !src || size_bytes == 0)
            return std::unexpected(make_error(error_code::invalid_argument, "download"));

        const detail::exclusive_guard guard;
        if (!detail::is_device_valid(dev.id()))
            return std::unexpected(make_error(error_code::invalid_argument, "download"));
        if (detail::is_device_lost(dev.id()))
            return std::unexpected(make_error(error_code::device_lost, "download"));

        std::uint64_t value = 0;
        const auto id = detail::begin_download(dev.id(), src.id(), offset_bytes, size_bytes, after, value);
        if (!id)
            return std::unexpected(id.error());

        return readback{dev, *id, size_bytes, timeline_point{dev, queue_kind::copy, value}};
    }

} // namespace catalyst::rendering
