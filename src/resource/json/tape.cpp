/**
 * @file tape.cpp
 * @brief Implements the tape node helpers and the @ref catalyst::resource::json::cursor accessors
 * declared in tape.hpp.
 * @details What is here is the accessors that do real work per call: the typed reads that check the
 * tag and throw, and the lookups that scan a container. The navigation members stay in the header
 * because a traversal calls them once per node -- see the note on
 * @ref catalyst::resource::json::cursor.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/json/tape.hpp>

#include <catalyst/resource/json/error.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace catalyst::resource::json
{




    bool cursor::as_bool() const
    {
        if (tag() == detail::tape::t_true)
            return true;
        if (tag() == detail::tape::t_false)
            return false;
        throw type_error("value is not a boolean");
    }

    std::int64_t cursor::as_int() const
    {
        if (!is_integer())
            throw type_error("value is not an integer");
        return raw_int();
    }

    double cursor::as_double() const
    {
        if (is_integer())
            return static_cast<double>(raw_int());
        if (is_floating())
            return raw_double();
        throw type_error("value is not a number");
    }

    std::string_view cursor::as_string() const
    {
        if (!is_string())
            throw type_error("value is not a string");
        return raw_string();
    }

    std::optional<bool> cursor::try_bool() const noexcept
    {
        if (tag() == detail::tape::t_true)
            return true;
        if (tag() == detail::tape::t_false)
            return false;
        return std::nullopt;
    }

    std::optional<std::int64_t> cursor::try_int() const noexcept
    {
        if (is_integer())
            return raw_int();
        return std::nullopt;
    }

    std::optional<double> cursor::try_double() const noexcept
    {
        if (is_integer())
            return static_cast<double>(raw_int());
        if (is_floating())
            return raw_double();
        return std::nullopt;
    }

    std::optional<std::string_view> cursor::try_string() const noexcept
    {
        if (is_string())
            return raw_string();
        return std::nullopt;
    }



    cursor cursor::at(std::string_view key) const
    {
        if (!is_object())
            throw type_error("value is not an object");
        const cursor c = find(key);
        if (!c.valid())
            throw std::out_of_range("object has no key '" + std::string(key) + "'");
        return c;
    }



    cursor::element_iterator cursor::element_iterator::operator++(int) noexcept
    {
        auto t = *this;
        ++*this;
        return t;
    }


    cursor::member_iterator cursor::member_iterator::operator++(int) noexcept
    {
        auto t = *this;
        ++*this;
        return t;
    }



    cursor cursor::operator[](std::size_t i) const
    {
        std::size_t k = 0;
        for (const cursor el : elements())
            if (k++ == i)
                return el;
        throw std::out_of_range("array index out of range");
    }

    cursor cursor::find(std::string_view key) const noexcept
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
