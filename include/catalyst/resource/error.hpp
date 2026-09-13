/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file error.hpp
 * @brief The failure value every fallible asset-system operation returns through `std::expected`.
 * @details A missing file, a pack that does not contain the entry a manifest promised, or a URI
 * naming a scheme nothing is mounted on are ordinary outcomes for a program that loads content off
 * a disk it does not control -- not exceptional ones -- so failures are values here, the way
 * `catalyst::rendering`, `catalyst::audio` and the `json` / `csv` / `uri` sub-modules report theirs.
 *
 * This is deliberately *not* the same type as @ref catalyst::resource::json::parse_error or
 * @ref catalyst::resource::uri_error. Those describe a byte offset in a document; this describes
 * something that happened to a named asset. When a loader wraps a parse failure, it keeps the
 * document's own message in @ref error::detail and reports @ref error_code::decode_failed, so the
 * caller branches on one enum and still gets the line number in a log.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace catalyst::resource
{

    /**
     * @enum error_code
     * @brief What went wrong.
     * @details Enumerators are appended, never reordered or reused: a saved log or a bug report may
     * name one by value.
     */
    enum class error_code : std::uint8_t
    {
        /** @brief No failure. Never carried by a returned @ref error; present so the struct has a
         * meaningful default. */
        none = 0,

        /** @brief The URI did not parse, or parsed into something no source can act on -- a
         * relative reference where an absolute one was required, for instance. */
        invalid_uri,

        /** @brief No @ref source is mounted for the URI's scheme. Distinct from
         * @ref not_found: the name is well formed but nothing claims to know that namespace. */
        no_such_mount,

        /** @brief The mount claimed the URI but has no entry under that path. */
        not_found,

        /** @brief The entry exists and the source refused to open it: permissions, a lock held by
         * another process, a directory where a file was expected. */
        access_denied,

        /** @brief The read began and did not finish: a truncated file, a short read, a pack whose
         * index disagrees with its contents. */
        io_error,

        /** @brief The bytes arrived and a decoder rejected them. @ref error::detail carries the
         * decoder's own message. */
        decode_failed,

        /** @brief The bytes are a format this build has no decoder for. */
        unsupported_format,

        /** @brief An @ref asset_handle whose slot has since been reused, or one minted by a
         * different registry. */
        stale_handle,

        /** @brief The registry is full: every one of @ref max_asset_slots slots is occupied. */
        registry_full,

        /** @brief The asset is still loading. Returned by the non-blocking accessors, and the
         * asset-system spelling of `rendering::error_code::not_ready`. */
        not_ready,

        /** @brief Asked for an asset as the wrong type -- a `mesh` handle against the `image`
         * registry. */
        type_mismatch,
    };

    /**
     * @fn to_string(error_code)
     * @brief A short, human-readable description of an @ref error_code.
     * @param code The code to describe.
     * @return A static string; never empty.
     */
    [[nodiscard]] constexpr std::string_view to_string(error_code code) noexcept
    {
        switch (code)
        {
        case error_code::none:
            return "no error";
        case error_code::invalid_uri:
            return "invalid URI";
        case error_code::no_such_mount:
            return "no source mounted for that scheme";
        case error_code::not_found:
            return "no such entry";
        case error_code::access_denied:
            return "access denied";
        case error_code::io_error:
            return "I/O error";
        case error_code::decode_failed:
            return "decode failed";
        case error_code::unsupported_format:
            return "unsupported format";
        case error_code::stale_handle:
            return "stale asset handle";
        case error_code::registry_full:
            return "asset registry full";
        case error_code::not_ready:
            return "asset not ready";
        case error_code::type_mismatch:
            return "asset type mismatch";
        }
        return "unknown error";
    }

    /**
     * @struct error
     * @brief A code plus the little that makes the code actionable.
     * @details The code alone is enough to branch on. @ref uri says *which* asset, which is what
     * turns a log line from "decode failed" into something a content author can fix, and
     * @ref detail carries whatever the layer below said -- a `json::parse_error` rendered to text,
     * an `errno` description, a pack entry name.
     *
     * Both string members own their storage. Unlike `rendering::error`, which points at backend
     * literals with static lifetime, the interesting half of an asset failure is the asset's name,
     * and that is built at run time.
     */
    struct error
    {
        /** @brief What went wrong. */
        error_code code = error_code::none;

        /** @brief The asset being operated on, as written by the caller. Empty when the failure is
         * not about a particular asset. */
        std::string uri;

        /** @brief What the layer below said, if it said anything. Empty otherwise. */
        std::string detail;

        /**
         * @fn message
         * @brief A one-line sentence naming the code, the asset and the detail.
         * @details Allocates. Intended for a log line or a fatal message, not for branching.
         */
        [[nodiscard]] std::string message() const;

        friend bool operator==(const error &, const error &) = default;
    };

    /**
     * @fn make_error
     * @brief Builds an @ref error. The convenience the loaders actually call.
     */
    [[nodiscard]] error make_error(error_code code, std::string_view uri = {}, std::string_view detail = {});

} // namespace catalyst::resource
