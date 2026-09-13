/**
 * @file parser.hpp
 * @brief The catalyst::resource::json parser: a recursive-descent RFC 8259 parser written once against a sink
 * interface, the two sinks that build a @ref value tree or a @ref document, and the @ref parse /
 * @ref parse_document entry points that report malformed input through `std::expected`.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "document.hpp"
#include "error.hpp"
#include "scan.hpp"
#include "value.hpp"

namespace catalyst::resource::json
{
    /**
     * @var max_depth
     * @brief The maximum container nesting depth the parsers accept. Deeper input is rejected with
     * @ref parse_error_code::nesting_too_deep so hostile documents cannot exhaust the stack.
     */
    inline constexpr std::size_t max_depth = 512;

    /// @brief Implementation details; not part of the public API.
    namespace detail
    {
        // -------------------------------------------------------------------------------------------
        // Failure signalling
        // -------------------------------------------------------------------------------------------

        /**
         * @class parse_failure
         * @brief Unwinds the recursive parser to the entry point, which turns it into a @ref parse_error.
         *
         * The grammar is recursive and every leaf can fail, so threading a status through each return
         * would add a branch to every hot path. A single throw per failed parse is cheaper and never
         * escapes @ref run, so callers see only the `std::expected` result.
         */
        class parse_failure final : public std::exception
        {
        public:
            explicit parse_failure(parse_error e) noexcept : error(e) {}
            [[nodiscard]] const char *what() const noexcept override { return "JSON parse failure"; }
            parse_error error;
        };

        /// @brief Abort the parse with @p code at byte offset @p pos.
        [[noreturn]] void fail_at(std::size_t pos, parse_error_code code);

        // -------------------------------------------------------------------------------------------
        // Shared parsing primitives (the grammar's leaves)
        // -------------------------------------------------------------------------------------------

        /// @brief Parse exactly four hexadecimal digits of a `\u` escape.
        /// @param pos In/out cursor; advanced past the four digits on success.
        [[nodiscard]] unsigned parse_hex4(std::string_view s, std::size_t &pos);

        /**
         * @fn decode_string_into
         * @brief Decode a JSON string body, appending the result to @p out.
         *
         * On entry @p pos points just past the opening quote; on return it points just past the closing
         * quote. The fast path (no escapes) is a single bulk append; escapes fall into a byte-wise loop.
         * This is the one place strings are decoded, so the escape/UTF-8 rules live here only.
         */
        void decode_string_into(std::string_view s, std::size_t &pos, std::string &out);

        /**
         * @struct number_token
         * @brief A scanned numeric token, with the pieces the fast conversion needs.
         *
         * While validating the grammar the scanner also accumulates the decimal digits into @ref mant
         * (dropping the decimal point and tracking it in @ref exp10) as long as there are at most 19
         * significant digits: enough to hold any `int64` and the whole Clinger fast-path range for
         * doubles. If the digits or exponent are too long for that, @ref fast is false and conversion
         * falls back to `std::from_chars` on @ref text.
         */
        struct number_token
        {
            std::string_view text;  ///< The token's characters.
            bool is_float = false;  ///< `true` if it has a fraction or exponent.
            bool neg = false;       ///< `true` if it has a leading '-'.
            bool fast = true;       ///< `true` if `mant`/`exp10` describe it exactly.
            std::uint64_t mant = 0; ///< Significant digits as an integer.
            int exp10 = 0;          ///< Power of ten to apply to `mant`.
        };

        /**
         * @fn scan_number
         * @brief Scan a JSON number, advancing @p pos to just past it.
         *
         * Validates the grammar (sign, no leading zeros, optional fraction and exponent) and gathers the
         * digits for @ref convert_number.
         */
        [[nodiscard]] number_token scan_number(std::string_view s, std::size_t &pos);

        /// @brief The converted form of a @ref number_token.
        struct number_value
        {
            bool is_int; ///< `true` if `i` holds the value, else `d` does.
            std::int64_t i;
            double d;
        };

        /**
         * @fn convert_number
         * @brief Convert a scanned number to `int64` (when it fits) or `double`.
         *
         * Integers that fit in `int64` become integers; everything else (fractional, exponential, or
         * overflowing) becomes a double.
         *
         * Doubles take Clinger's fast path when it is exact: a mantissa of at most 2^53 scaled by a
         * power of ten up to 10^22 is one correctly rounded multiply or divide, because both operands
         * are exactly representable. Anything else goes through `std::from_chars`, which is always
         * correctly rounded but several times slower.
         *
         * Fails with @ref parse_error_code::number_out_of_range if a double cannot hold the value.
         */
        [[nodiscard]] number_value convert_number(const number_token &n, std::size_t pos);

        // -------------------------------------------------------------------------------------------
        // The grammar, written once: basic_parser<Sink>
        // -------------------------------------------------------------------------------------------

        /**
         * @class basic_parser
         * @brief Recursive-descent JSON parser (RFC 8259) that reports to a sink.
         *
         * The grammar lives here exactly once. What gets *built* is up to the `Sink`: @ref value_sink
         * assembles an owning `value` tree, @ref tape_sink emits a `document`'s tape. Every open
         * container is represented by a sink-defined *handle* that lives on the parser's stack frame
         * for that container, so a sink can keep per-container state (a growing `array`, a tape header
         * index) with no side stack of its own. A sink provides:
         *
         *  - `root_handle root()` and `finish(root_handle&)` -> the parse result
         *  - `on_null(H&)`, `on_bool(H&, bool)`, `on_int(H&, int64)`, `on_double(H&, double)`:
         *    a scalar under the container handle `H`
         *  - `std::string& begin_string(H&)` / `end_string(H&)`: the parser decodes the string body
         *    straight into the returned buffer
         *  - `std::string& begin_key(object_handle&)` / `end_key(object_handle&)`
         *  - `array_handle begin_array()` / `end_array(H& parent, array_handle&, count)`
         *  - `object_handle begin_object()` / `end_object(H& parent, object_handle&, count)`
         *
         * Nesting deeper than @ref max_depth is rejected. Failure is signalled by throwing
         * @ref parse_failure; use @ref run to get a `std::expected` instead.
         */
        template <class Sink>
        class basic_parser
        {
        public:
            /// @brief What the sink's `finish` produces: a `value` or a `document`.
            using result_type = decltype(std::declval<Sink &>().finish(std::declval<typename Sink::root_handle &>()));

            /// @param text The JSON text to parse; not copied, must outlive the parse.
            basic_parser(std::string_view text, Sink &sink) noexcept : text_(text), sink_(sink) {}

            /// @brief Parse the whole input as a single JSON document.
            /// @throws parse_failure on malformed input or trailing characters.
            result_type parse()
            {
                auto root = sink_.root();
                skip_ws();
                parse_value(root, 0);
                skip_ws();
                if (pos_ != text_.size())
                    fail(parse_error_code::trailing_characters);
                return sink_.finish(root);
            }

        private:
            std::string_view text_; ///< The input text being parsed.
            std::size_t pos_ = 0;   ///< Current read offset into @ref text_.
            Sink &sink_;            ///< Receives the parsed events.

            [[noreturn]] void fail(parse_error_code code) const { fail_at(pos_, code); }
            [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }
            [[nodiscard]] char peek() const noexcept { return at_end() ? '\0' : text_[pos_]; }
            void skip_ws() noexcept { pos_ = scan::ws_scalar(text_, pos_); }

            /// @brief The right code for "wanted X here": end-of-input if that is what we hit.
            [[nodiscard]] parse_error_code here_or_end(parse_error_code code) const noexcept
            {
                return at_end() ? parse_error_code::unexpected_end : code;
            }

            /// @brief Consume an exact literal at the current position.
            void expect_literal(std::string_view lit)
            {
                if (text_.size() - pos_ < lit.size() || text_.substr(pos_, lit.size()) != lit)
                    fail(parse_error_code::invalid_literal);
                pos_ += lit.size();
            }

            /// @brief Parse any JSON value into the container `parent`.
            /// @note A 256-entry jump-table dispatch was benchmarked here and measured as a no-op; the
            /// compiler already lowers this small switch to efficient code.
            template <class Parent>
            void parse_value(Parent &parent, std::size_t depth)
            {
                if (at_end())
                    fail(parse_error_code::unexpected_end);
                const char c = peek();
                switch (c)
                {
                case '{':
                    parse_object(parent, depth);
                    break;
                case '[':
                    parse_array(parent, depth);
                    break;
                case '"':
                    parse_string(parent);
                    break;
                case 't':
                    expect_literal("true");
                    sink_.on_bool(parent, true);
                    break;
                case 'f':
                    expect_literal("false");
                    sink_.on_bool(parent, false);
                    break;
                case 'n':
                    expect_literal("null");
                    sink_.on_null(parent);
                    break;
                default:
                    if (c == '-' || (c >= '0' && c <= '9'))
                        parse_number(parent);
                    else
                        fail(parse_error_code::unexpected_character);
                    break;
                }
            }

            template <class Parent>
            void parse_number(Parent &parent)
            {
                const std::size_t start = pos_;
                const number_value nv = convert_number(scan_number(text_, pos_), start);
                if (nv.is_int)
                    sink_.on_int(parent, nv.i);
                else
                    sink_.on_double(parent, nv.d);
            }

            /// @brief Parse a string value (the current character is the opening quote).
            template <class Parent>
            void parse_string(Parent &parent)
            {
                ++pos_; // consume opening '"'
                decode_string_into(text_, pos_, sink_.begin_string(parent));
                sink_.end_string(parent);
            }

            void enter(std::size_t depth)
            {
                if (depth >= max_depth)
                    fail(parse_error_code::nesting_too_deep);
            }

            template <class Parent>
            void parse_array(Parent &parent, std::size_t depth)
            {
                enter(depth);
                ++pos_; // consume '['
                auto arr = sink_.begin_array();
                std::uint64_t count = 0;
                skip_ws();
                if (peek() == ']')
                {
                    ++pos_;
                }
                else
                {
                    while (true)
                    {
                        parse_value(arr, depth + 1);
                        ++count;
                        skip_ws();
                        const char c = peek();
                        if (c == ']')
                        {
                            ++pos_;
                            break;
                        }
                        if (c != ',')
                            fail(here_or_end(parse_error_code::expected_comma_or_bracket));
                        ++pos_;
                        skip_ws();
                    }
                }
                sink_.end_array(parent, arr, count);
            }

            template <class Parent>
            void parse_object(Parent &parent, std::size_t depth)
            {
                enter(depth);
                ++pos_; // consume '{'
                auto obj = sink_.begin_object();
                std::uint64_t count = 0;
                skip_ws();
                if (peek() == '}')
                {
                    ++pos_;
                }
                else
                {
                    while (true)
                    {
                        if (peek() != '"')
                            fail(here_or_end(parse_error_code::expected_key));
                        ++pos_; // consume opening '"'
                        decode_string_into(text_, pos_, sink_.begin_key(obj));
                        sink_.end_key(obj);
                        skip_ws();
                        if (peek() != ':')
                            fail(here_or_end(parse_error_code::expected_colon));
                        ++pos_;
                        skip_ws();
                        parse_value(obj, depth + 1);
                        ++count;
                        skip_ws();
                        const char c = peek();
                        if (c == '}')
                        {
                            ++pos_;
                            break;
                        }
                        if (c != ',')
                            fail(here_or_end(parse_error_code::expected_comma_or_brace));
                        ++pos_;
                        skip_ws();
                    }
                }
                sink_.end_object(parent, obj, count);
            }
        };

        /**
         * @fn run
         * @brief Drive a @ref basic_parser over @p text and report the outcome as `std::expected`.
         *
         * This is the only place @ref parse_failure is caught; everything above it sees a plain value.
         * `std::bad_alloc` is not a parse error and propagates.
         */
        template <class Sink>
        [[nodiscard]] std::expected<typename basic_parser<Sink>::result_type, parse_error> run(std::string_view text,
                                                                                               Sink &sink)
        {
            try
            {
                return basic_parser<Sink>(text, sink).parse();
            }
            catch (const parse_failure &f)
            {
                return std::unexpected(f.error);
            }
        }

        /**
         * @class value_sink
         * @brief Sink that assembles an owning @ref value tree.
         *
         * Each handle *is* the container being filled (plus the pending key for objects), living on the
         * parser's stack frame; a completed child is pushed straight into it. That is exactly the data
         * flow of a hand-written recursive builder, with the grammar factored out.
         */
        class value_sink
        {
        public:
            /**
             * @brief Initial capacity given to a container on its first insert.
             *
             * Skips the 1->2->4 reallocation cascade for the small containers that dominate real
             * documents. Measured: 4 beat 8 on every benchmark payload, because 8 slots of a 40-byte
             * `value` is 320 bytes for what is typically a 2-5 element container.
             */
            static constexpr std::size_t kInitialReserve = 4;

            struct root_handle
            {
                value v;
                void add(value &&x) noexcept { v = std::move(x); }
            };
            struct array_handle
            {
                array arr;
                void add(value &&x);
            };
            struct object_handle
            {
                object obj;
                std::string key; ///< Decoded in place; consumed by the next `add`.
                void add(value &&x);
            };

            [[nodiscard]] root_handle root() const { return {}; }
            [[nodiscard]] value finish(root_handle &r) noexcept { return std::move(r.v); }

            template <class H>
            void on_null(H &h)
            {
                h.add(value());
            }
            template <class H>
            void on_bool(H &h, bool b)
            {
                h.add(value(b));
            }
            template <class H>
            void on_int(H &h, std::int64_t i)
            {
                h.add(value(i));
            }
            template <class H>
            void on_double(H &h, double d)
            {
                h.add(value(d));
            }

            template <class H>
            std::string &begin_string(H &)
            {
                scratch_.clear();
                return scratch_;
            }
            template <class H>
            void end_string(H &h)
            {
                h.add(value(std::move(scratch_)));
            }

            std::string &begin_key(object_handle &h);
            void end_key(object_handle &) noexcept {}

            [[nodiscard]] array_handle begin_array() const { return {}; }
            template <class H>
            void end_array(H &parent, array_handle &a, std::uint64_t)
            {
                parent.add(value(std::move(a.arr)));
            }
            [[nodiscard]] object_handle begin_object() const { return {}; }
            template <class H>
            void end_object(H &parent, object_handle &o, std::uint64_t)
            {
                parent.add(value(std::move(o.obj)));
            }

        private:
            std::string scratch_; ///< String values are decoded here, then moved out.
        };

    } // namespace detail

    // -----------------------------------------------------------------------------------------------
    // Entry points
    // -----------------------------------------------------------------------------------------------

    /**
     * @fn parse(std::string_view text)
     * @brief Parse JSON text into an owning @ref value tree.
     * @param text The text to parse. It is not retained; the result owns all of its data.
     * @return The parsed value, or a @ref parse_error describing the first problem found.
     */
    [[nodiscard]] std::expected<value, parse_error> parse(std::string_view text);

    /**
     * @fn parse_document(std::string_view text)
     * @brief Parse JSON text into a flat @ref document for fast read-only traversal.
     * @param text The text to parse. It is not retained; the document owns all of its data.
     * @return The parsed document, or a @ref parse_error describing the first problem found.
     */
    [[nodiscard]] std::expected<document, parse_error> parse_document(std::string_view text);

} // namespace catalyst::resource::json
