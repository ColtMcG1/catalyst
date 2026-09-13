/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file source.hpp
 * @brief @ref catalyst::resource::source -- the seam between "a name for some bytes" and the bytes,
 * plus the two implementations every build has: a directory on disk and a table in memory.
 * @details A source answers one question -- *given this URI, what are the bytes?* -- and knows
 * nothing about what the bytes mean. Decoding happens a layer up, in the loaders; mounting and
 * scheme dispatch happens a layer up from here too, in @ref catalyst::resource::vfs. Keeping the
 * three apart is what lets a shipped build read a texture out of a pack file while the editor reads
 * the same URI off the artist's disk, with no loader changing by a line.
 *
 * The interface is deliberately small. `open` is the whole of it; `exists` and `stat` are there
 * because a hot-reload watcher and an editor's asset browser both need to ask about an entry
 * without paying to read it, and `list` because something has to be able to enumerate a pack.
 * There is no seek, no partial read and no write: a source is a read-only, whole-entry thing.
 * Streaming a 2 GB video is not what this is for, and pretending otherwise would put a file handle
 * in every asset.
 *
 * **Threads.** A `source` must be safe to call from several threads at once, because the async read
 * path (see vfs.hpp) will call it from a worker while the main thread calls it too. Both sources
 * here are: the file source holds no mutable state across a call, and the memory source's table is
 * fixed at construction.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/resource/blob.hpp>
#include <catalyst/resource/error.hpp>
#include <catalyst/resource/handle.hpp>
#include <catalyst/resource/uri/reference.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalyst::resource
{

    /**
     * @enum source_caps
     * @brief What a source can do beyond `open`. Combine with `|`.
     * @details Queried rather than assumed: a pack source can enumerate and can report sizes
     * without reading, a network source can do neither, and a hot-reload watcher needs to know
     * which it is talking to before it decides whether to poll.
     */
    enum class source_caps : std::uint8_t
    {
        none = 0,
        /** @brief @ref source::list returns entries rather than @ref error_code::unsupported_format. */
        enumerable = 1u << 0,
        /** @brief @ref source::stat is cheap -- it does not read the entry to answer. */
        stattable = 1u << 1,
        /** @brief Entries can change after the source is mounted, so a watcher should poll
         * @ref source::stat. False for a sealed pack, true for a directory on disk. */
        mutable_entries = 1u << 2,
    };

    /**
     * @struct entry_info
     * @brief What @ref source::stat knows about an entry without reading it.
     */
    struct entry_info
    {
        /** @brief Size in bytes, or 0 when the source cannot say without reading. */
        std::uint64_t size_bytes = 0;

        /** @brief An opaque token that changes when the entry's contents change. A file source uses
         * the last-write time; a pack source uses a content hash. Compare for inequality only --
         * ordering is meaningless. Zero means "cannot tell", and a watcher should treat the entry
         * as never changing. */
        std::uint64_t revision = 0;

        friend bool operator==(const entry_info &, const entry_info &) noexcept = default;
    };

    /**
     * @class source
     * @brief A read-only namespace of byte entries addressed by URI.
     */
    class source
    {
    public:
        virtual ~source() = default;

        /** @brief A short name for logs and diagnostics, e.g. `file:C:/game/assets`. */
        [[nodiscard]] virtual std::string_view name() const noexcept = 0;

        /** @brief What this source supports beyond `open`. */
        [[nodiscard]] virtual source_caps caps() const noexcept { return source_caps::none; }

        /**
         * @brief Reads the whole entry @p location names.
         * @param location The URI, already resolved against the mount by @ref vfs. Sources look at
         *        @ref uri::path and, where they are hierarchical, @ref uri::authority; a source is
         *        free to ignore the query and fragment.
         * @return The bytes, or why not. @ref error_code::not_found when the entry is absent,
         *         @ref error_code::io_error when the read began and did not finish.
         */
        [[nodiscard]] virtual std::expected<blob, error> open(const uri &location) = 0;

        /**
         * @brief Whether @p location names an entry. Default: opens it and throws the bytes away,
         * which is correct but wasteful -- override it.
         */
        [[nodiscard]] virtual bool exists(const uri &location);

        /**
         * @brief Size and revision of @p location without reading it.
         * @return @ref error_code::unsupported_format when @ref source_caps::stattable is not set.
         */
        [[nodiscard]] virtual std::expected<entry_info, error> stat(const uri &location);

        /**
         * @brief Entries under @p prefix, as URIs this source will accept back in @ref open.
         * @return @ref error_code::unsupported_format when @ref source_caps::enumerable is not set.
         */
        [[nodiscard]] virtual std::expected<std::vector<uri>, error> list(const uri &prefix);
    };

    /**
     * @brief A source reading from a directory tree on the host filesystem.
     * @details The URI path is taken relative to @p root, percent-decoded, and required to stay
     * inside it: a path that climbs out with `..` is rejected with @ref error_code::access_denied
     * rather than resolved, because a manifest on disk is content and content does not get to name
     * files outside the mount.
     *
     * Reports @ref source_caps::enumerable, @ref source_caps::stattable and
     * @ref source_caps::mutable_entries.
     */
    [[nodiscard]] std::unique_ptr<source> make_file_source(std::filesystem::path root);

    /**
     * @brief A source over a fixed table of in-memory entries. The one tests and defaults use.
     * @param entries Path (as it will appear in the URI, without a leading `/`) to bytes. The bytes
     *        are *not* copied and must outlive the source -- reads hand back @ref blob::borrow.
     * @details Reports @ref source_caps::enumerable and @ref source_caps::stattable, but not
     * @ref source_caps::mutable_entries: the table is fixed at construction.
     */
    [[nodiscard]] std::unique_ptr<source>
    make_memory_source(std::vector<std::pair<std::string, std::span<const std::byte>>> entries);

} // namespace catalyst::resource

namespace catalyst::rendering
{
    /** @brief Opt @ref catalyst::resource::source_caps into the shared flag-enum operators. See
     * `<catalyst/resource/handle.hpp>` for why the specialisation lives in this namespace. */
    template <>
    inline constexpr bool is_flags_enum_v<catalyst::resource::source_caps> = true;
} // namespace catalyst::rendering
