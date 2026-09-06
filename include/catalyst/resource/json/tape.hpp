/**
 * @file tape.hpp
 * @brief The flat "tape" encoding of a parsed JSON document and the non-owning @ref cursor that navigates it.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "error.hpp"
#include "value.hpp"

namespace catalyst::resource::json
{
    // -----------------------------------------------------------------------------------------------
    // Tape DOM: a flat, allocation-free representation of a parsed document
    // -----------------------------------------------------------------------------------------------
    //
    // The owning `value` tree is the ergonomic API, but it heap-allocates a node per array/object/
    // string. For a parse-and-read workload that allocation traffic, not the scanning, is the ceiling.
    // The `document` (see document.hpp) is the fast alternative: the whole tree is encoded as a flat
    // tape of 64-bit words plus one contiguous string arena, navigated by a lightweight non-owning
    // `cursor`. No per-node allocation; the parser fills two growable buffers and stops.

    namespace detail::tape
    {
        /**
         * @brief Tape encoding: one (or two) `uint64_t` words per node.
         *
         * Each node begins with a tag word: the high byte is a @ref tag, the low 56 bits are a per-tag
         * payload. The word count of a node is fixed by its tag, which is what lets a @ref cursor walk
         * and skip subtrees:
         *
         *  - `t_null` / `t_false` / `t_true` : 1 word (no payload).
         *  - `t_int` / `t_double`            : tag word + 1 raw `bit_cast` word.
         *  - `t_string`                      : 1 word, payload = byte offset into the arena, where a
         *                                      4-byte little-endian length precedes the bytes.
         *  - `t_array` / `t_object`          : tag word + 1 count word. The tag payload is the tape
         *                                      index just past the whole container (the O(1) skip
         *                                      target); the second word is the element/member count.
         *                                      An object's members are laid out as alternating key (a
         *                                      `t_string`) then value, so it has `2 * count` children.
         *
         * Limits that follow from the layout: an arena offset is 56 bits and a string length is 32 bits
         * (a single string over 4 GiB is not supported).
         */
        enum tag : std::uint8_t
        {
            t_null = 0,   ///< JSON `null`.
            t_false = 1,  ///< JSON `false`.
            t_true = 2,   ///< JSON `true`.
            t_int = 3,    ///< Integer; raw `int64` in the following word.
            t_double = 4, ///< Double; raw `double` in the following word.
            t_string = 5, ///< String; payload is the arena offset.
            t_array = 6,  ///< Array; payload is the skip index, +1 count word.
            t_object = 7, ///< Object; payload is the skip index, +1 count word.
        };

        constexpr int kTagShift = 56; ///< Payload occupies the low 56 bits.
        constexpr std::uint64_t kPayloadMask = (std::uint64_t(1) << kTagShift) - 1;

        [[nodiscard]] constexpr std::uint64_t make(tag t, std::uint64_t payload = 0) noexcept
        {
            return (std::uint64_t(t) << kTagShift) | (payload & kPayloadMask);
        }
        [[nodiscard]] constexpr tag tag_of(std::uint64_t w) noexcept { return static_cast<tag>(w >> kTagShift); }
        [[nodiscard]] constexpr std::uint64_t payload_of(std::uint64_t w) noexcept { return w & kPayloadMask; }

        /// @brief The tape index immediately past the node beginning at @p i.
        [[nodiscard]] inline std::size_t node_end(const std::uint64_t *tape, std::size_t i) noexcept
        {
            switch (tag_of(tape[i]))
            {
            case t_int:
            case t_double:
                return i + 2;
            case t_array:
            case t_object:
                return static_cast<std::size_t>(payload_of(tape[i]));
            default:
                return i + 1; // null, bool, string
            }
        }

    } // namespace detail::tape

    /**
     * @class cursor
     * @brief A non-owning handle to one node of a @ref document.
     *
     * A cursor is a nullable handle in the same spirit as a pointer: a default-constructed cursor is
     * invalid (see @ref valid), and that is how @ref find reports an absent key and how an empty
     * document reports its root. Every other member requires a valid cursor. Cursors borrow from the
     * document that produced them and must not outlive it.
     */
    class cursor
    {
    public:
        cursor() noexcept = default;

        /// @brief Construct a cursor over a tape node.
        cursor(const std::uint64_t *tape, const char *strings, std::size_t idx) noexcept
            : tape_(tape), strings_(strings), idx_(idx)
        {
        }

        /**
         * @fn valid()
         * @brief Test whether this cursor refers to an actual node.
         * @return `false` for a default-constructed cursor (a failed @ref find, or the root of an empty
         *         document).
         */
        [[nodiscard]] bool valid() const noexcept { return tape_ != nullptr; }

        /// @brief The dynamic type of the referenced node.
        [[nodiscard]] type kind() const noexcept
        {
            switch (tag())
            {
            case detail::tape::t_null:
                return type::null;
            case detail::tape::t_false:
            case detail::tape::t_true:
                return type::boolean;
            case detail::tape::t_int:
                return type::integer;
            case detail::tape::t_double:
                return type::floating;
            case detail::tape::t_string:
                return type::string;
            case detail::tape::t_array:
                return type::array;
            default:
                return type::object;
            }
        }

        [[nodiscard]] bool is_null() const noexcept { return tag() == detail::tape::t_null; }
        [[nodiscard]] bool is_boolean() const noexcept
        {
            return tag() == detail::tape::t_false || tag() == detail::tape::t_true;
        }
        [[nodiscard]] bool is_integer() const noexcept { return tag() == detail::tape::t_int; }
        [[nodiscard]] bool is_floating() const noexcept { return tag() == detail::tape::t_double; }
        [[nodiscard]] bool is_number() const noexcept { return is_integer() || is_floating(); }
        [[nodiscard]] bool is_string() const noexcept { return tag() == detail::tape::t_string; }
        [[nodiscard]] bool is_array() const noexcept { return tag() == detail::tape::t_array; }
        [[nodiscard]] bool is_object() const noexcept { return tag() == detail::tape::t_object; }

        /// @name Checked access
        /// Each accessor throws @ref type_error on a type mismatch.
        /// @{

        /// @throws type_error if the node is not a boolean.
        [[nodiscard]] bool as_bool() const
        {
            if (tag() == detail::tape::t_true)
                return true;
            if (tag() == detail::tape::t_false)
                return false;
            throw type_error("value is not a boolean");
        }

        /// @throws type_error if the node is not an integer.
        [[nodiscard]] std::int64_t as_int() const
        {
            if (!is_integer())
                throw type_error("value is not an integer");
            return raw_int();
        }

        /// @brief The numeric payload as a `double`; integers are widened.
        /// @throws type_error if the node is not a number.
        [[nodiscard]] double as_double() const
        {
            if (is_integer())
                return static_cast<double>(raw_int());
            if (is_floating())
                return raw_double();
            throw type_error("value is not a number");
        }

        /// @brief The string payload as a view into the document's arena.
        /// @throws type_error if the node is not a string.
        [[nodiscard]] std::string_view as_string() const
        {
            if (!is_string())
                throw type_error("value is not a string");
            return raw_string();
        }

        /// @}

        /// @name Unchecked access
        /// Each accessor returns `std::nullopt` on a type mismatch and never throws.
        /// @{

        /// @brief The boolean payload, or `std::nullopt` if this is not a boolean.
        [[nodiscard]] std::optional<bool> try_bool() const noexcept
        {
            if (tag() == detail::tape::t_true)
                return true;
            if (tag() == detail::tape::t_false)
                return false;
            return std::nullopt;
        }

        /// @brief The integer payload, or `std::nullopt` if this is not an integer.
        [[nodiscard]] std::optional<std::int64_t> try_int() const noexcept
        {
            if (is_integer())
                return raw_int();
            return std::nullopt;
        }

        /// @brief The numeric payload as a `double` (integers widened), or `std::nullopt` if not a number.
        [[nodiscard]] std::optional<double> try_double() const noexcept
        {
            if (is_integer())
                return static_cast<double>(raw_int());
            if (is_floating())
                return raw_double();
            return std::nullopt;
        }

        /// @brief A view of the string payload, or `std::nullopt` if this is not a string.
        [[nodiscard]] std::optional<std::string_view> try_string() const noexcept
        {
            if (is_string())
                return raw_string();
            return std::nullopt;
        }

        /// @}

        /// @brief Number of contained elements; 0 for every non-container type.
        [[nodiscard]] std::size_t size() const noexcept
        {
            if (is_array() || is_object())
                return static_cast<std::size_t>(tape_[idx_ + 1]);
            return 0;
        }

        /// @name Navigation
        /// @{

        /**
         * @fn first_child()
         * @brief A cursor to the first child node of a container.
         * @pre This node is an array or object. For an object the first child is the first key; its
         *      value is `first_child().next_sibling()`.
         */
        [[nodiscard]] cursor first_child() const noexcept { return cursor(tape_, strings_, idx_ + 2); }

        /**
         * @fn next_sibling()
         * @brief A cursor to the node immediately following this one.
         *
         * Within a container this is the next sibling; it skips entire subtrees in O(1) for nested
         * arrays/objects.
         */
        [[nodiscard]] cursor next_sibling() const noexcept
        {
            return cursor(tape_, strings_, detail::tape::node_end(tape_, idx_));
        }

        class element_iterator; ///< Forward iterator over an array's element cursors.
        class member_iterator;  ///< Forward iterator over an object's @ref member pairs.

        /// @brief One key/value pair of an object, as yielded by @ref members().
        struct member;

        /// @brief A range of child cursors / members, usable in a range-for.
        template <class It>
        struct range
        {
            It first, last;
            [[nodiscard]] It begin() const noexcept { return first; }
            [[nodiscard]] It end() const noexcept { return last; }
            [[nodiscard]] bool empty() const noexcept { return first == last; }
        };

        /// @brief The elements of an array, in order. O(1) per step.
        /// @throws type_error if this is not an array.
        [[nodiscard]] range<element_iterator> elements() const;

        /// @brief The key/value members of an object, in order. O(1) per step.
        /// @throws type_error if this is not an object.
        [[nodiscard]] range<member_iterator> members() const;

        /// @brief Bounds-checked array element access. O(i).
        /// @throws type_error if this is not an array.
        /// @throws std::out_of_range if @p i is out of bounds.
        [[nodiscard]] cursor operator[](std::size_t i) const;

        /**
         * @fn find(std::string_view key)
         * @brief Look up an object key without throwing. O(n).
         * @return A cursor to the first matching value, or an invalid cursor (see @ref valid) if the
         *         key is absent or this node is not an object.
         */
        [[nodiscard]] cursor find(std::string_view key) const noexcept;

        /// @brief Test for the presence of an object key; `false` if this is not an object.
        [[nodiscard]] bool contains(std::string_view key) const noexcept { return find(key).valid(); }

        /**
         * @fn at(std::string_view key)
         * @brief Look up an object key, throwing if absent.
         * @throws type_error if this is not an object.
         * @throws std::out_of_range if @p key is absent.
         */
        [[nodiscard]] cursor at(std::string_view key) const
        {
            if (!is_object())
                throw type_error("value is not an object");
            const cursor c = find(key);
            if (!c.valid())
                throw std::out_of_range("object has no key '" + std::string(key) + "'");
            return c;
        }

        /// @copydoc at
        [[nodiscard]] cursor operator[](std::string_view key) const { return at(key); }

        /// @}

    private:
        [[nodiscard]] detail::tape::tag tag() const noexcept { return detail::tape::tag_of(tape_[idx_]); }
        [[nodiscard]] std::int64_t raw_int() const noexcept { return std::bit_cast<std::int64_t>(tape_[idx_ + 1]); }
        [[nodiscard]] double raw_double() const noexcept { return std::bit_cast<double>(tape_[idx_ + 1]); }
        [[nodiscard]] std::string_view raw_string() const noexcept
        {
            const std::size_t off = static_cast<std::size_t>(detail::tape::payload_of(tape_[idx_]));
            std::uint32_t len;
            std::memcpy(&len, strings_ + off, sizeof len);
            return std::string_view(strings_ + off + sizeof len, len);
        }

        const std::uint64_t *tape_ = nullptr; ///< Borrowed tape words.
        const char *strings_ = nullptr;       ///< Borrowed string arena.
        std::size_t idx_ = 0;                 ///< Index of the referenced node.
    };

    class cursor::element_iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = cursor;
        using difference_type = std::ptrdiff_t;
        using pointer = const cursor *;
        using reference = cursor;

        element_iterator() noexcept = default;
        explicit element_iterator(cursor c) noexcept : c_(c) {}
        [[nodiscard]] cursor operator*() const noexcept { return c_; }
        element_iterator &operator++() noexcept
        {
            c_ = c_.next_sibling();
            return *this;
        }
        element_iterator operator++(int) noexcept
        {
            auto t = *this;
            ++*this;
            return t;
        }
        [[nodiscard]] bool operator==(const element_iterator &o) const noexcept { return c_.idx_ == o.c_.idx_; }

    private:
        cursor c_;
    };

    struct cursor::member
    {
        std::string_view key; ///< The key text (a view into the arena).
        cursor value;         ///< The member's value.
    };

    class cursor::member_iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = member;
        using difference_type = std::ptrdiff_t;
        using pointer = const member *;
        using reference = member;

        member_iterator() noexcept = default;
        explicit member_iterator(cursor key) noexcept : key_(key) {}
        [[nodiscard]] member operator*() const noexcept { return member{key_.raw_string(), key_.next_sibling()}; }
        member_iterator &operator++() noexcept
        {
            key_ = key_.next_sibling().next_sibling();
            return *this;
        }
        member_iterator operator++(int) noexcept
        {
            auto t = *this;
            ++*this;
            return t;
        }
        [[nodiscard]] bool operator==(const member_iterator &o) const noexcept { return key_.idx_ == o.key_.idx_; }

    private:
        cursor key_;
    };

    inline cursor::range<cursor::element_iterator> cursor::elements() const
    {
        if (!is_array())
            throw type_error("value is not an array");
        return {element_iterator(first_child()), element_iterator(next_sibling())};
    }

    inline cursor::range<cursor::member_iterator> cursor::members() const
    {
        if (!is_object())
            throw type_error("value is not an object");
        return {member_iterator(first_child()), member_iterator(next_sibling())};
    }

    inline cursor cursor::operator[](std::size_t i) const
    {
        std::size_t k = 0;
        for (const cursor el : elements())
            if (k++ == i)
                return el;
        throw std::out_of_range("array index out of range");
    }

    inline cursor cursor::find(std::string_view key) const noexcept
    {
        if (!is_object())
            return cursor();
        const member_iterator last(next_sibling());
        for (member_iterator it(first_child()); it != last; ++it)
        {
            const member m = *it;
            if (m.key == key)
                return m.value;
        }
        return cursor();
    }

} // namespace catalyst::resource::json
