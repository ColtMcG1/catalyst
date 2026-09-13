/**
 * @file document.cpp
 * @brief Implements the flat @ref catalyst::resource::json::document, the parser sink that fills it,
 * and @ref catalyst::resource::json::to_value, all declared in document.hpp.
 * @details A container is emitted with a placeholder header and backpatched when it closes, which is
 * what lets a cursor skip a whole subtree in constant time later. The sink's per-node callbacks are
 * templates on the parser's handle type and stay in the header; what is here is the buffer
 * management underneath them.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/json/document.hpp>

#include <catalyst/resource/json/tape.hpp>
#include <catalyst/resource/json/value.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace catalyst::resource::json
{

    cursor document::root() const noexcept
    {
        if (tape_.empty())
            return cursor();
        return cursor(tape_.data(), strings_.data(), 0);
    }

    value to_value(const cursor &c)
    {
        switch (c.kind())
        {
        case type::null:
            return value(nullptr);
        case type::boolean:
            return value(c.as_bool());
        case type::integer:
            return value(c.as_int());
        case type::floating:
            return value(c.as_double());
        case type::string:
            return value(std::string(c.as_string()));
        case type::array:
        {
            array arr;
            arr.reserve(c.size());
            for (const cursor el : c.elements())
                arr.push_back(to_value(el));
            return value(std::move(arr));
        }
        default:
        { // type::object
            object obj;
            obj.reserve(c.size());
            for (const auto [key, val] : c.members())
                obj.emplace_back(std::string(key), to_value(val));
            return value(std::move(obj));
        }
        }
    }

    namespace detail
    {
        tape_sink::tape_sink(std::size_t input_size)
        {
            strings_.reserve(input_size);
            tape_.reserve(input_size / 4 + 8);
        }

        document tape_sink::finish(root_handle &) noexcept
        {
            document doc;
            doc.tape_ = std::move(tape_);
            doc.strings_ = std::move(strings_);
            return doc;
        }

        tape_sink::container_handle tape_sink::open(tape::tag t)
        {
            const container_handle h{tape_.size()};
            emit(t);            // payload (skip index) backpatched on close
            tape_.push_back(0); // count backpatched on close
            return h;
        }

        void tape_sink::close(tape::tag t, container_handle h, std::uint64_t count)
        {
            tape_[h.header] = tape::make(t, tape_.size());
            tape_[h.header + 1] = count;
        }

    } // namespace detail
} // namespace catalyst::resource::json
