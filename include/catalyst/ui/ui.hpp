/**
 * @file ui.hpp
 * @brief Umbrella header for the catalyst::ui module.
 * @details Including this header pulls in the whole UI module: the CSS-like measurement types, the
 * geometry and color primitives, the style properties, the retained node tree and the layout engine.
 * Individual headers can be included instead when only part of the module is needed.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/ui/color.hpp>
#include <catalyst/ui/geometry.hpp>
#include <catalyst/ui/layout.hpp>
#include <catalyst/ui/measurement.hpp>
#include <catalyst/ui/node.hpp>
#include <catalyst/ui/style.hpp>

/**
 * @namespace catalyst::ui
 * @brief Retained, declarative user interface: a tree of styled nodes, a flexbox layout engine, and
 * the draw list they produce.
 * @details Applications build a `tree` of `node` handles, describe each one with a `style` written in
 * CSS-like units (`px`, `dp`, `em`, `rem`, `%`, `vw`, `vh`, and physical units), and call `layout` to
 * turn that description into positioned boxes. The module never talks to a graphics API itself: it
 * emits a backend-agnostic batch that a renderer submits, which keeps layout testable without a
 * device. Content the layout engine cannot measure on its own, such as text, reports its intrinsic
 * size through a `measure_fn` callback.
 */
namespace catalyst::ui
{

    /**
     * @fn module_name
     * @brief Returns the name of this module as a string. This can be used for logging, debugging, or any situation where you want to identify the module by name.
     * @return A string literal representing the name of this module.
     */
    const char *module_name();

} // namespace catalyst::ui
