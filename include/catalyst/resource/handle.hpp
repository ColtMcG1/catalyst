/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file handle.hpp
 * @brief The handle vocabulary of the asset system: @ref catalyst::resource::asset_handle, the
 * index/generation packing behind it, and the flag-enum helpers re-exported from the rendering
 * module.
 * @details **Nothing here is a second copy of the rendering module's handle machinery.**
 * `rendering::resource_handle<Tag>`, `rendering::resource_id` and the `flags_enum` operators live in
 * `<catalyst/rendering/types.hpp>` and are re-exported into `catalyst::resource` below. That header
 * is entirely `constexpr` templates and enums -- it declares no functions with external linkage, so
 * including it costs an include path and nothing else: `catalyst_resource` does not link
 * `catalyst_rendering`, and a headless build with `CATALYST_BUILD_RENDERING=OFF` still compiles
 * every line of this module. Only the GPU bridge (see docs/resource.md, Tier 3) links the renderer.
 *
 * What the asset system *does* add is a meaning for the 64 bits. A `rendering::resource_handle` is
 * an opaque id minted by a backend and interpreted only by that backend. An
 * @ref catalyst::resource::asset_handle is minted by a @ref catalyst::resource::registry and is
 * `generation:32 | index:32` -- the index says which slot, the generation says which *occupant* of
 * that slot. Assets get unloaded and their slots reused, so a handle kept across an unload must be
 * detectable as stale rather than silently resolving to whatever moved in. That is the one thing a
 * bare backend id cannot do, and the whole reason this file exists.
 *
 * Generations start at 1, so slot 0 is a usable slot and the all-zero id remains the "no asset"
 * value that `operator bool` tests for.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/rendering/types.hpp>

#include <cstdint>

namespace catalyst::resource
{
    // -----------------------------------------------------------------------------
    // Re-exports from catalyst::rendering
    // -----------------------------------------------------------------------------

    /// @brief The 64-bit identifier type. Defined once, in `<catalyst/rendering/types.hpp>`.
    using rendering::resource_id;

    /// @brief The type-safe handle template. Defined once, in `<catalyst/rendering/types.hpp>`.
    using rendering::resource_handle;

    /// @brief The opt-in trait marking an `enum class` as a set of bit flags.
    using rendering::is_flags_enum_v;

    /// @brief The concept satisfied by an enum opted in through @ref is_flags_enum_v.
    using rendering::flags_enum;

    /**
     * @name Flag-enum operators
     * @brief Re-exported so that argument-dependent lookup finds them for flag enums declared in
     * `catalyst::resource`. Using-*declarations* (unlike using-directives) are visible to ADL, so a
     * caller in any namespace can write `a | b` on a `catalyst::resource` flag enum.
     *
     * Opting an enum in still names the rendering namespace, because a variable template can only
     * be specialised where it was declared:
     * @code
     *   namespace catalyst::rendering {
     *       template <> inline constexpr bool is_flags_enum_v<catalyst::resource::source_caps> = true;
     *   }
     * @endcode
     * @{
     */
    using rendering::operator|;
    using rendering::operator&;
    using rendering::operator^;
    using rendering::operator~;
    using rendering::operator|=;
    using rendering::operator&=;
    using rendering::has_any;
    using rendering::has_flag;
    /** @} */

    // -----------------------------------------------------------------------------
    // Asset handles
    // -----------------------------------------------------------------------------

    /**
     * @brief A handle to an asset of type `T` held by a @ref registry.
     * @details `T` is both the stored type and the handle's tag, so `asset_handle<mesh>` and
     * `asset_handle<image>` are distinct types and neither converts to the other or to a
     * `rendering::texture`. A default-constructed handle is the null handle and tests `false`.
     *
     * A handle is a weak reference: holding one does not keep the asset loaded. Ask the registry to
     * resolve it every time you need the object, and treat `nullptr` back as "it went away" rather
     * than as a bug. See @ref registry::retain for the counted form.
     */
    template <typename T>
    using asset_handle = resource_handle<T>;

    /// @brief Number of slots a registry can address. The index half of an @ref asset_handle.
    inline constexpr std::uint32_t max_asset_slots = 0xFFFF'FFFFu;

    /**
     * @brief Packs a slot index and a generation into the id carried by an @ref asset_handle.
     * @param index Slot index, `0 .. max_asset_slots - 1`.
     * @param generation Occupancy counter for that slot; must be non-zero for the handle to be
     *        distinguishable from the null handle.
     */
    [[nodiscard]] constexpr resource_id make_asset_id(std::uint32_t index, std::uint32_t generation) noexcept
    {
        return (static_cast<resource_id>(generation) << 32) | static_cast<resource_id>(index);
    }

    /// @brief The slot index encoded in @p id. Meaningless for the null id.
    [[nodiscard]] constexpr std::uint32_t asset_index(resource_id id) noexcept
    {
        return static_cast<std::uint32_t>(id & 0xFFFF'FFFFull);
    }

    /// @brief The generation encoded in @p id. Zero only for the null id.
    [[nodiscard]] constexpr std::uint32_t asset_generation(resource_id id) noexcept
    {
        return static_cast<std::uint32_t>(id >> 32);
    }

    /// @brief The slot index a handle refers to.
    template <typename T>
    [[nodiscard]] constexpr std::uint32_t asset_index(asset_handle<T> h) noexcept
    {
        return asset_index(h.id());
    }

    /// @brief The generation a handle was minted with.
    template <typename T>
    [[nodiscard]] constexpr std::uint32_t asset_generation(asset_handle<T> h) noexcept
    {
        return asset_generation(h.id());
    }

} // namespace catalyst::resource
