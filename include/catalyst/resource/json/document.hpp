/**
 * @file document.hpp
 * @brief The owning @ref document that holds a parsed JSON tree in tape form, and the parser sink that fills it.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "tape.hpp"

namespace catalyst::resource::json
{
    namespace detail
    {
        class tape_sink; // fills a document's buffers; befriended below.
    }

    /**
     * @class document
     * @brief An owning, flat ("tape") representation of a parsed JSON document.
     *
     * Produced by @ref parse_document. Holds the entire tree in two buffers with no per-node
     * allocation. Navigate it from @ref root(); the returned @ref cursor and any cursors derived from
     * it borrow from this document and must not outlive it. Convert a subtree to the owning
     * representation with @ref to_value when it needs to be kept or edited.
     */
    class document
    {
    public:
        document() = default;

        /**
         * @fn root()
         * @brief A cursor to the document's root value.
         * @return The root cursor, or an invalid cursor if the document is empty (only a
         *         default-constructed document is empty; @ref parse_document never yields one).
         */
        [[nodiscard]] cursor root() const noexcept
        {
            if (tape_.empty())
                return cursor();
            return cursor(tape_.data(), strings_.data(), 0);
        }

        /// @return `true` if the document holds no parsed value.
        [[nodiscard]] bool empty() const noexcept { return tape_.empty(); }

    private:
        friend class detail::tape_sink;
        std::vector<std::uint64_t> tape_; ///< Flat node stream; see @ref detail::tape.
        std::string strings_;             ///< Contiguous string arena.
    };

    namespace detail
    {
        /**
         * @class tape_sink
         * @brief Parser sink that emits a @ref document's tape and string arena.
         *
         * Containers are emitted with a placeholder header, then backpatched with their child count
         * and skip index when they close; the handle is just the header's tape index. Strings are
         * decoded straight into the arena behind a 4-byte length prefix. See @ref basic_parser for
         * the sink protocol.
         */
        class tape_sink
        {
        public:
            /**
             * @param input_size Size of the text about to be parsed; used to reserve both buffers
             *        once. Decoded strings never exceed the input, and one tape word per ~4 input
             *        bytes is a comfortable upper estimate, so neither buffer grows again mid-parse.
             *        (Measured: this alone nearly doubled tape parse throughput on multi-MB inputs;
             *        growing by doubling copies the whole buffer at every step.)
             */
            explicit tape_sink(std::size_t input_size)
            {
                strings_.reserve(input_size);
                tape_.reserve(input_size / 4 + 8);
            }

            struct root_handle
            {
            };
            struct container_handle
            {
                std::size_t header;
            };
            using array_handle = container_handle;
            using object_handle = container_handle;

            [[nodiscard]] root_handle root() const noexcept { return {}; }
            [[nodiscard]] document finish(root_handle &) noexcept
            {
                document doc;
                doc.tape_ = std::move(tape_);
                doc.strings_ = std::move(strings_);
                return doc;
            }

            template <class H>
            void on_null(H &)
            {
                emit(tape::t_null);
            }
            template <class H>
            void on_bool(H &, bool b)
            {
                emit(b ? tape::t_true : tape::t_false);
            }
            template <class H>
            void on_int(H &, std::int64_t i)
            {
                emit(tape::t_int);
                tape_.push_back(std::bit_cast<std::uint64_t>(i));
            }
            template <class H>
            void on_double(H &, double d)
            {
                emit(tape::t_double);
                tape_.push_back(std::bit_cast<std::uint64_t>(d));
            }

            template <class H>
            std::string &begin_string(H &)
            {
                string_off_ = strings_.size();
                strings_.append(sizeof(std::uint32_t), '\0'); // length placeholder
                return strings_;
            }
            template <class H>
            void end_string(H &)
            {
                const auto len = static_cast<std::uint32_t>(strings_.size() - string_off_ - sizeof(std::uint32_t));
                std::memcpy(strings_.data() + string_off_, &len, sizeof len);
                emit(tape::t_string, static_cast<std::uint64_t>(string_off_));
            }
            std::string &begin_key(object_handle &h) { return begin_string(h); }
            void end_key(object_handle &h) { end_string(h); }

            [[nodiscard]] array_handle begin_array() { return open(tape::t_array); }
            template <class H>
            void end_array(H &, array_handle &a, std::uint64_t count)
            {
                close(tape::t_array, a, count);
            }
            [[nodiscard]] object_handle begin_object() { return open(tape::t_object); }
            template <class H>
            void end_object(H &, object_handle &o, std::uint64_t count)
            {
                close(tape::t_object, o, count);
            }

        private:
            void emit(tape::tag t, std::uint64_t payload = 0) { tape_.push_back(tape::make(t, payload)); }

            container_handle open(tape::tag t)
            {
                const container_handle h{tape_.size()};
                emit(t);            // payload (skip index) backpatched on close
                tape_.push_back(0); // count backpatched on close
                return h;
            }
            void close(tape::tag t, container_handle h, std::uint64_t count)
            {
                tape_[h.header] = tape::make(t, tape_.size());
                tape_[h.header + 1] = count;
            }

            std::vector<std::uint64_t> tape_; ///< Emitted tape words.
            std::string strings_;             ///< Emitted string arena.
            std::size_t string_off_ = 0;      ///< Arena offset of the string being decoded.
        };

    } // namespace detail

    /**
     * @fn to_value(const cursor &c)
     * @brief Materialize a tape node into an owning @ref value tree.
     *
     * The bridge from the fast representation to the ergonomic one: copies the subtree under @p c into
     * a standalone @ref value (allocating as the `value` API normally does).
     * @param c A valid cursor.
     */
    [[nodiscard]] inline value to_value(const cursor &c)
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

} // namespace catalyst::resource::json
