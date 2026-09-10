#pragma once

#include "../test_common.hpp"

#include <cmath>

#include <catalyst/ui/geometry.hpp>

namespace catalyst::tests
{

inline bool near(float a, float b, float eps = 1e-3f) noexcept
{
    return std::fabs(a - b) <= eps;
}

inline bool near_point(const catalyst::ui::point &p, float x, float y, float eps = 1e-3f) noexcept
{
    return near(p.x(), x, eps) && near(p.y(), y, eps);
}

inline bool near_rect(const catalyst::ui::rect &r, float x, float y, float w, float h, float eps = 1e-3f) noexcept
{
    return near(r.min.x(), x, eps) && near(r.min.y(), y, eps) && near(r.max.x() - r.min.x(), w, eps) &&
           near(r.max.y() - r.min.y(), h, eps);
}

} // namespace catalyst::tests
