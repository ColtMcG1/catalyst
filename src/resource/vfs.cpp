/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file vfs.cpp
 * @brief Mount table, scheme dispatch and the read paths.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/vfs.hpp>

#include <algorithm>

namespace catalyst::resource
{
    namespace
    {
        /// Whether `path` lies under `prefix` on a whole-segment boundary: `/core` claims `/core/x`
        /// but not `/core_extra/x`.
        bool path_under(std::string_view path, std::string_view prefix) noexcept
        {
            if (prefix.empty())
                return true;
            if (!path.starts_with(prefix))
                return false;

            return path.size() == prefix.size() || path[prefix.size()] == '/' || prefix.back() == '/';
        }

        bool claims(const mount_point &m, const uri &location) noexcept
        {
            const auto scheme = location.scheme();
            if (!scheme || *scheme != m.scheme)
                return false;

            if (!m.authority.empty())
            {
                const auto authority = location.authority();
                if (!authority || *authority != m.authority)
                    return false;
            }

            return path_under(location.path(), m.path_prefix);
        }
    } // namespace

    // -----------------------------------------------------------------------------
    // Mounting
    // -----------------------------------------------------------------------------

    std::expected<void, error> vfs::mount(mount_point where, std::unique_ptr<source> src)
    {
        if (where.scheme.empty())
            return std::unexpected(make_error(error_code::invalid_uri, {}, "mount point names no scheme"));
        if (!src)
            return std::unexpected(make_error(error_code::invalid_uri, where.scheme, "null source"));

        // Keep both vectors sorted by descending specificity so `find_source` can take the first
        // match. `upper_bound` puts an equally specific new mount *before* the existing ones, which
        // is what makes a patch pack mounted later shadow the base it was mounted over.
        const std::size_t specificity = where.specificity();
        const auto at = std::upper_bound(mounts_.begin(), mounts_.end(), specificity,
                                         [](std::size_t value, const mount_point &m)
                                         { return value > m.specificity(); });

        const auto index = static_cast<std::size_t>(at - mounts_.begin());
        mounts_.insert(at, std::move(where));
        sources_.insert(sources_.begin() + static_cast<std::ptrdiff_t>(index), std::move(src));
        return {};
    }

    std::size_t vfs::unmount(const mount_point &where)
    {
        std::size_t removed = 0;
        for (std::size_t i = mounts_.size(); i-- > 0;)
        {
            if (mounts_[i].scheme != where.scheme || mounts_[i].authority != where.authority ||
                mounts_[i].path_prefix != where.path_prefix)
                continue;

            mounts_.erase(mounts_.begin() + static_cast<std::ptrdiff_t>(i));
            sources_.erase(sources_.begin() + static_cast<std::ptrdiff_t>(i));
            ++removed;
        }
        return removed;
    }

    void vfs::clear() noexcept
    {
        mounts_.clear();
        sources_.clear();
    }

    source *vfs::find_source(const uri &location) const noexcept
    {
        for (std::size_t i = 0; i < mounts_.size(); ++i)
        {
            if (claims(mounts_[i], location))
                return sources_[i].get();
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------------
    // Naming
    // -----------------------------------------------------------------------------

    std::expected<uri, error> vfs::resolve(std::string_view reference) const
    {
        auto parsed = uri::parse(reference);
        if (!parsed)
            return std::unexpected(
                make_error(error_code::invalid_uri, reference, parsed.error().message()));

        // A relative reference only means something against the URI it was written in.
        if (parsed->is_relative() && !base_.empty())
        {
            auto resolved = base_.resolve(*parsed);
            if (!resolved)
                return std::unexpected(
                    make_error(error_code::invalid_uri, reference, resolved.error().message()));
            parsed = std::move(resolved);
        }

        // Canonical spelling, so two names for one asset hit one cache entry.
        return parsed->normalized();
    }

    // -----------------------------------------------------------------------------
    // Reading
    // -----------------------------------------------------------------------------

    std::expected<blob, error> vfs::read(const uri &location) const
    {
        source *src = find_source(location);
        if (!src)
            return std::unexpected(make_error(error_code::no_such_mount, location.string()));

        return src->open(location);
    }

    std::expected<blob, error> vfs::read(std::string_view reference) const
    {
        auto location = resolve(reference);
        if (!location)
            return std::unexpected(std::move(location.error()));

        return read(*location);
    }

    events::task<std::expected<blob, error>> vfs::read_async(uri location) const
    {
        // Tier 1: the shape without the threading. The coroutine does the blocking read on whichever
        // thread resumes it, which for a caller that awaits immediately is the calling thread. Tier 3
        // replaces the body with a hand-off to a worker pool; no caller changes.
        co_return read(location);
    }

    // -----------------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------------

    bool vfs::exists(const uri &location) const
    {
        source *src = find_source(location);
        return src && src->exists(location);
    }

    std::expected<entry_info, error> vfs::stat(const uri &location) const
    {
        source *src = find_source(location);
        if (!src)
            return std::unexpected(make_error(error_code::no_such_mount, location.string()));

        return src->stat(location);
    }

    std::expected<std::vector<uri>, error> vfs::list(const uri &prefix) const
    {
        const auto scheme = prefix.scheme();
        if (!scheme)
            return std::unexpected(make_error(error_code::invalid_uri, prefix.string(),
                                              "listing needs an absolute URI"));

        std::vector<uri> out;
        bool any_mount = false;

        for (std::size_t i = 0; i < mounts_.size(); ++i)
        {
            const mount_point &m = mounts_[i];
            if (m.scheme != *scheme)
                continue;

            // Either the mount is under the prefix, or the prefix is under the mount; both mean the
            // mount can contribute entries beneath the prefix.
            if (!claims(m, prefix) && !path_under(m.path_prefix, prefix.path()))
                continue;

            any_mount = true;

            // A source that cannot enumerate contributes nothing rather than failing the call: a
            // namespace half of which is a network source should still list the half that can.
            if (!has_flag(sources_[i]->caps(), source_caps::enumerable))
                continue;

            auto entries = sources_[i]->list(prefix);
            if (!entries)
                continue;

            out.insert(out.end(), std::make_move_iterator(entries->begin()),
                       std::make_move_iterator(entries->end()));
        }

        if (!any_mount)
            return std::unexpected(make_error(error_code::no_such_mount, prefix.string()));

        // A patch pack and the base it shadows can both name one entry; report it once.
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

} // namespace catalyst::resource
