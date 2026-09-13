/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Pipeline state objects: the immutable combination of shaders, vertex layout and fixed-function state
 * (rasterizer, depth/stencil, blending) that a draw or dispatch executes with.
 * @details All state is baked at creation, matching how Vulkan, D3D12 and Metal work internally. The only state that is
 * set dynamically on the command list is the viewport and scissor rectangle.
 */

#pragma once

#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/shader.hpp>
#include <catalyst/rendering/types.hpp>

#include <cstdint>
#include <span>

namespace catalyst::rendering
{

    // -----------------------------------------------------------------------------
    // Vertex input
    // -----------------------------------------------------------------------------

    enum class vertex_input_rate : std::uint8_t
    {
        per_vertex,
        per_instance,
    };

    /**
     * @struct vertex_binding
     * @brief One vertex buffer slot: which `set_vertex_buffer` binding index feeds it and how far apart elements are.
     */
    struct vertex_binding
    {
        std::uint32_t binding = 0;
        std::uint32_t stride_bytes = 0;
        vertex_input_rate input_rate = vertex_input_rate::per_vertex;
    };

    /**
     * @struct vertex_attribute
     * @brief One shader input (`location`) sourced from a `vertex_binding` at a byte offset within each element.
     */
    struct vertex_attribute
    {
        std::uint32_t location = 0;
        std::uint32_t binding = 0;
        format element_format = format::unknown;
        std::uint32_t offset_bytes = 0;
    };

    /**
     * @struct vertex_layout
     * @brief Non-owning views over the bindings and attributes of a graphics pipeline. The spans only need to stay alive
     * for the duration of `create_graphics_pipeline`.
     */
    struct vertex_layout
    {
        std::span<const vertex_binding> bindings;
        std::span<const vertex_attribute> attributes;
    };

    // -----------------------------------------------------------------------------
    // Fixed-function state
    // -----------------------------------------------------------------------------

    enum class primitive_topology : std::uint8_t
    {
        point_list,
        line_list,
        line_strip,
        triangle_list,
        triangle_strip,
    };

    enum class fill_mode : std::uint8_t
    {
        solid,
        wireframe,
    };

    enum class cull_mode : std::uint8_t
    {
        none,
        front,
        back,
    };

    enum class front_face : std::uint8_t
    {
        counter_clockwise,
        clockwise,
    };

    enum class compare_op : std::uint8_t
    {
        never,
        less,
        equal,
        less_equal,
        greater,
        not_equal,
        greater_equal,
        always,
    };

    struct rasterizer_state
    {
        fill_mode fill = fill_mode::solid;
        cull_mode cull = cull_mode::back;
        front_face front = front_face::counter_clockwise;
        bool depth_clamp = false;
        float depth_bias = 0.0f;
        float depth_bias_slope = 0.0f;
    };

    struct depth_stencil_state
    {
        bool depth_test = false;
        bool depth_write = false;
        compare_op depth_compare = compare_op::less;
    };

    enum class blend_factor : std::uint8_t
    {
        zero,
        one,
        src_color,
        one_minus_src_color,
        dst_color,
        one_minus_dst_color,
        src_alpha,
        one_minus_src_alpha,
        dst_alpha,
        one_minus_dst_alpha,
        constant_color,
        one_minus_constant_color,
    };

    enum class blend_op : std::uint8_t
    {
        add,
        subtract,
        reverse_subtract,
        min,
        max,
    };

    enum class color_write_mask : std::uint8_t
    {
        none = 0,
        r = 1u << 0,
        g = 1u << 1,
        b = 1u << 2,
        a = 1u << 3,
        all = 0x0F,
    };

    template <>
    inline constexpr bool is_flags_enum_v<color_write_mask> = true;

    /**
     * @struct blend_state
     * @brief Per-render-target blending. Defaults to opaque (blending disabled, all channels written).
     */
    struct blend_state
    {
        bool enabled = false;
        blend_factor src_color = blend_factor::one;
        blend_factor dst_color = blend_factor::zero;
        blend_op color_op = blend_op::add;
        blend_factor src_alpha = blend_factor::one;
        blend_factor dst_alpha = blend_factor::zero;
        blend_op alpha_op = blend_op::add;
        color_write_mask write_mask = color_write_mask::all;
    };

    /** @brief Blending disabled; source overwrites destination. */
    [[nodiscard]] constexpr blend_state blend_opaque() noexcept { return {}; }

    /** @brief Standard premultiplied-free alpha blending: `src * src.a + dst * (1 - src.a)`. */
    [[nodiscard]] constexpr blend_state blend_alpha() noexcept
    {
        blend_state s;
        s.enabled = true;
        s.src_color = blend_factor::src_alpha;
        s.dst_color = blend_factor::one_minus_src_alpha;
        s.src_alpha = blend_factor::one;
        s.dst_alpha = blend_factor::one_minus_src_alpha;
        return s;
    }

    // -----------------------------------------------------------------------------
    // Pipelines
    // -----------------------------------------------------------------------------

    enum class pipeline_type : std::uint8_t
    {
        graphics,
        compute,
    };

    /**
     * @struct graphics_pipeline_desc
     * @brief Everything needed to build a graphics pipeline. Spans are non-owning and only read during creation.
     */
    struct graphics_pipeline_desc
    {
        shader vertex_shader;
        /** May be invalid for depth-only passes. */
        shader fragment_shader;
        vertex_layout vertex_input;
        primitive_topology topology = primitive_topology::triangle_list;
        rasterizer_state rasterizer;
        depth_stencil_state depth_stencil;
        /** Formats of the colour attachments the pipeline will render into, in attachment order. */
        std::span<const format> color_formats;
        /** One entry per colour attachment, or empty to use `blend_opaque()` for all of them. */
        std::span<const blend_state> color_blend;
        /** `format::unknown` when the pipeline has no depth attachment. */
        format depth_format = format::unknown;
        std::uint32_t sample_count = 1;
        const char *debug_name = nullptr;
    };

    /**
     * @struct compute_pipeline_desc
     * @brief Everything needed to build a compute pipeline.
     */
    struct compute_pipeline_desc
    {
        shader compute_shader;
        const char *debug_name = nullptr;
    };

    struct pipeline_tag
    {
    };

    /**
     * @brief Handle to a graphics or compute pipeline. See `create_graphics_pipeline` / `create_compute_pipeline`.
     */
    using pipeline = resource_handle<pipeline_tag>;

    /**
     * @brief Builds a graphics pipeline. Returns an invalid handle when the device or vertex shader is invalid, shader
     * stages do not match their slots, the vertex layout references an undeclared binding, or attachment counts exceed
     * `max_color_attachments`.
     */
    [[nodiscard]] pipeline create_graphics_pipeline(const device &dev, const graphics_pipeline_desc &desc);

    /**
     * @brief Builds a compute pipeline. Returns an invalid handle when the device or shader is invalid or the shader is
     * not a compute-stage module.
     */
    [[nodiscard]] pipeline create_compute_pipeline(const device &dev, const compute_pipeline_desc &desc);

    /**
     * @brief Releases the pipeline and resets `p` to an invalid handle. Command lists that reference it must have
     * finished executing.
     */
    void destroy_pipeline(pipeline &p) noexcept;

    [[nodiscard]] bool is_valid(const pipeline &p) noexcept;

    [[nodiscard]] pipeline_type get_pipeline_type(const pipeline &p) noexcept;

} // namespace catalyst::rendering
