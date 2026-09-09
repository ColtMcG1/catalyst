#pragma once

#include "detail/concepts.hpp"
#include "detail/config.hpp"
#include "detail/hash.hpp"
#include "detail/vector_iterator.hpp"

#include <concepts>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>

// -------------------------------------------------------------------------
// vector
// -------------------------------------------------------------------------
//
// The fixed-size vector type, its concept, and the operators that belong to
// it: the element-wise arithmetic and the std integration. The functions
// *on* vectors live elsewhere -- geometry.hpp for dot, cross, magnitude and
// friends, elementwise.hpp for the scalar functions applied per element.

namespace math
{

    // ---------------------------------------------------------------------
    // Concepts
    // ---------------------------------------------------------------------
    //
    // numeric, scalar_like and detail::bare live in detail/concepts.hpp,
    // which is shared with matrix.hpp.

    // Anything with a scalar_like value_type, a compile-time length, and
    // const element access. Satisfied by cv/ref-qualified vectors too, so it can
    // constrain forwarding references.
    template <typename V>
    concept vector_like =
        requires {
            typename detail::bare<V>::value_type;
            { detail::bare<V>::length } -> std::convertible_to<std::size_t>;
            requires(detail::bare<V>::length > 0);
        } &&
        requires(const detail::bare<V> &v, std::size_t i) {
            { v[i] } -> std::convertible_to<typename detail::bare<V>::value_type>;
        } &&
        scalar_like<typename detail::bare<V>::value_type>;

    // ---------------------------------------------------------------------
    // vector
    // ---------------------------------------------------------------------

    template <scalar_like T, std::size_t N>
        requires(N > 0)
    struct vector
    {
        static constexpr std::size_t length = N;

        using value_type = T;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using reference = T &;
        using const_reference = const T &;
        using pointer = T *;
        using const_pointer = const T *;

        using iterator = vector_iterator<T>;
        using const_iterator = vector_iterator<const T>;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        // The only data member. Keeps the type an aggregate, trivially
        // copyable and standard-layout, so vector<float, 3>{1, 2, 3} works and
        // sizeof(vector<T, N>) == N * sizeof(T).
        value_type values[length]{};

        // ---- factories ---------------------------------------------------

        static constexpr vector zero() noexcept { return vector{}; }

        static constexpr vector filled(T value) noexcept
        {
            vector result{};
            for (auto &e : result.values)
                e = value;
            return result;
        }

        template <std::size_t I>
            requires(I < N)
        static constexpr vector axis() noexcept
        {
            vector result{};
            result.values[I] = T{1};
            return result;
        }

        // ---- size / raw access -------------------------------------------

        static constexpr size_type size() noexcept { return N; }

        constexpr auto data(this auto &&self) noexcept { return &self.values[0]; }

        // ---- iterators ---------------------------------------------------

        constexpr iterator begin() noexcept { return iterator(values); }
        constexpr iterator end() noexcept { return iterator(values + N); }
        constexpr const_iterator begin() const noexcept { return const_iterator(values); }
        constexpr const_iterator end() const noexcept { return const_iterator(values + N); }
        constexpr const_iterator cbegin() const noexcept { return begin(); }
        constexpr const_iterator cend() const noexcept { return end(); }

        constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
        constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
        constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
        constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
        constexpr const_reverse_iterator crbegin() const noexcept { return rbegin(); }
        constexpr const_reverse_iterator crend() const noexcept { return rend(); }

        // ---- element access ----------------------------------------------
        //
        // std::forward_like keeps the value category of the vector: an
        // lvalue vector yields T&, a const one const T&, a temporary T&&.

        constexpr decltype(auto) operator[](this auto &&self, size_type index) noexcept
        {
            MATH_ASSERT(index < N, "math::vector::operator[]: index out of range");
            return std::forward_like<decltype(self)>(self.values[index]);
        }

        constexpr decltype(auto) at(this auto &&self, size_type index)
        {
            if (index >= N)
                throw std::out_of_range("math::vector::at: index out of range");
            return std::forward_like<decltype(self)>(self.values[index]);
        }

        template <std::size_t I>
            requires(I < N)
        constexpr decltype(auto) get(this auto &&self) noexcept
        {
            return std::forward_like<decltype(self)>(self.values[I]);
        }

        constexpr decltype(auto) x(this auto &&self) noexcept
            requires(N > 0)
        {
            return std::forward_like<decltype(self)>(self.values[0]);
        }
        constexpr decltype(auto) r(this auto &&self) noexcept
            requires(N > 0)
        {
            return std::forward_like<decltype(self)>(self.values[0]);
        }

        constexpr decltype(auto) y(this auto &&self) noexcept
            requires(N > 1)
        {
            return std::forward_like<decltype(self)>(self.values[1]);
        }
        constexpr decltype(auto) g(this auto &&self) noexcept
            requires(N > 1)
        {
            return std::forward_like<decltype(self)>(self.values[1]);
        }

        constexpr decltype(auto) z(this auto &&self) noexcept
            requires(N > 2)
        {
            return std::forward_like<decltype(self)>(self.values[2]);
        }
        constexpr decltype(auto) b(this auto &&self) noexcept
            requires(N > 2)
        {
            return std::forward_like<decltype(self)>(self.values[2]);
        }

        constexpr decltype(auto) w(this auto &&self) noexcept
            requires(N > 3)
        {
            return std::forward_like<decltype(self)>(self.values[3]);
        }
        constexpr decltype(auto) a(this auto &&self) noexcept
            requires(N > 3)
        {
            return std::forward_like<decltype(self)>(self.values[3]);
        }

        // ---- swizzles (return copies) ------------------------------------
        //
        // swizzle<I...>() builds a vector from any selection of components,
        // repeats allowed: v.swizzle<2, 1, 0>() reverses a vec3, and
        // v.swizzle<0, 0, 0, 0>() splats x. The named ones are the common
        // cases.

        template <std::size_t... I>
            requires(sizeof...(I) > 0 && ((I < N) && ...))
        constexpr vector<T, sizeof...(I)> swizzle() const noexcept
        {
            return {values[I]...};
        }

        constexpr vector<T, 2> xy() const noexcept
            requires(N >= 2)
        {
            return {values[0], values[1]};
        }

        constexpr vector<T, 2> yx() const noexcept
            requires(N >= 2)
        {
            return {values[1], values[0]};
        }

        constexpr vector<T, 2> xz() const noexcept
            requires(N >= 3)
        {
            return {values[0], values[2]};
        }

        constexpr vector<T, 2> yz() const noexcept
            requires(N >= 3)
        {
            return {values[1], values[2]};
        }

        constexpr vector<T, 2> zw() const noexcept
            requires(N >= 4)
        {
            return {values[2], values[3]};
        }

        constexpr vector<T, 3> xyz() const noexcept
            requires(N >= 3)
        {
            return {values[0], values[1], values[2]};
        }

        constexpr vector<T, 3> rgb() const noexcept
            requires(N >= 3)
        {
            return {values[0], values[1], values[2]};
        }

        // ---- conversion --------------------------------------------------

        template <scalar_like U>
        constexpr vector<U, N> cast() const noexcept
        {
            vector<U, N> result{};
            for (size_type i = 0; i < N; ++i)
                result.values[i] = static_cast<U>(values[i]);
            return result;
        }

        // ---- comparison --------------------------------------------------

        constexpr bool operator==(const vector &) const noexcept = default;

        // ---- compound assignment -----------------------------------------
        //
        // Mixed element types are allowed; the result is narrowed back to T
        // exactly as `int i; i += 2.5;` would.

        template <vector_like V>
            requires(V::length == N)
        constexpr vector &operator+=(const V &other) noexcept
        {
            for (size_type i = 0; i < N; ++i)
                values[i] = static_cast<T>(values[i] + other[i]);
            return *this;
        }

        template <vector_like V>
            requires(V::length == N)
        constexpr vector &operator-=(const V &other) noexcept
        {
            for (size_type i = 0; i < N; ++i)
                values[i] = static_cast<T>(values[i] - other[i]);
            return *this;
        }

        template <vector_like V>
            requires(V::length == N)
        constexpr vector &operator*=(const V &other) noexcept
        {
            for (size_type i = 0; i < N; ++i)
                values[i] = static_cast<T>(values[i] * other[i]);
            return *this;
        }

        template <vector_like V>
            requires(V::length == N)
        constexpr vector &operator/=(const V &other) noexcept
        {
            for (size_type i = 0; i < N; ++i)
                values[i] = static_cast<T>(values[i] / other[i]);
            return *this;
        }

        constexpr vector &operator+=(scalar_like auto s) noexcept
        {
            for (auto &e : values)
                e = static_cast<T>(e + s);
            return *this;
        }

        constexpr vector &operator-=(scalar_like auto s) noexcept
        {
            for (auto &e : values)
                e = static_cast<T>(e - s);
            return *this;
        }

        constexpr vector &operator*=(scalar_like auto s) noexcept
        {
            for (auto &e : values)
                e = static_cast<T>(e * s);
            return *this;
        }

        constexpr vector &operator/=(scalar_like auto s) noexcept
        {
            for (auto &e : values)
                e = static_cast<T>(e / s);
            return *this;
        }
    };

    // vector{1, 2, 3} -> vector<int, 3>; vector{1, 2.0} -> vector<double, 2>.
    template <scalar_like T, scalar_like... U>
    vector(T, U...) -> vector<std::common_type_t<T, U...>, 1 + sizeof...(U)>;

    // ---- static assertions for vector ------------------------------------
    // Pure sanity checks for vector

    static_assert(sizeof(vector<float, 3>) == 3 * sizeof(float));
    static_assert(std::is_trivially_copyable_v<vector<float, 3>>);
    static_assert(std::is_standard_layout_v<vector<float, 3>>);

    // ---- aliases ---------------------------------------------------------

    template <scalar_like T>
    using vec2 = vector<T, 2>;
    template <scalar_like T>
    using vec3 = vector<T, 3>;
    template <scalar_like T>
    using vec4 = vector<T, 4>;

    using vec2f = vec2<float>;
    using vec3f = vec3<float>;
    using vec4f = vec4<float>;
    using vec2d = vec2<double>;
    using vec3d = vec3<double>;
    using vec4d = vec4<double>;
    using vec2i = vec2<int>;
    using vec3i = vec3<int>;
    using vec4i = vec4<int>;

    // ---------------------------------------------------------------------
    // Element-wise machinery
    // ---------------------------------------------------------------------

    namespace detail
    {
        // Teaches common_element_t (detail/concepts.hpp) about vectors, so
        // vector<int> op vector<float> -> float, and vector op scalar works.
        template <vector_like V>
        struct element<V>
        {
            using type = typename bare<V>::value_type;
        };

        // Builds vector<T, N>{f(0), f(1), ..., f(N-1)} in one expression.
        // The pack expansion is the unrolled loop, and it is constexpr.
        template <scalar_like T, std::size_t N>
        constexpr vector<T, N> generate(auto &&f)
        {
            return [&]<std::size_t... I>(std::index_sequence<I...>)
            {
                return vector<T, N>{static_cast<T>(f(I))...};
            }(std::make_index_sequence<N>{});
        }

        template <vector_like L, vector_like R>
        constexpr auto zip(const L &lhs, const R &rhs, auto op)
        {
            return generate<common_element_t<L, R>, L::length>([&](std::size_t i)
                                                               { return op(lhs[i], rhs[i]); });
        }

        template <vector_like V, scalar_like S>
        constexpr auto zip(const V &v, S s, auto op)
        {
            return generate<common_element_t<V, S>, V::length>([&](std::size_t i)
                                                               { return op(v[i], s); });
        }

        template <typename S, vector_like V>
            requires scalar_like<S>
        constexpr auto zip(S s, const V &v, auto op)
        {
            return generate<common_element_t<S, V>, V::length>([&](std::size_t i)
                                                               { return op(s, v[i]); });
        }

        template <vector_like V>
        constexpr auto map(const V &v, auto op)
        {
            return generate<typename element<V>::type, V::length>([&](std::size_t i)
                                                                  { return op(v[i]); });
        }
    } // namespace detail

    // ---------------------------------------------------------------------
    // Arithmetic operators
    // ---------------------------------------------------------------------
    //
    // The scalar-first overloads spell their constraints as
    // `template <typename S, vector_like V> requires scalar_like<S>` rather
    // than `template <scalar_like S, vector_like V>`, and the order matters:
    // constraints are checked in declaration order, and scalar_like<T> for
    // an opted-in element type T is itself defined by whether `t + t` is
    // valid. Resolving that expression considers these operator templates
    // with S = V = T, and if scalar_like<S> were checked first, the check
    // would recurse into itself, which is ill-formed (MSVC diagnoses it).
    // Checking vector_like<V> first rejects the candidate before that.

    template <vector_like V>
    constexpr auto operator+(const V &v)
    {
        return detail::map(v, [](auto e)
                           { return +e; });
    }

    template <vector_like V>
    constexpr auto operator-(const V &v)
    {
        return detail::map(v, [](auto e)
                           { return -e; });
    }

    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto operator+(const L &lhs, const R &rhs)
    {
        return detail::zip(lhs, rhs, std::plus<>{});
    }
    template <vector_like V, scalar_like S>
    constexpr auto operator+(const V &v, S s)
    {
        return detail::zip(v, s, std::plus<>{});
    }
    template <typename S, vector_like V>
        requires scalar_like<S>
    constexpr auto operator+(S s, const V &v)
    {
        return detail::zip(s, v, std::plus<>{});
    }

    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto operator-(const L &lhs, const R &rhs)
    {
        return detail::zip(lhs, rhs, std::minus<>{});
    }
    template <vector_like V, scalar_like S>
    constexpr auto operator-(const V &v, S s)
    {
        return detail::zip(v, s, std::minus<>{});
    }
    template <typename S, vector_like V>
        requires scalar_like<S>
    constexpr auto operator-(S s, const V &v)
    {
        return detail::zip(s, v, std::minus<>{});
    }

    // Element-wise (Hadamard) product. Use dot() for the inner product.
    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto operator*(const L &lhs, const R &rhs)
    {
        return detail::zip(lhs, rhs, std::multiplies<>{});
    }
    template <vector_like V, scalar_like S>
    constexpr auto operator*(const V &v, S s)
    {
        return detail::zip(v, s, std::multiplies<>{});
    }
    template <typename S, vector_like V>
        requires scalar_like<S>
    constexpr auto operator*(S s, const V &v)
    {
        return detail::zip(s, v, std::multiplies<>{});
    }

    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto operator/(const L &lhs, const R &rhs)
    {
        return detail::zip(lhs, rhs, std::divides<>{});
    }
    template <vector_like V, scalar_like S>
    constexpr auto operator/(const V &v, S s)
    {
        return detail::zip(v, s, std::divides<>{});
    }
    template <typename S, vector_like V>
        requires scalar_like<S>
    constexpr auto operator/(S s, const V &v)
    {
        return detail::zip(s, v, std::divides<>{});
    }

} // namespace math

// -------------------------------------------------------------------------
// std integration: structured bindings, hashing, std::format
// -------------------------------------------------------------------------

namespace std
{
    template <typename T, std::size_t N>
    struct tuple_size<math::vector<T, N>> : std::integral_constant<std::size_t, N>
    {
    };

    template <std::size_t I, typename T, std::size_t N>
    struct tuple_element<I, math::vector<T, N>>
    {
        using type = T;
    };

    template <typename T, std::size_t N>
    struct hash<math::vector<T, N>>
    {
        std::size_t operator()(const math::vector<T, N> &v) const noexcept
        {
            std::size_t seed = 0;
            for (const auto &e : v)
                math::detail::hash_combine(seed, e);
            return seed;
        }
    };

    // Formats as "(x, y, z)". The format spec applies to every element, so
    // std::format("{:.2f}", v) works.
    template <typename T, std::size_t N, typename CharT>
    struct formatter<math::vector<T, N>, CharT> : formatter<T, CharT>
    {
        template <typename FormatContext>
        auto format(const math::vector<T, N> &v, FormatContext &ctx) const
        {
            auto out = ctx.out();
            *out++ = CharT('(');
            for (std::size_t i = 0; i < N; ++i)
            {
                if (i != 0)
                {
                    *out++ = CharT(',');
                    *out++ = CharT(' ');
                }
                ctx.advance_to(out);
                out = formatter<T, CharT>::format(v[i], ctx);
            }
            *out++ = CharT(')');
            return out;
        }
    };
} // namespace std

