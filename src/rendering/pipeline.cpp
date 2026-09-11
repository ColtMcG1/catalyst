/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Public pipeline API; performs backend-independent validation of the descriptors, then forwards to the active
 * backend.
 */

#include <catalyst/rendering/pipeline.hpp>

#include "detail_backend.hpp"
#include "detail_sync.hpp"

#include <algorithm>

namespace catalyst::rendering
{

    namespace
    {
        bool validate_vertex_layout(const vertex_layout &layout) noexcept
        {
            if (layout.bindings.size() > max_vertex_bindings || layout.attributes.size() > max_vertex_attributes)
                return false;

            for (const vertex_binding &b : layout.bindings)
            {
                if (b.binding >= max_vertex_bindings)
                    return false;
            }

            for (const vertex_attribute &a : layout.attributes)
            {
                if (a.element_format == format::unknown)
                    return false;

                const bool declared = std::any_of(layout.bindings.begin(), layout.bindings.end(),
                                                  [&](const vertex_binding &b) { return b.binding == a.binding; });
                if (!declared)
                    return false;
            }

            return true;
        }
    } // namespace

    pipeline create_graphics_pipeline(const device &dev, const graphics_pipeline_desc &desc)
    {
        if (!dev || !desc.vertex_shader || desc.sample_count == 0)
            return {};

        if (get_shader_stage(desc.vertex_shader) != shader_stage::vertex || !is_valid(desc.vertex_shader))
            return {};

        if (desc.fragment_shader &&
            (!is_valid(desc.fragment_shader) || get_shader_stage(desc.fragment_shader) != shader_stage::fragment))
            return {};

        if (!validate_vertex_layout(desc.vertex_input))
            return {};

        if (desc.color_formats.size() > max_color_attachments)
            return {};

        if (!desc.color_blend.empty() && desc.color_blend.size() != desc.color_formats.size())
            return {};

        for (const format f : desc.color_formats)
        {
            if (f == format::unknown || is_depth_format(f))
                return {};
        }

        if (desc.depth_format != format::unknown && !is_depth_format(desc.depth_format))
            return {};

        // Taken here rather than at the top: the validation above calls back into the public API
        // (`is_valid`, `get_shader_stage`), each of which takes the lock in shared mode, and a
        // shared lock is not reentrant.
        const detail::exclusive_guard guard;
        return pipeline{detail::create_graphics_pipeline(dev.id(), desc)};
    }

    pipeline create_compute_pipeline(const device &dev, const compute_pipeline_desc &desc)
    {
        if (!dev || !is_valid(desc.compute_shader))
            return {};
        if (get_shader_stage(desc.compute_shader) != shader_stage::compute)
            return {};
        const detail::exclusive_guard guard;
        return pipeline{detail::create_compute_pipeline(dev.id(), desc)};
    }

    void destroy_pipeline(pipeline &p) noexcept
    {
        if (!p)
            return;
        {
            const detail::exclusive_guard guard;
            detail::destroy_pipeline(p.id());
        }
        p = pipeline{};
    }

    bool is_valid(const pipeline &p) noexcept
    {
        if (!p)
            return false;
        const detail::shared_guard guard;
        return detail::is_pipeline_valid(p.id());
    }

    pipeline_type get_pipeline_type(const pipeline &p) noexcept
    {
        if (!p)
            return pipeline_type::graphics;
        const detail::shared_guard guard;
        return detail::get_pipeline_type(p.id());
    }

} // namespace catalyst::rendering
