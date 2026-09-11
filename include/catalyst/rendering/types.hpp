/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Shared vocabulary for the Catalyst rendering API: opaque resource handles, pixel formats, flag-enum helpers and the
 * small geometry structs (extents, viewports, scissors) used by buffers, textures, pipelines and command lists.
 * @details Every GPU object exposed by the rendering module is referred to through a `resource_handle<Tag>`. A handle is a
 * trivially copyable 64-bit identifier owned by the active backend; it carries no ownership of its own and is manipulated
 * exclusively through the free functions declared in the per-resource headers (e.g. `create_buffer` / `destroy_buffer`).
 * This mirrors `catalyst::platform::window` and keeps the public headers free of any backend-specific types.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace catalyst::rendering
{

    /**
     * @brief Backend-scoped identifier of a GPU resource. Zero is reserved for "no resource".
     */
    using resource_id = std::uint64_t;

    /**
     * @class resource_handle
     * @brief Type-safe wrapper around a `resource_id`. `Tag` is an empty struct that makes handles of different resource
     * kinds distinct types, so a `buffer` cannot be passed where a `texture` is expected.
     */
    template <typename Tag>
    class resource_handle
    {
    public:
        constexpr resource_handle() noexcept = default;
        constexpr explicit resource_handle(resource_id id) noexcept : id_(id) {}

        /** @brief Raw backend identifier. */
        [[nodiscard]] constexpr resource_id id() const noexcept { return id_; }

        /** @brief True when the handle refers to *some* resource. Use the per-resource `is_valid` to check it still exists. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return id_ != 0; }

        friend constexpr bool operator==(const resource_handle &, const resource_handle &) noexcept = default;

    private:
        resource_id id_{};
    };

    // -----------------------------------------------------------------------------
    // Flag enums
    // -----------------------------------------------------------------------------

    /**
     * @brief Opt-in trait: specialise to `true` for an `enum class` whose values are bit flags. Enables the bitwise
     * operators and `has_flag` / `has_any` below for that enum.
     */
    template <typename E>
    inline constexpr bool is_flags_enum_v = false;

    template <typename E>
    concept flags_enum = std::is_enum_v<E> && is_flags_enum_v<E>;

    template <flags_enum E>
    [[nodiscard]] constexpr E operator|(E a, E b) noexcept
    {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
    }

    template <flags_enum E>
    [[nodiscard]] constexpr E operator&(E a, E b) noexcept
    {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
    }

    template <flags_enum E>
    [[nodiscard]] constexpr E operator^(E a, E b) noexcept
    {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(a) ^ static_cast<U>(b));
    }

    template <flags_enum E>
    [[nodiscard]] constexpr E operator~(E a) noexcept
    {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(~static_cast<U>(a));
    }

    template <flags_enum E>
    constexpr E &operator|=(E &a, E b) noexcept { return a = a | b; }

    template <flags_enum E>
    constexpr E &operator&=(E &a, E b) noexcept { return a = a & b; }

    /** @brief True when every bit of `flag` is set in `value` (and `flag` is not empty). */
    template <flags_enum E>
    [[nodiscard]] constexpr bool has_flag(E value, E flag) noexcept
    {
        using U = std::underlying_type_t<E>;
        return static_cast<U>(flag) != 0 && (static_cast<U>(value) & static_cast<U>(flag)) == static_cast<U>(flag);
    }

    /** @brief True when at least one bit of `flags` is set in `value`. */
    template <flags_enum E>
    [[nodiscard]] constexpr bool has_any(E value, E flags) noexcept
    {
        using U = std::underlying_type_t<E>;
        return (static_cast<U>(value) & static_cast<U>(flags)) != 0;
    }

    // -----------------------------------------------------------------------------
    // Backends
    // -----------------------------------------------------------------------------

    /**
     * @enum backend_kind
     * @brief Identifies which graphics API implements the rendering module in this build.
     */
    enum class backend_kind : std::uint8_t
    {
        null,
        vulkan,
        d3d12,
        metal,
    };

    [[nodiscard]] constexpr const char *to_string(backend_kind kind) noexcept
    {
        switch (kind)
        {
        case backend_kind::null:   return "null";
        case backend_kind::vulkan: return "vulkan";
        case backend_kind::d3d12:  return "d3d12";
        case backend_kind::metal:  return "metal";
        }
        return "unknown";
    }

    // -----------------------------------------------------------------------------
    // Queues
    // -----------------------------------------------------------------------------

    /**
     * @enum queue_kind
     * @brief Which hardware queue work runs on.
     * @details Named here rather than in queue.hpp because both `timeline_point` and `queue` carry
     * one and neither header can include the other.
     *
     * A device exposes all three whatever the adapter has; `queue_info::dedicated` says whether the
     * one you asked for is real hardware or the graphics queue under another name. That is the
     * honest version of what `queue_type` used to be, which restricted what a command list could
     * record while every queue ran on the same `VkQueue` regardless.
     */
    enum class queue_kind : std::uint8_t
    {
        /** @brief Accepts everything: draws, dispatches and copies. Always dedicated. */
        graphics,
        /** @brief Dispatches and copies. Dedicated on adapters with an async-compute engine. */
        compute,
        /** @brief Copies only. Dedicated on adapters with a DMA engine, which is most of them. */
        copy,
    };

    /** @brief Number of values in @ref queue_kind. */
    inline constexpr std::size_t queue_kind_count = 3;

    /** @brief A short, stable name for a queue kind ("graphics", "compute", "copy"). */
    [[nodiscard]] constexpr const char *to_string(queue_kind kind) noexcept
    {
        switch (kind)
        {
        case queue_kind::graphics: return "graphics";
        case queue_kind::compute:  return "compute";
        case queue_kind::copy:     return "copy";
        }
        return "unknown";
    }

    /**
     * @brief True when a list recorded for `kind` may contain commands needing `required`.
     * @details The capability lattice: graphics can do everything, compute can dispatch and copy,
     * copy can only copy. Used at *record* time - a `copy` list refuses `dispatch`, a `compute` list
     * refuses `draw`.
     *
     * It deliberately says nothing about submission. A list may only be submitted to a queue of its
     * own kind, because a command buffer belongs to the queue family its pool was created from and
     * no other; see queue.hpp. The fallback for an adapter with no dedicated engine happens a level
     * below that, where the copy *queue* is the graphics queue under another name, so the list was
     * allocated from the right family all along.
     */
    [[nodiscard]] constexpr bool queue_accepts(queue_kind kind, queue_kind required) noexcept
    {
        return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(required);
    }

    // -----------------------------------------------------------------------------
    // Formats
    // -----------------------------------------------------------------------------

    /**
     * @enum format
     * @brief Pixel / vertex-element formats. The set is intentionally the intersection of what Vulkan, D3D12 and Metal all
     * support natively so every entry maps 1:1 onto each backend.
     */
    enum class format : std::uint8_t
    {
        unknown,

        r8_unorm,
        rg8_unorm,
        rgba8_unorm,
        rgba8_unorm_srgb,
        bgra8_unorm,
        bgra8_unorm_srgb,

        r16_float,
        rg16_float,
        rgba16_float,
        r16_uint,

        r32_uint,
        r32_sint,
        r32_float,
        rg32_float,
        rgb32_float,
        rgba32_float,

        d16_unorm,
        d32_float,
        d24_unorm_s8_uint,
        d32_float_s8_uint,
    };

    /** @brief Size in bytes of one texel / vertex element of `f`; 0 for `format::unknown`. */
    [[nodiscard]] constexpr std::uint32_t format_size_bytes(format f) noexcept
    {
        switch (f)
        {
        case format::unknown:           return 0;
        case format::r8_unorm:          return 1;
        case format::rg8_unorm:         return 2;
        case format::rgba8_unorm:       return 4;
        case format::rgba8_unorm_srgb:  return 4;
        case format::bgra8_unorm:       return 4;
        case format::bgra8_unorm_srgb:  return 4;
        case format::r16_float:         return 2;
        case format::rg16_float:        return 4;
        case format::rgba16_float:      return 8;
        case format::r16_uint:          return 2;
        case format::r32_uint:          return 4;
        case format::r32_sint:          return 4;
        case format::r32_float:         return 4;
        case format::rg32_float:        return 8;
        case format::rgb32_float:       return 12;
        case format::rgba32_float:      return 16;
        case format::d16_unorm:         return 2;
        case format::d32_float:         return 4;
        case format::d24_unorm_s8_uint: return 4;
        case format::d32_float_s8_uint: return 8;
        }
        return 0;
    }

    [[nodiscard]] constexpr bool is_depth_format(format f) noexcept
    {
        return f == format::d16_unorm || f == format::d32_float || f == format::d24_unorm_s8_uint ||
               f == format::d32_float_s8_uint;
    }

    [[nodiscard]] constexpr bool is_stencil_format(format f) noexcept
    {
        return f == format::d24_unorm_s8_uint || f == format::d32_float_s8_uint;
    }

    [[nodiscard]] constexpr bool is_srgb_format(format f) noexcept
    {
        return f == format::rgba8_unorm_srgb || f == format::bgra8_unorm_srgb;
    }

    /**
     * @enum index_type
     * @brief Element width of an index buffer.
     */
    enum class index_type : std::uint8_t
    {
        uint16,
        uint32,
    };

    [[nodiscard]] constexpr std::uint32_t index_size_bytes(index_type t) noexcept
    {
        return t == index_type::uint16 ? 2u : 4u;
    }

    // -----------------------------------------------------------------------------
    // Geometry
    // -----------------------------------------------------------------------------

    struct extent2d
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        friend constexpr bool operator==(const extent2d &, const extent2d &) noexcept = default;
    };

    struct extent3d
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t depth = 1;

        friend constexpr bool operator==(const extent3d &, const extent3d &) noexcept = default;
    };

    /**
     * @brief Viewport transform in framebuffer pixels. Depth range follows the [0, 1] convention on every backend.
     */
    struct viewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float min_depth = 0.0f;
        float max_depth = 1.0f;
    };

    /**
     * @brief Scissor rectangle in framebuffer pixels (origin top-left).
     */
    struct scissor_rect
    {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // -----------------------------------------------------------------------------
    // Limits shared by all backends
    // -----------------------------------------------------------------------------

    inline constexpr std::uint32_t max_color_attachments = 8;
    inline constexpr std::uint32_t max_vertex_bindings = 16;
    inline constexpr std::uint32_t max_vertex_attributes = 16;
    inline constexpr std::uint32_t max_push_constant_bytes = 128;

    /**
     * @name Resource binding slots
     * @brief Upper bounds of the `slot` argument of `set_uniform_buffer`, `set_storage_buffer`, `set_texture` and
     * `set_sampler` (see command.hpp for how slots map onto each backend's binding model).
     * @{
     */
    inline constexpr std::uint32_t max_uniform_buffer_slots = 16;
    inline constexpr std::uint32_t max_storage_buffer_slots = 16;
    inline constexpr std::uint32_t max_texture_slots = 16;
    inline constexpr std::uint32_t max_sampler_slots = 16;
    /** @} */

} // namespace catalyst::rendering
