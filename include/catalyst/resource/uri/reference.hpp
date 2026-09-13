/**
 * @file reference.hpp
 * @brief The @ref catalyst::resource::uri value type: an owning, parsed RFC 3986 URI reference, its
 * component accessors, and its `std::hash` and `std::formatter` specializations.
 * @details A parsed `uri` owns its text and stores byte offsets into it, so every accessor here is a
 * `string_view` with no allocation. The class is declared whole here; the algorithms behind it are
 * split across the module by subject -- parsing and composition in parser.cpp, resolution and
 * normalization in resolver.cpp -- and the headers that go with those carry the helpers each is
 * built from. Include uri.hpp for the whole module.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "error.hpp"
#include "percent.hpp"

namespace catalyst::resource
{
    // -----------------------------------------------------------------
    // Uniform Resource Identifier (URI)
    // -----------------------------------------------------------------
    // URIs provide a standard way to identify resources within a system.
    // A URI typically consists of a scheme, an authority, a path, a query, and a fragment.
    //
    //   foo://user@example.com:8042/over/there?name=ferret#nose
    //   \_/  \__/ \_________/ \__/ \_________/ \_________/ \__/
    // scheme userinfo  host    port    path        query   fragment
    //        \______________________/
    //                authority
    //
    // The scheme specifies the protocol or method used to access the resource.
    // The authority typically includes the user information, host, and port.
    // The path specifies the location of the resource within the authority.
    // The query provides additional parameters for the resource.
    // The fragment identifies a specific part of the resource.

    /**
     * @class uri
     * @brief An owning, parsed URI reference.
     *
     * The text is held in one string and each component is a byte range into it, so every accessor
     * returns a `string_view` and costs nothing. A default-constructed `uri` is the empty relative
     * reference: no scheme, no authority, an empty path. That is a legal URI reference, so there is
     * no invalid state to check for -- @ref parse either gives you a `uri` or gives you a
     * @ref uri_error.
     *
     * Components come back exactly as they were written, still percent-encoded, because that is the
     * only form that round-trips. Use @ref percent_decode, or the `decoded_*` helpers, when you want
     * the bytes a component denotes rather than the bytes it is spelled with.
     */
    class uri
    {
    public:
        // A query parameter exactly as it appears in the text, still
        // percent-encoded. `value` is absent when the pair carried no "=" at
        // all, which is not the same as "k=" with an empty value.
        struct query_parameter
        {
            std::string_view name;
            std::optional<std::string_view> value;

            friend constexpr bool operator==(const query_parameter &, const query_parameter &) = default;
        };

        // The pieces from_parts() assembles. An absent component leaves no
        // trace in the text; a present but empty one still writes its
        // delimiter, so { .path = "", .query = "" } composes "?".
        struct parts
        {
            std::optional<std::string_view> scheme;
            std::optional<std::string_view> authority;
            std::string_view path;
            std::optional<std::string_view> query;
            std::optional<std::string_view> fragment;
        };

        // What to leave literal, on top of the unreserved set, for each place
        // an encoded string can go. "&", "=" and "+" are absent from
        // query_value on purpose: the first two separate parameters and the
        // third is read as a space by HTML form decoders.
        struct encode_set
        {
            static constexpr std::string_view path_segment = "!$&'()*+,;=:@";
            static constexpr std::string_view query_value = "!$'()*,;:@/?";
            static constexpr std::string_view fragment = "!$&'()*+,;=:@/?";
            static constexpr std::string_view userinfo = "!$&'()*+,;=:";
        };

    public:
        uri() = default;

        // -----------------------------------------------------------------
        // Parsing and composition
        // -----------------------------------------------------------------

        /**
         * @fn parse
         * @brief Parse @p text as an RFC 3986 URI reference.
         *
         * Accepts both absolute URIs (`pack:core/tex.png`) and relative references
         * (`../textures/tex.png`, `//host/p`, `?q`, `#f`); ask @ref is_absolute which you got. The
         * text is copied, so @p text need not outlive the result.
         *
         * @param text The text to parse. Empty text is valid: it is the empty relative reference.
         * @return The parsed URI, or the first @ref uri_error found.
         */
        [[nodiscard]] static std::expected<uri, uri_error> parse(std::string_view text);

        /**
         * @fn from_parts
         * @brief Compose a URI from components and parse the result.
         *
         * The components are written with their delimiters and the composed text is then validated,
         * so this cannot produce a `uri` whose own text would not reparse. Components are taken as
         * already percent-encoded; run @ref percent_encode with the matching @ref encode_set first
         * if they are not.
         *
         * @param p The components to assemble.
         * @return The composed URI, or the @ref uri_error the composed text was rejected with.
         */
        [[nodiscard]] static std::expected<uri, uri_error> from_parts(const parts &p);

        /**
         * @fn resolve(const uri &base, const uri &ref)
         * @brief Resolve @p ref against @p base per RFC 3986 section 5.2.2.
         *
         * This is what turns a manifest-relative name into something loadable: `../textures/x.png`
         * against `pack:core/materials/stone.json` is `pack:core/textures/x.png`. An absolute @p ref
         * is returned normalized-of-dot-segments but otherwise unchanged.
         *
         * @param base The base URI. Must be absolute (must have a scheme).
         * @param ref The reference to resolve.
         * @return The resolved target, or @ref uri_error_code::base_not_absolute.
         */
        [[nodiscard]] static std::expected<uri, uri_error> resolve(const uri &base, const uri &ref);

        /**
         * @fn resolve(const uri &ref) const
         * @brief Resolve @p ref against this URI as the base.
         * @param ref The reference to resolve.
         * @return The resolved target, or @ref uri_error_code::base_not_absolute if `*this` has no scheme.
         */
        [[nodiscard]] std::expected<uri, uri_error> resolve(const uri &ref) const { return resolve(*this, ref); }

        /**
         * @fn normalized()
         * @brief Syntax-based normalization: the form to compare and to cache by.
         *
         * Applies exactly the transformations RFC 3986 section 6.2.2 calls safe, which are the ones
         * that cannot change which resource is named:
         *  - the scheme and the host are lowercased;
         *  - percent-escapes are uppercased (`%7e` becomes `%7E`);
         *  - escapes of unreserved bytes are decoded (`%7E` becomes `~`);
         *  - `.` and `..` segments are removed from the path;
         *  - an empty path becomes `/` when an authority is present.
         *
         * Case in the path, query and fragment is left alone, and an empty-but-present query or
         * fragment keeps its delimiter: both of those are significant to some schemes.
         *
         * @return A new URI; `*this` is unchanged.
         */
        [[nodiscard]] uri normalized() const;

        /**
         * @fn equivalent
         * @brief Compare two URIs after @ref normalized.
         *
         * `operator==` compares the text byte for byte, which is what a cache of already-normalized
         * keys wants. This is the more forgiving comparison, at the cost of normalizing both sides.
         */
        [[nodiscard]] static bool equivalent(const uri &a, const uri &b);

        // -----------------------------------------------------------------
        // Whole-URI access
        // -----------------------------------------------------------------

        /// @brief The URI text, exactly as parsed or composed.
        [[nodiscard]] std::string_view view() const noexcept { return text_; }

        /// @brief The URI text as the owned string.
        [[nodiscard]] const std::string &string() const noexcept { return text_; }

        /// @return `true` if this is the empty relative reference.
        [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

        /// @return `true` if a scheme is present, i.e. this is a URI rather than a relative reference.
        [[nodiscard]] bool is_absolute() const noexcept { return scheme_len_ != 0; }

        /// @return `true` if no scheme is present.
        [[nodiscard]] bool is_relative() const noexcept { return scheme_len_ == 0; }

        // -----------------------------------------------------------------
        // Components, as written (still percent-encoded)
        // -----------------------------------------------------------------

        /// @brief The scheme without its `:`, e.g. `pack`, or `nullopt` for a relative reference.
        [[nodiscard]] std::optional<std::string_view> scheme() const noexcept;

        /// @brief The authority without its leading `//`, or `nullopt` if there was no `//`.
        [[nodiscard]] std::optional<std::string_view> authority() const noexcept;

        /// @brief The path. Always present; may be empty.
        [[nodiscard]] std::string_view path() const noexcept;

        /// @brief The query without its leading `?`, or `nullopt` if there was no `?`.
        [[nodiscard]] std::optional<std::string_view> query() const noexcept;

        /// @brief The fragment without its leading `#`, or `nullopt` if there was no `#`.
        [[nodiscard]] std::optional<std::string_view> fragment() const noexcept;

        /// @brief The userinfo without its trailing `@`, or `nullopt` if the authority had none.
        [[nodiscard]] std::optional<std::string_view> userinfo() const noexcept;

        /**
         * @fn host()
         * @brief The host, with the brackets of an IP-literal kept.
         *
         * The brackets stay because they are what distinguishes `[::1]` from a registered name and
         * because dropping them would not round-trip. An authority may legitimately have an empty
         * host (`file:///path`), which is why this is an empty view rather than `nullopt` in that case.
         */
        [[nodiscard]] std::optional<std::string_view> host() const noexcept;

        /// @brief The port digits without the `:`, or `nullopt` if the authority carried no `:port`.
        [[nodiscard]] std::optional<std::string_view> port() const noexcept;

        /**
         * @fn port_number()
         * @brief The port as a number.
         * @return The port, or `nullopt` when absent or empty (`host:` means "the default port").
         */
        [[nodiscard]] std::optional<std::uint16_t> port_number() const noexcept;

        // -----------------------------------------------------------------
        // Query parameters
        // -----------------------------------------------------------------

        /**
         * @fn query_parameters()
         * @brief Split the query on `&` (and `;`) into name/value pairs, still percent-encoded.
         *
         * A pair with no `=` yields an absent `value`, which is how a flag (`?verbose`) is told apart
         * from an empty value (`?verbose=`). Empty pairs (`?a=1&&b=2`) are skipped. The returned
         * views borrow from this URI.
         *
         * @return The parameters in the order they appear; duplicates are kept, since `?a=1&a=2` is
         *         meaningful to plenty of schemes.
         */
        [[nodiscard]] std::vector<query_parameter> query_parameters() const;

        /**
         * @fn find_query_parameter
         * @brief The first parameter named @p name, compared as written.
         * @param name The parameter name, percent-encoded the same way it appears in the query.
         * @return The parameter, or `nullopt` if there is none by that name.
         */
        [[nodiscard]] std::optional<query_parameter> find_query_parameter(std::string_view name) const;

        // -----------------------------------------------------------------
        // Decoded convenience accessors
        // -----------------------------------------------------------------

        /// @brief The path with its escapes decoded. Fails only on a malformed escape, which a
        /// parsed URI cannot contain, so this is infallible in practice on a `uri` from @ref parse.
        [[nodiscard]] std::expected<std::string, uri_error> decoded_path() const;

        /// @brief The fragment with its escapes decoded, or `nullopt` if there is no fragment.
        [[nodiscard]] std::optional<std::string> decoded_fragment() const;

        /**
         * @fn path_segments()
         * @brief The path split on `/`, still percent-encoded, with empty segments dropped.
         *
         * Dropping empties means a leading `/` does not produce an empty first segment and `a//b`
         * yields two segments, which is what a caller walking a virtual directory tree wants. Use
         * @ref path when the exact spelling matters.
         */
        [[nodiscard]] std::vector<std::string_view> path_segments() const;

        // -----------------------------------------------------------------
        // Comparison
        // -----------------------------------------------------------------

        /// @brief Byte-for-byte comparison of the URI text. See @ref equivalent for the looser one.
        [[nodiscard]] friend bool operator==(const uri &a, const uri &b) noexcept { return a.text_ == b.text_; }

        /// @brief Lexicographic ordering of the URI text, so a `uri` can key an ordered map.
        [[nodiscard]] friend std::strong_ordering operator<=>(const uri &a, const uri &b) noexcept
        {
            return a.text_ <=> b.text_;
        }

    private:
        /// @brief Sentinel offset meaning "this component is absent".
        static constexpr std::uint32_t npos32 = 0xFFFFFFFFu;

        [[nodiscard]] std::optional<std::string_view> component(std::uint32_t off, std::uint32_t len) const noexcept;

        /// @brief Length of the host at the front of @p host_and_port, brackets included.
        [[nodiscard]] static std::size_t host_length(std::string_view host_and_port) noexcept;

        std::string text_{}; ///< The whole URI reference; every component is a range into this.

        std::uint32_t scheme_len_ = 0; ///< Scheme length; 0 means absent (a scheme is never empty).
        std::uint32_t authority_off_ = npos32, authority_len_ = 0;
        std::uint32_t path_off_ = 0, path_len_ = 0;
        std::uint32_t query_off_ = npos32, query_len_ = 0;
        std::uint32_t fragment_off_ = npos32, fragment_len_ = 0;
    };

} // namespace catalyst::resource

// -------------------------------------------------------------------------
// std integration: hashing, std::format
// -------------------------------------------------------------------------

namespace std
{
    template <>
    struct hash<catalyst::resource::uri>
    {
        std::size_t operator()(const catalyst::resource::uri &u) const noexcept
        {
            return std::hash<std::string_view>{}(u.view());
        }
    };

    // Formats as the URI text and takes the same specifiers a string does,
    // so std::format("{:>40}", u) right-aligns it.
    template <>
    struct formatter<catalyst::resource::uri, char> : formatter<std::string_view, char>
    {
        template <typename FormatContext>
        auto format(const catalyst::resource::uri &u, FormatContext &ctx) const
        {
            return formatter<std::string_view, char>::format(u.view(), ctx);
        }
    };
} // namespace std
