/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file loader.cpp
 * @brief The two non-template functions behind loader.hpp.
 * @details Everything else in the loader seam is a template and lives in
 * detail/loader_impl.hpp. These are here so that the `uri` work -- resolving a dependency against
 * its parent, and deciding when a failure gets a name -- exists once rather than once per
 * instantiation.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/loader.hpp>

namespace catalyst::resource
{

    std::expected<uri, error> load_context::resolve(std::string_view reference) const
    {
        auto parsed = uri::parse(reference);
        if (!parsed)
            return std::unexpected(make_error(error_code::invalid_uri, reference, parsed.error().message()));

        // Against this asset's own name, not against `vfs::base`. A reference inside a material is
        // relative to that material; the vfs's base is where a *batch* of names was listed.
        if (parsed->is_relative() && !name.empty())
        {
            auto resolved = name.resolve(*parsed);
            if (!resolved)
                return std::unexpected(make_error(error_code::invalid_uri, reference, resolved.error().message()));

            parsed = std::move(resolved);
        }

        // Canonical spelling, so the registry sees one cache entry for one asset.
        return parsed->normalized();
    }

    namespace detail
    {

        error with_name(error failure, const uri &name)
        {
            // Only if the layer below did not already name something. A source that failed on a
            // specific pack entry, or a loader that failed on a dependency rather than on itself,
            // has said something more useful than the name this call started from, and overwriting
            // it would turn a precise message into a vague one.
            if (failure.uri.empty())
                failure.uri = name.string();

            return failure;
        }

    } // namespace detail

} // namespace catalyst::resource
