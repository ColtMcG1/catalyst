#pragma once

// -------------------------------------------------------------------------
// Debug assertions
// -------------------------------------------------------------------------
//
// MATH_ASSERT guards preconditions that are not worth an exception:
// operator[] indices, division by a zero-length vector, and the like. It
// is the standard assert unless NDEBUG is defined, and a no-op otherwise.
// A failing condition inside constant evaluation is a compile error either
// way. Define MATH_ASSERT before including any header to substitute your
// own handler.

#ifndef MATH_ASSERT
#ifdef NDEBUG
#define MATH_ASSERT(condition, message) ((void)0)
#else
#include <cassert>
#define MATH_ASSERT(condition, message) assert((condition) && message)
#endif
#endif
