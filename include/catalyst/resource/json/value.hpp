/**
 * @file value.hpp
 * @brief The owning JSON value tree for the catalyst::resource::json module: the @ref value class and the
 * @ref array / @ref object containers it is built from, plus the @ref type tag shared with the tape DOM.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "error.hpp"

namespace catalyst::resource::json
{
    class value;

    /**
     * @typedef array
     * @brief A JSON array: an ordered sequence of values.
     */
    using array = std::vector<value>;

    /**
     * @typedef object
     * @brief A JSON object that preserves insertion order.
     *
     * Unlike `std::map`, insertion order is retained, which matches what most callers expect when
     * round-tripping documents. Duplicate keys are kept as parsed; lookups return the first match.
     */
    using object = std::vector<std::pair<std::string, value>>;

    /**
     * @enum type
     * @brief The dynamic type tag of a @ref value or @ref cursor.
     */
    enum class type
    {
        null,     ///< JSON `null`.
        boolean,  ///< JSON `true` / `false`.
        integer,  ///< Signed 64-bit integral number.
        floating, ///< IEEE-754 double.
        string,   ///< JSON string.
        array,    ///< JSON array.
        object,   ///< JSON object.
    };

    /**
     * @class value
     * @brief A first-class JSON value holding any of the seven JSON types.
     *
     * The active alternative is reported by @ref kind() and can be queried with the `is_*` predicates.
     * Two families of accessors extract the payload:
     *
     *  - `as_*` are checked accesses that throw @ref type_error on a mismatch. Use them when the shape
     *    of the document is known and a mismatch is a bug.
     *  - `try_*` return `std::optional` (scalars) or a nullable pointer (containers) and never throw.
     *    Use them when the shape is data, not a contract: optional config keys, loosely typed fields.
     */
    class value
    {
    private:
        /// @brief The underlying storage; alternatives are ordered exactly like @ref type.
        using storage = std::variant<std::monostate, bool, std::int64_t, double, std::string, array, object>;

        /// @brief `T &` or `const T &`, matching the constness of the deduced `this` type @p Self.
        template <class Self, class T>
        using ref_like = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const T &, T &>;

        /// @brief `T *` or `const T *`, matching the constness of the deduced `this` type @p Self.
        template <class Self, class T>
        using ptr_like = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const T *, T *>;

        /**
         * @fn expect
         * @brief Extract the active alternative or throw @ref type_error.
         * @tparam T The expected alternative.
         * @param what A description of the expected type for the error message.
         */
        template <class T, class Self>
        [[nodiscard]] ref_like<Self, T> expect(this Self &&self, std::string_view what)
        {
            if (auto *p = std::get_if<T>(&self.data_))
                return *p;
            throw type_error("value is not " + std::string(what));
        }

        /**
         * @fn maybe
         * @brief Copy out the active alternative, or `std::nullopt` if it is not @p T.
         */
        template <class T>
        [[nodiscard]] std::optional<T> maybe() const noexcept
        {
            if (const auto *p = std::get_if<T>(&data_))
                return *p;
            return std::nullopt;
        }

        template <type K, class T>
        static constexpr bool alt_is =
            std::is_same_v<std::variant_alternative_t<static_cast<std::size_t>(K), storage>, T>;

        static_assert(alt_is<type::null, std::monostate> && alt_is<type::boolean, bool> &&
                          alt_is<type::integer, std::int64_t> && alt_is<type::floating, double> &&
                          alt_is<type::string, std::string> && alt_is<type::array, array> &&
                          alt_is<type::object, object>,
                      "variant alternatives must be ordered exactly like enum type");


    public:
        /// @name Construction
        /// @{

        value() noexcept = default;       ///< Construct a null value.
        value(std::nullptr_t) noexcept {} ///< Construct a null value.
        value(bool b) : data_(b) {}       ///< Construct a boolean value.

        /**
         * @fn value(I i)
         * @brief Construct an integer value from any integral type (except `bool`).
         * @param i The integer value to store.
         */
        template <class I>
            requires std::is_integral_v<I> && (!std::is_same_v<I, bool>)
        value(I i) : data_(static_cast<std::int64_t>(i))
        {
        }

        /**
         * @fn value(F f)
         * @brief Construct a floating-point value (stored as `double`).
         * @param f The floating-point value to store.
         */
        template <class F>
            requires std::is_floating_point_v<F>
        value(F f) : data_(static_cast<double>(f))
        {
        }

        /**
         * @fn value(const char *s)
         * @brief Construct a string value from a C-style string.
         * @param s The C-style string to copy.
         */
        value(const char *s) : data_(std::string(s)) {}

        /**
         * @fn value(std::string_view s)
         * @brief Construct a string value from a string view.
         * @param s The string view to copy.
         */
        value(std::string_view s) : data_(std::string(s)) {}

        /**
         * @fn value(std::string s)
         * @brief Construct a string value from a string, taking ownership.
         * @param s The string to take ownership of.
         */
        value(std::string s) : data_(std::move(s)) {}

        /**
         * @fn value(array a)
         * @brief Construct an array value, taking ownership.
         * @param a The array to take ownership of.
         */
        value(array a) : data_(std::move(a)) {}

        /**
         * @fn value(object o)
         * @brief Construct an object value, taking ownership.
         * @param o The object to take ownership of.
         */
        value(object o) : data_(std::move(o)) {}

        /**
         * @fn value(std::initializer_list<value> init)
         * @brief Construct a value from a braced initializer list.
         * @param init The braced initializer list to construct the value from.
         * @note If every element of the list is a two-element array whose first element is a string, the
         * value is constructed as an object; otherwise it is constructed as an array. An empty list
         * produces an empty array. As with any initializer-list constructor, `value{true}` selects this
         * overload and yields the one-element array `[true]`; write `value v = true;` or `value(true)`
         * for a scalar.
         */
        value(std::initializer_list<value> init);

        /// @}

        /// @name Introspection
        /// @{

        /**
         * @fn kind()
         * @brief The dynamic type tag of this value.
         * @return The type of the value.
         */
        [[nodiscard]] type kind() const noexcept { return static_cast<type>(data_.index()); }

        [[nodiscard]] bool is_null() const noexcept { return kind() == type::null; }         ///< `true` for JSON `null`.
        [[nodiscard]] bool is_boolean() const noexcept { return kind() == type::boolean; }   ///< `true` for `true`/`false`.
        [[nodiscard]] bool is_integer() const noexcept { return kind() == type::integer; }   ///< `true` for an integral number.
        [[nodiscard]] bool is_floating() const noexcept { return kind() == type::floating; } ///< `true` for a floating number.
        [[nodiscard]] bool is_number() const noexcept { return is_integer() || is_floating(); } ///< `true` for any number.
        [[nodiscard]] bool is_string() const noexcept { return kind() == type::string; }     ///< `true` for a string.
        [[nodiscard]] bool is_array() const noexcept { return kind() == type::array; }       ///< `true` for an array.
        [[nodiscard]] bool is_object() const noexcept { return kind() == type::object; }     ///< `true` for an object.

        /// @}

        /// @name Checked access
        /// Each accessor throws @ref type_error on a type mismatch.
        /// @{

        /**
         * @fn as_bool()
         * @brief Access the value as a boolean.
         * @throws type_error if the value is not a boolean.
         */
        [[nodiscard]] bool as_bool() const { return expect<bool>("a boolean"); }

        /**
         * @fn as_int()
         * @brief Access the value as an integer.
         * @throws type_error if the value is not an integer.
         */
        [[nodiscard]] std::int64_t as_int() const { return expect<std::int64_t>("an integer"); }

        /**
         * @fn as_double()
         * @brief Access the numeric payload as a `double`; integers are widened.
         * @throws type_error if the value is not a number.
         */
        [[nodiscard]] double as_double() const;

        /**
         * @fn as_string()
         * @brief Access the value as a string.
         * @throws type_error if the value is not a string.
         */
        [[nodiscard]] const std::string &as_string() const { return expect<std::string>("a string"); }

        /**
         * @fn as_array()
         * @brief Access the array payload (const or mutable, matching `this`).
         * @throws type_error if the value is not an array.
         */
        template <class Self>
        [[nodiscard]] ref_like<Self, array> as_array(this Self &&self)
        {
            return self.template expect<array>("an array");
        }

        /**
         * @fn as_object()
         * @brief Access the object payload (const or mutable, matching `this`).
         * @throws type_error if the value is not an object.
         */
        template <class Self>
        [[nodiscard]] ref_like<Self, object> as_object(this Self &&self)
        {
            return self.template expect<object>("an object");
        }

        /// @}

        /// @name Unchecked access
        /// Each accessor returns an empty optional / null pointer on a type mismatch and never throws.
        /// @{

        /// @brief The boolean payload, or `std::nullopt` if this is not a boolean.
        [[nodiscard]] std::optional<bool> try_bool() const noexcept { return maybe<bool>(); }

        /// @brief The integer payload, or `std::nullopt` if this is not an integer.
        [[nodiscard]] std::optional<std::int64_t> try_int() const noexcept { return maybe<std::int64_t>(); }

        /// @brief The numeric payload as a `double` (integers widened), or `std::nullopt` if not a number.
        [[nodiscard]] std::optional<double> try_double() const noexcept;

        /// @brief A view of the string payload, or `std::nullopt` if this is not a string.
        /// @note The view borrows from this value and is invalidated by any mutation of it.
        [[nodiscard]] std::optional<std::string_view> try_string() const noexcept;

        /// @brief A pointer to the array payload (const or mutable, matching `this`), or `nullptr`.
        template <class Self>
        [[nodiscard]] ptr_like<Self, array> try_array(this Self &self) noexcept
        {
            return std::get_if<array>(&self.data_);
        }

        /// @brief A pointer to the object payload (const or mutable, matching `this`), or `nullptr`.
        template <class Self>
        [[nodiscard]] ptr_like<Self, object> try_object(this Self &self) noexcept
        {
            return std::get_if<object>(&self.data_);
        }

        /// @}

        /**
         * @fn size()
         * @brief The number of elements in the array or object.
         * @return The element count, or 0 if the value is neither an array nor an object.
         */
        [[nodiscard]] std::size_t size() const noexcept;

        /// @name Array access
        /// @{

        /**
         * @fn operator[](std::size_t i)
         * @brief Bounds-checked element access by index.
         * @param i The index of the element to access.
         * @throws type_error if this is not an array.
         * @throws std::out_of_range if @p i is out of bounds.
         */
        template <class Self>
        [[nodiscard]] ref_like<Self, value> operator[](this Self &&self, std::size_t i)
        {
            return self.as_array().at(i);
        }

        /// @}

        /// @name Object access
        /// @{

        /**
         * @fn find(std::string_view key)
         * @brief Look up a key without throwing.
         * @param key The key to look up.
         * @return A pointer to the first matching member's value (const or mutable, matching `this`),
         *         or `nullptr` if the key is absent or this is not an object.
         */
        template <class Self>
        [[nodiscard]] ptr_like<Self, value> find(this Self &self, std::string_view key) noexcept
        {
            using pointer = ptr_like<Self, value>;
            if (auto *obj = std::get_if<object>(&self.data_))
                for (auto &[k, v] : *obj)
                    if (k == key)
                        return static_cast<pointer>(&v);
            return static_cast<pointer>(nullptr);
        }

        /**
         * @fn contains(std::string_view key)
         * @brief Test for the presence of a key.
         * @param key The key to test for.
         * @return `true` if this is an object and the key is present, `false` otherwise.
         */
        [[nodiscard]] bool contains(std::string_view key) const noexcept { return find(key) != nullptr; }

        /**
         * @fn at(std::string_view key)
         * @brief Look up a key, throwing if it is absent.
         * @param key The key to look up.
         * @return The value associated with the key.
         * @throws type_error if this is not an object.
         * @throws std::out_of_range if @p key is absent.
         */
        [[nodiscard]] const value &at(std::string_view key) const;

        /// @copydoc at(std::string_view key)
        [[nodiscard]] const value &operator[](std::string_view key) const { return at(key); }

        /**
         * @fn operator[](std::string_view key)
         * @brief Mutable object lookup with auto-vivification.
         *
         * Inserts a null value when the key is absent, turning a null value into an empty object first.
         * @throws type_error if this is neither null nor an object.
         */
        value &operator[](std::string_view key);

        /// @}

    private:
        storage data_;
    };

} // namespace catalyst::resource::json
