/**
 * @file value.cpp
 * @brief Implements the non-template members of @ref catalyst::resource::json::value, declared in
 * value.hpp.
 * @details The accessors that stay in the header are the ones parameterized on the deduced `this`,
 * which have to be templates so that a const value hands back a const reference and a mutable one
 * hands back a mutable reference from the same source. What is here is everything that does not
 * need that: the numeric widening, the object lookups, and the initializer-list constructor that
 * decides between an array and an object.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/json/value.hpp>

#include <catalyst/resource/json/error.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace catalyst::resource::json
{

    value::value(std::initializer_list<value> init)
    {
        bool object_like = init.size() > 0;
        for (const value &el : init)
        {
            if (!(el.is_array() && el.as_array().size() == 2 && el.as_array().front().is_string()))
            {
                object_like = false;
                break;
            }
        }
        if (object_like)
        {
            object obj;
            obj.reserve(init.size());
            for (const value &el : init)
            {
                const array &pair = el.as_array();
                obj.emplace_back(pair[0].as_string(), pair[1]);
            }
            data_ = std::move(obj);
        }
        else
        {
            data_ = array(init);
        }
    }

    double value::as_double() const
    {
        if (const auto *i = std::get_if<std::int64_t>(&data_))
            return static_cast<double>(*i);
        return expect<double>("a number");
    }

    std::optional<double> value::try_double() const noexcept
    {
        if (const auto *i = std::get_if<std::int64_t>(&data_))
            return static_cast<double>(*i);
        return maybe<double>();
    }

    std::optional<std::string_view> value::try_string() const noexcept
    {
        if (const auto *s = std::get_if<std::string>(&data_))
            return std::string_view(*s);
        return std::nullopt;
    }

    std::size_t value::size() const noexcept
    {
        if (const auto *a = std::get_if<array>(&data_))
            return a->size();
        if (const auto *o = std::get_if<object>(&data_))
            return o->size();
        return 0;
    }

    const value &value::at(std::string_view key) const
    {
        const object &obj = as_object();
        for (const auto &[k, v] : obj)
            if (k == key)
                return v;
        throw std::out_of_range("object has no key '" + std::string(key) + "'");
    }

    value &value::operator[](std::string_view key)
    {
        if (is_null())
            data_ = object{};
        object &obj = as_object();
        for (auto &[k, v] : obj)
            if (k == key)
                return v;
        obj.emplace_back(std::string(key), value{});
        return obj.back().second;
    }

} // namespace catalyst::resource::json
