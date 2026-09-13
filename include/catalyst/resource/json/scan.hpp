/**
 * @file scan.hpp
 * @brief JSON's stop-byte sets, bound to the shared scanners in @ref catalyst::text::scan.
 * @details The scanning itself -- the SWAR masks, the tail handling, the scalar reference -- is not
 * specific to JSON and lives in `<catalyst/text/scan.hpp>`. What is specific to JSON is *which* bytes
 * end a string run: the closing quote, an escape, or an unescaped control byte. That is the same set
 * the serializer has to escape, so both directions share these names.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/text/scan.hpp>

#include <cstddef>
#include <string_view>

namespace catalyst::resource::json::detail::scan
{
    /**
     * @fn scalar
     * @brief Scalar (byte-at-a-time) string-body scan.
     *
     * Returns the index of the first byte that ends a JSON string run: the closing quote (`"`), an
     * escape (`\`), or an unescaped control byte (`< 0x20`), or `s.size()` if the input ends first.
     * UTF-8 lead/continuation bytes (`>= 0x80`) never end a run.
     *
     * The reference implementation that @ref swar must agree with on every input.
     */
    [[nodiscard]] inline std::size_t scalar(std::string_view s, std::size_t from) noexcept
    {
        return text::scan::scalar<text::scan::control_bytes::stop, '"', '\\'>(s, from);
    }

    /**
     * @fn swar
     * @brief SWAR string-body scan: the production fast path, 8 bytes per iteration.
     *
     * Returns the same index as @ref scalar; a differential check in `tests/resource/test_json.cpp`
     * pins that they agree, and `tests/text/test_scan.cpp` pins the underlying scanners across every
     * stop set.
     */
    [[nodiscard]] inline std::size_t swar(std::string_view s, std::size_t from) noexcept
    {
        return text::scan::swar<text::scan::control_bytes::stop, '"', '\\'>(s, from);
    }

    /// @brief Test whether a byte is JSON whitespace (space, tab, LF, CR).
    using text::scan::is_ws;

    /// @brief Skip a run of JSON whitespace; returns the index of the first byte that is not.
    [[nodiscard]] inline std::size_t ws_scalar(std::string_view s, std::size_t from) noexcept
    {
        return text::scan::skip_ws(s, from);
    }

} // namespace catalyst::resource::json::detail::scan
