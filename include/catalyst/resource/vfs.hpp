/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file vfs.hpp
 * @brief @ref catalyst::resource::vfs -- the mount table that turns a URI into a
 * @ref catalyst::resource::source and gets the bytes, synchronously or on a worker.
 * @details One `vfs` owns the mounts, and everything above it names assets by URI and nothing else.
 * That indirection is the point of the layer: a shipped build mounts a sealed pack on `asset:`, the
 * editor mounts the artist's working tree on the same scheme, and a test mounts a
 * @ref make_memory_source table -- and the URI in a manifest, a material or a save file is
 * identical in all three.
 *
 * **Resolution.** A mount claims a scheme, optionally narrowed by an authority and a path prefix.
 * Mounts are consulted most-specific first, so `asset://core/` can be a pack while `asset://` at
 * large stays on disk. A relative reference is resolved against @ref vfs::base first, which is how
 * a manifest that says `../textures/stone_d.png` becomes a name the mount table can dispatch; see
 * `uri::resolve`. Names are matched after `uri::normalized`, so two spellings of one asset hit one
 * cache entry.
 *
 * **Async.** @ref vfs::read_async returns an `events::task`. In Tier 1 it completes synchronously
 * on the calling thread -- the seam exists so callers are written against the shape they will get,
 * not so they get it yet. Tier 3 puts a worker pool behind it and the awaiting coroutine resumes on
 * whichever thread pumps it, exactly as `rendering::transfer` already does.
 *
 * **Threads.** Reads are safe from any thread. Mounting is not: build the table during startup,
 * then read from it. A vfs whose mounts change under a concurrent read is a bug this class does not
 * try to make safe, because the alternative is a lock on the hot path for something that happens
 * twice per run.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/events/task.hpp>
#include <catalyst/resource/blob.hpp>
#include <catalyst/resource/error.hpp>
#include <catalyst/resource/source.hpp>
#include <catalyst/resource/uri/reference.hpp>

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace catalyst::resource
{

    /**
     * @struct mount_point
     * @brief Where a source is attached in the URI namespace.
     */
    struct mount_point
    {
        /** @brief The scheme this mount claims, without the `:` -- `asset`, `pack`, `user`. */
        std::string scheme;

        /** @brief The authority it narrows to, or empty for "any authority under that scheme". */
        std::string authority;

        /** @brief The path prefix it narrows to, or empty for "the whole scheme". Matched on whole
         * path segments, so `/core` claims `/core/x` but not `/core_extra/x`. */
        std::string path_prefix;

        /** @brief The number of characters a candidate URI must match for this mount to claim it.
         * Mounts are tried in descending order of this, so the most specific one wins. */
        [[nodiscard]] std::size_t specificity() const noexcept
        {
            return authority.size() + path_prefix.size();
        }
    };

    /**
     * @class vfs
     * @brief The mount table. Move-only; one per application is the expected shape.
     */
    class vfs
    {
    public:
        vfs() = default;

        vfs(const vfs &) = delete;
        vfs &operator=(const vfs &) = delete;
        vfs(vfs &&) noexcept = default;
        vfs &operator=(vfs &&) noexcept = default;

        /**
         * @brief Attaches @p src at @p where. Later mounts of an equal specificity shadow earlier
         * ones, so mounting a patch pack over a base pack does what it looks like.
         * @return @ref error_code::invalid_uri when @p where names no scheme, or a null source.
         */
        std::expected<void, error> mount(mount_point where, std::unique_ptr<source> src);

        /**
         * @brief Removes every mount whose scheme, authority and prefix equal @p where.
         * @return The number of mounts removed.
         */
        std::size_t unmount(const mount_point &where);

        /** @brief Drops every mount. */
        void clear() noexcept;

        /** @brief The mounts, most specific first. */
        [[nodiscard]] std::span<const mount_point> mounts() const noexcept { return mounts_; }

        /**
         * @brief The URI relative references are resolved against. Set it to the manifest a batch of
         * names came from, and `read("../shared/x.json")` means what the manifest's author meant.
         */
        [[nodiscard]] const uri &base() const noexcept { return base_; }
        void set_base(uri base) { base_ = std::move(base); }

        /**
         * @brief Parses @p reference, resolves it against @ref base, and normalizes it.
         * @details The canonical spelling of an asset name, and the key the registry caches under.
         * Call it once and keep the result when you are about to do several things with one name.
         */
        [[nodiscard]] std::expected<uri, error> resolve(std::string_view reference) const;

        /**
         * @brief Reads the whole entry named by @p location. Blocks.
         * @param location An already-resolved URI. Use @ref resolve on a name that may be relative.
         * @return The bytes, or why not -- @ref error_code::no_such_mount when nothing claims the
         *         scheme, and whatever the source said otherwise.
         */
        [[nodiscard]] std::expected<blob, error> read(const uri &location) const;

        /** @brief @ref resolve followed by @ref read. The convenience most callers want. */
        [[nodiscard]] std::expected<blob, error> read(std::string_view reference) const;

        /**
         * @brief The non-blocking form of @ref read.
         * @details Tier 1 completes on the calling thread the moment the task is awaited; the
         * signature is the contract, the threading is Tier 3. Write callers against this rather
         * than against @ref read wherever the read is not on a startup path, and they will get the
         * worker pool for free when it lands.
         */
        [[nodiscard]] events::task<std::expected<blob, error>> read_async(uri location) const;

        /** @brief Whether anything claims @p location and has an entry under it. */
        [[nodiscard]] bool exists(const uri &location) const;

        /** @brief @ref source::stat through the mount that claims @p location. */
        [[nodiscard]] std::expected<entry_info, error> stat(const uri &location) const;

        /**
         * @brief Entries under @p prefix, across every mount that could claim something beneath it.
         * @details Mounts that are not @ref source_caps::enumerable contribute nothing rather than
         * failing the call, so listing a namespace half of which is a network source returns what
         * can be listed.
         */
        [[nodiscard]] std::expected<std::vector<uri>, error> list(const uri &prefix) const;

        /**
         * @brief The source that would serve @p location, or null.
         * @details Exposed for the hot-reload watcher, which needs to know whether the mount behind
         * an asset reports @ref source_caps::mutable_entries before it bothers polling.
         */
        [[nodiscard]] source *find_source(const uri &location) const noexcept;

    private:
        // Parallel arrays, kept sorted by descending mount_point::specificity.
        std::vector<mount_point> mounts_;
        std::vector<std::unique_ptr<source>> sources_;
        uri base_;
    };

} // namespace catalyst::resource
