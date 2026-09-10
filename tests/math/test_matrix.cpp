#include <catalyst/math/linear_algebra.hpp>
#include <catalyst/math/matrix.hpp>
#include <catalyst/math/vector.hpp>

#include "test_common.hpp"

namespace
{

using catalyst::tests::nearly_equal;

void test_row_major_is_the_default_layout()
{
    using math::matrix;

    // Brace init fills the stored lines in order, and for the default
    // row-major order a stored line is a row: row0 = [1,2,3], row1 = [4,5,6].
    const matrix<float, 2, 3> m{1, 2, 3, 4, 5, 6};

    static_assert(m.order == math::matrix_order::row_major);
    static_assert(m.rows == 2 && m.cols == 3);

    CT_REQUIRE(nearly_equal(m(0, 0), 1.0f));
    CT_REQUIRE(nearly_equal(m(0, 1), 2.0f));
    CT_REQUIRE(nearly_equal(m(0, 2), 3.0f));
    CT_REQUIRE(nearly_equal(m(1, 0), 4.0f));
    CT_REQUIRE(nearly_equal(m(1, 1), 5.0f));
    CT_REQUIRE(nearly_equal(m(1, 2), 6.0f));

    // operator[] hands out the stored line, which is the row here.
    const auto r0 = m[0];
    const auto r1 = m[1];
    CT_REQUIRE(nearly_equal(r0[0], 1.0f));
    CT_REQUIRE(nearly_equal(r0[2], 3.0f));
    CT_REQUIRE(nearly_equal(r1[0], 4.0f));
    CT_REQUIRE(nearly_equal(r1[2], 6.0f));
}

void test_element_access_is_independent_of_storage_order()
{
    using math::matrix;
    using math::matrix_order;

    // The same logical matrix in both orders. (row, col) indexing must agree,
    // which is the whole point of not exposing the layout through it.
    const matrix<float, 2, 3> row_major = matrix<float, 2, 3>::from_rows(
        math::vector<float, 3>{1.0f, 2.0f, 3.0f},
        math::vector<float, 3>{4.0f, 5.0f, 6.0f});

    const matrix<float, 2, 3, matrix_order::column_major> col_major =
        matrix<float, 2, 3, matrix_order::column_major>::from_rows(
            math::vector<float, 3>{1.0f, 2.0f, 3.0f},
            math::vector<float, 3>{4.0f, 5.0f, 6.0f});

    static_assert(col_major.order == matrix_order::column_major);

    for (std::size_t r = 0; r < 2; ++r)
        for (std::size_t c = 0; c < 3; ++c)
            CT_REQUIRE(nearly_equal(row_major(r, c), col_major(r, c)));

    // A column-major matrix stores columns, so its stored lines are 2 long
    // while the row-major one's are 3.
    static_assert(decltype(row_major)::line_length == 3);
    static_assert(decltype(col_major)::line_length == 2);
}

void test_from_rows_and_from_columns_are_transposes()
{
    using math::matrix;
    using math::vector;

    const auto by_rows = matrix<float, 2, 3>::from_rows(
        vector<float, 3>{1.0f, 2.0f, 3.0f},
        vector<float, 3>{4.0f, 5.0f, 6.0f});

    const auto by_cols = matrix<float, 3, 2>::from_columns(
        vector<float, 3>{1.0f, 2.0f, 3.0f},
        vector<float, 3>{4.0f, 5.0f, 6.0f});

    const auto transposed = math::transpose(by_cols);

    for (std::size_t r = 0; r < 2; ++r)
        for (std::size_t c = 0; c < 3; ++c)
            CT_REQUIRE(nearly_equal(by_rows(r, c), transposed(r, c)));
}

void test_mul_vec_and_mul_mat()
{
    using math::matrix;
    using math::vector;

    // row0 = [1,2,3], row1 = [4,5,6]
    const matrix<float, 2, 3> a{1, 2, 3, 4, 5, 6};
    const vector<float, 3> v{10.0f, 100.0f, 1000.0f};

    // Column vector on the right: r[i] = dot(row i, v).
    const auto r = a * v;
    CT_REQUIRE(nearly_equal(r[0], 3210.0f));
    CT_REQUIRE(nearly_equal(r[1], 6540.0f));

    // b is 3x2 with rows [1,2], [3,4], [5,6].
    const matrix<float, 3, 2> b{1, 2, 3, 4, 5, 6};
    const auto ab = a * b; // 2x2

    CT_REQUIRE(nearly_equal(ab(0, 0), 22.0f));
    CT_REQUIRE(nearly_equal(ab(0, 1), 28.0f));
    CT_REQUIRE(nearly_equal(ab(1, 0), 49.0f));
    CT_REQUIRE(nearly_equal(ab(1, 1), 64.0f));
}

void test_identity_and_inverse()
{
    using math::mat3f;
    using math::vec3f;

    const auto i = mat3f::identity();
    const vec3f v{1.0f, 2.0f, 3.0f};
    const auto out = i * v;

    CT_REQUIRE(nearly_equal(out[0], v[0]));
    CT_REQUIRE(nearly_equal(out[1], v[1]));
    CT_REQUIRE(nearly_equal(out[2], v[2]));
    CT_REQUIRE(math::is_identity(i));

    const mat3f m = mat3f::from_rows(
        vec3f{2.0f, 0.0f, 1.0f},
        vec3f{1.0f, 3.0f, 2.0f},
        vec3f{1.0f, 1.0f, 2.0f});

    const auto inv = math::inverse(m);
    CT_REQUIRE(math::is_identity(m * inv, 1e-4f));

    // A singular matrix has no inverse, and try_inverse reports that rather
    // than throwing.
    const mat3f singular = mat3f::from_rows(
        vec3f{1.0f, 2.0f, 3.0f},
        vec3f{2.0f, 4.0f, 6.0f},
        vec3f{1.0f, 1.0f, 1.0f});

    CT_REQUIRE(!math::try_inverse(singular).has_value());
    CT_REQUIRE(nearly_equal(math::determinant(singular), 0.0f));
}

void test_transpose_and_trace()
{
    using math::mat3f;
    using math::vec3f;

    const mat3f m = mat3f::from_rows(
        vec3f{1.0f, 2.0f, 3.0f},
        vec3f{4.0f, 5.0f, 6.0f},
        vec3f{7.0f, 8.0f, 9.0f});

    const auto t = math::transpose(m);
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c)
            CT_REQUIRE(nearly_equal(t(r, c), m(c, r)));

    CT_REQUIRE(nearly_equal(math::trace(m), 15.0f));
    CT_REQUIRE(math::is_symmetric(m + t));
}

} // namespace

int main()
{
    test_row_major_is_the_default_layout();
    test_element_access_is_independent_of_storage_order();
    test_from_rows_and_from_columns_are_transposes();
    test_mul_vec_and_mul_mat();
    test_identity_and_inverse();
    test_transpose_and_trace();
    return 0;
}
