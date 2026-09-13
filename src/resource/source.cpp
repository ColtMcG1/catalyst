/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file source.cpp
 * @brief The `source` defaults, plus the filesystem and in-memory sources.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/source.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <unordered_map>

namespace catalyst::resource
{
    namespace
    {
        /**
         * Turns a URI path into a path relative to a mount root, and refuses anything that would
         * escape it. Content names files; content does not get to name files outside its mount.
         */
        std::expected<std::filesystem::path, error> relative_path_of(const uri &location)
        {
            auto decoded = location.decoded_path();
            if (!decoded)
                return std::unexpected(
                    make_error(error_code::invalid_uri, location.string(), decoded.error().message()));

            std::filesystem::path rel;
            for (const std::filesystem::path segment : std::filesystem::path{*decoded})
            {
                const std::string text = segment.string();
                if (text.empty() || text == "." || text == "/" || text == "\\")
                    continue;
                if (text == "..")
                    return std::unexpected(make_error(error_code::access_denied, location.string(),
                                                      "path escapes the mount root"));
                rel /= segment;
            }

            return rel;
        }

        class file_source final : public source
        {
        public:
            explicit file_source(std::filesystem::path root)
                : root_(std::move(root)), name_("file:" + root_.generic_string())
            {
            }

            [[nodiscard]] std::string_view name() const noexcept override { return name_; }

            [[nodiscard]] source_caps caps() const noexcept override
            {
                return source_caps::enumerable | source_caps::stattable | source_caps::mutable_entries;
            }

            [[nodiscard]] std::expected<blob, error> open(const uri &location) override
            {
                auto rel = relative_path_of(location);
                if (!rel)
                    return std::unexpected(std::move(rel.error()));

                const std::filesystem::path full = root_ / *rel;

                std::error_code ec;
                const auto size = std::filesystem::file_size(full, ec);
                if (ec)
                    return std::unexpected(make_error(error_code::not_found, location.string(), ec.message()));

                std::ifstream in(full, std::ios::binary);
                if (!in)
                    return std::unexpected(make_error(error_code::access_denied, location.string(),
                                                      "could not open for reading"));

                std::vector<std::byte> storage(static_cast<std::size_t>(size));
                if (size != 0)
                {
                    in.read(reinterpret_cast<char *>(storage.data()), static_cast<std::streamsize>(size));
                    if (in.gcount() != static_cast<std::streamsize>(size))
                        return std::unexpected(make_error(error_code::io_error, location.string(),
                                                          "short read"));
                }

                return blob::adopt(std::move(storage));
            }

            [[nodiscard]] bool exists(const uri &location) override
            {
                auto rel = relative_path_of(location);
                if (!rel)
                    return false;

                std::error_code ec;
                return std::filesystem::is_regular_file(root_ / *rel, ec);
            }

            [[nodiscard]] std::expected<entry_info, error> stat(const uri &location) override
            {
                auto rel = relative_path_of(location);
                if (!rel)
                    return std::unexpected(std::move(rel.error()));

                const std::filesystem::path full = root_ / *rel;

                std::error_code ec;
                const auto size = std::filesystem::file_size(full, ec);
                if (ec)
                    return std::unexpected(make_error(error_code::not_found, location.string(), ec.message()));

                const auto written = std::filesystem::last_write_time(full, ec);
                const std::uint64_t revision =
                    ec ? 0 : static_cast<std::uint64_t>(written.time_since_epoch().count());

                return entry_info{static_cast<std::uint64_t>(size), revision};
            }

            [[nodiscard]] std::expected<std::vector<uri>, error> list(const uri &prefix) override
            {
                auto rel = relative_path_of(prefix);
                if (!rel)
                    return std::unexpected(std::move(rel.error()));

                const std::filesystem::path base = root_ / *rel;

                std::error_code ec;
                std::filesystem::recursive_directory_iterator it(base, ec);
                if (ec)
                    return std::unexpected(make_error(error_code::not_found, prefix.string(), ec.message()));

                std::vector<uri> out;
                for (const auto &entry : it)
                {
                    if (!entry.is_regular_file(ec))
                        continue;

                    const std::string relative =
                        std::filesystem::relative(entry.path(), root_, ec).generic_string();
                    if (ec)
                        continue;

                    auto parsed = uri::parse(relative);
                    if (parsed)
                        out.push_back(std::move(*parsed));
                }

                std::sort(out.begin(), out.end());
                return out;
            }

        private:
            std::filesystem::path root_;
            std::string name_;
        };

        class memory_source final : public source
        {
        public:
            explicit memory_source(std::vector<std::pair<std::string, std::span<const std::byte>>> entries)
            {
                entries_.reserve(entries.size());
                for (auto &[path, bytes] : entries)
                    entries_.insert_or_assign(std::move(path), bytes);
            }

            [[nodiscard]] std::string_view name() const noexcept override { return "memory"; }

            [[nodiscard]] source_caps caps() const noexcept override
            {
                return source_caps::enumerable | source_caps::stattable;
            }

            [[nodiscard]] std::expected<blob, error> open(const uri &location) override
            {
                const auto it = entries_.find(key_of(location));
                if (it == entries_.end())
                    return std::unexpected(make_error(error_code::not_found, location.string()));

                // The table's bytes outlive the source by contract, so hand out a borrow.
                return blob::borrow(
                    std::span<std::byte>{const_cast<std::byte *>(it->second.data()), it->second.size()});
            }

            [[nodiscard]] bool exists(const uri &location) override
            {
                return entries_.contains(key_of(location));
            }

            [[nodiscard]] std::expected<entry_info, error> stat(const uri &location) override
            {
                const auto it = entries_.find(key_of(location));
                if (it == entries_.end())
                    return std::unexpected(make_error(error_code::not_found, location.string()));

                // Fixed table, so one revision for the life of the source.
                return entry_info{static_cast<std::uint64_t>(it->second.size()), 1};
            }

            [[nodiscard]] std::expected<std::vector<uri>, error> list(const uri &prefix) override
            {
                const std::string base = key_of(prefix);

                std::vector<uri> out;
                for (const auto &[path, bytes] : entries_)
                {
                    if (!path.starts_with(base))
                        continue;

                    auto parsed = uri::parse(path);
                    if (parsed)
                        out.push_back(std::move(*parsed));
                }

                std::sort(out.begin(), out.end());
                return out;
            }

        private:
            /// Entries are tabled without a leading slash; URI paths carry one.
            static std::string key_of(const uri &location)
            {
                std::string_view path = location.path();
                while (path.starts_with('/'))
                    path.remove_prefix(1);
                return std::string{path};
            }

            std::unordered_map<std::string, std::span<const std::byte>> entries_;
        };

    } // namespace

    // -----------------------------------------------------------------------------
    // source defaults
    // -----------------------------------------------------------------------------

    bool source::exists(const uri &location)
    {
        return open(location).has_value();
    }

    std::expected<entry_info, error> source::stat(const uri &location)
    {
        return std::unexpected(make_error(error_code::unsupported_format, location.string(),
                                          "source does not support stat"));
    }

    std::expected<std::vector<uri>, error> source::list(const uri &prefix)
    {
        return std::unexpected(make_error(error_code::unsupported_format, prefix.string(),
                                          "source does not support enumeration"));
    }

    // -----------------------------------------------------------------------------
    // Factories
    // -----------------------------------------------------------------------------

    std::unique_ptr<source> make_file_source(std::filesystem::path root)
    {
        return std::make_unique<file_source>(std::move(root));
    }

    std::unique_ptr<source>
    make_memory_source(std::vector<std::pair<std::string, std::span<const std::byte>>> entries)
    {
        return std::make_unique<memory_source>(std::move(entries));
    }

} // namespace catalyst::resource
