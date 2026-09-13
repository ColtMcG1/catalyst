/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file registry.hpp
 * @brief @ref catalyst::resource::registry -- the slot table that owns loaded assets of one type
 * and hands out generation-checked handles to them -- and @ref catalyst::resource::store, which
 * owns one registry per type so an application has a single object to hold.
 * @details The two things a registry is for:
 *
 *   - **Identity that survives reuse.** Slots are recycled when assets unload. A handle carries the
 *     generation its slot was at when the handle was minted, so a handle held across an unload
 *     resolves to `nullptr` instead of to whatever moved into the slot. See handle.hpp; this is the
 *     one piece of handle semantics the asset system adds over `rendering::resource_handle`.
 *   - **One copy per name.** Assets are keyed by their canonical (resolved, normalized) URI, so two
 *     materials naming one texture get one texture. @ref registry::find is the lookup, and the
 *     loaders in Tier 2 consult it before they read a byte.
 *
 * **Lifetime is counted, not inferred.** @ref registry::insert starts an asset at one reference.
 * @ref registry::retain adds one, @ref registry::release drops one, and the asset is destroyed when
 * the count reaches zero. There is no tracing, no defer-to-frame-end and no eviction policy here --
 * a budget-driven cache is Tier 4, and it will be built *on* this, by releasing what it decides to
 * drop.
 *
 * A handle is a weak reference: holding one keeps nothing alive. That is deliberate. The strong
 * reference is the count, and something has to own it explicitly -- a scene, a level, a material --
 * which is exactly the question "who is keeping this 80 MB texture resident?" that a
 * `shared_ptr`-shaped API makes unanswerable.
 *
 * **Threads.** A registry is *not* internally synchronised. Resolve handles on whatever thread owns
 * the registry. Loading happens off-thread through @ref vfs::read_async; the resulting object is
 * handed to `insert` on the owning thread. Locking every `get` to allow otherwise would cost the
 * hot path -- resolving a handle -- for a benefit only the loader needs.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/resource/error.hpp>
#include <catalyst/resource/handle.hpp>
#include <catalyst/resource/uri/reference.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace catalyst::resource
{

    /**
     * @struct asset_info
     * @brief What a registry knows about a slot besides the object in it.
     */
    struct asset_info
    {
        /** @brief The canonical URI the asset was inserted under. Empty for an unnamed asset. */
        const uri *key = nullptr;

        /** @brief Outstanding references. Always at least 1 for a live asset. */
        std::uint32_t references = 0;

        /** @brief The slot's occupancy counter, as encoded in live handles to it. */
        std::uint32_t generation = 0;
    };

    /**
     * @class registry
     * @brief Owns every loaded asset of type `T` and the handles that name them.
     * @tparam T The asset type. Needs no base class, no traits and no virtuals -- only to be
     *         destructible and movable into a slot.
     */
    template <typename T>
    class registry
    {
    public:
        using value_type = T;
        using handle_type = asset_handle<T>;

        registry() = default;

        registry(const registry &) = delete;
        registry &operator=(const registry &) = delete;
        registry(registry &&) noexcept = default;
        registry &operator=(registry &&) noexcept = default;

        // -----------------------------------------------------------------
        // Insertion
        // -----------------------------------------------------------------

        /**
         * @brief Stores @p value under the canonical name @p key at a reference count of 1.
         * @param key The resolved, normalized URI. Use `vfs::resolve` to produce it.
         * @param value The loaded object, moved into the slot.
         * @return A handle to it, or @ref error_code::registry_full.
         * @details If @p key is already present the existing asset is *not* replaced: the call
         * retains it and returns its handle, and @p value is destroyed. That makes concurrent
         * loaders of one name converge instead of racing to overwrite. Use @ref replace for the
         * hot-reload case, where overwriting is the point.
         */
        [[nodiscard]] std::expected<handle_type, error> insert(uri key, T value);

        /** @brief @ref insert for an asset with no name -- a procedurally generated mesh, a render
         * target. Never found by @ref find, released like any other. */
        [[nodiscard]] std::expected<handle_type, error> insert_unnamed(T value);

        /**
         * @brief Replaces the object behind @p handle in place, keeping the slot, the generation
         * and the reference count.
         * @details The hot-reload operation: every handle held anywhere stays valid and starts
         * resolving to the new object. Fails with @ref error_code::stale_handle if the slot has
         * moved on.
         */
        std::expected<void, error> replace(handle_type handle, T value);

        // -----------------------------------------------------------------
        // Lookup
        // -----------------------------------------------------------------

        /**
         * @brief The object behind @p handle, or `nullptr` if the handle is null or stale.
         * @details The hot path. A pointer rather than a reference because "it went away" is an
         * ordinary answer here, and rather than `std::expected` because there is exactly one reason
         * it can fail and the caller is going to branch on the pointer anyway.
         *
         * The pointer is valid until the next insertion or release. Do not keep it.
         */
        [[nodiscard]] T *get(handle_type handle) noexcept;
        [[nodiscard]] const T *get(handle_type handle) const noexcept;

        /** @brief Whether @p handle resolves to a live asset. */
        [[nodiscard]] bool is_valid(handle_type handle) const noexcept { return get(handle) != nullptr; }

        /** @brief The handle for @p key, or the null handle if nothing is loaded under that name.
         * Does not retain -- see @ref acquire. */
        [[nodiscard]] handle_type find(const uri &key) const noexcept;

        /** @brief @ref find plus a @ref retain, as one step: the "is it already loaded?" call a
         * loader makes before reading bytes. Null handle back means go and load it. */
        [[nodiscard]] handle_type acquire(const uri &key) noexcept;

        /** @brief Name, reference count and generation of @p handle, or `nullopt` if it is stale. */
        [[nodiscard]] std::optional<asset_info> info(handle_type handle) const noexcept;

        // -----------------------------------------------------------------
        // Lifetime
        // -----------------------------------------------------------------

        /** @brief Adds a reference. @return false if @p handle is stale. */
        bool retain(handle_type handle) noexcept;

        /**
         * @brief Drops a reference, destroying the asset and freeing its slot when the count hits
         * zero.
         * @return true when this call destroyed the asset.
         * @details Releasing a stale handle is a no-op returning false, not an error: releasing
         * twice is the shape of a bug worth finding, but crashing on it during shutdown is worse.
         */
        bool release(handle_type handle) noexcept;

        // -----------------------------------------------------------------
        // Bulk
        // -----------------------------------------------------------------

        /** @brief Number of live assets. */
        [[nodiscard]] std::size_t size() const noexcept { return live_; }

        [[nodiscard]] bool empty() const noexcept { return live_ == 0; }

        /** @brief Destroys every asset regardless of reference count, invalidating every handle.
         * Shutdown only. */
        void clear() noexcept;

        /**
         * @brief Calls `fn(handle, T&)` for every live asset, in slot order.
         * @details Do not insert or release from inside @p fn.
         */
        template <typename F>
        void for_each(F &&fn);

    private:
        struct slot
        {
            std::optional<T> value;
            uri key;
            bool named = false;
            std::uint32_t generation = 1; // 1-based, so a live handle is never the null id
            std::uint32_t references = 0;
        };

        [[nodiscard]] slot *resolve(handle_type handle) noexcept;
        [[nodiscard]] const slot *resolve(handle_type handle) const noexcept;
        [[nodiscard]] std::expected<std::uint32_t, error> take_slot();
        void free_slot(std::uint32_t index) noexcept;

        std::vector<slot> slots_;
        std::vector<std::uint32_t> free_list_;
        std::unordered_map<uri, std::uint32_t> by_key_;
        std::size_t live_ = 0;
    };

    // -----------------------------------------------------------------------------
    // store
    // -----------------------------------------------------------------------------

    /**
     * @class store
     * @brief Owns one @ref registry per asset type, created on first use.
     * @details So an application holds one object -- `store` -- rather than a growing list of
     * registries it has to thread through everything. The type-erasure is only in the map: a
     * `registry<T>` recovered through @ref store::registry_for is the concrete class with no
     * virtual dispatch on `get`.
     *
     * Destruction order across types is unspecified, which matters if one asset type holds handles
     * into another. Where it matters, call @ref clear in the order you need before the store goes
     * away.
     */
    class store
    {
    public:
        store() = default;

        store(const store &) = delete;
        store &operator=(const store &) = delete;
        store(store &&) noexcept = default;
        store &operator=(store &&) noexcept = default;

        /** @brief The registry for `T`, created on first call. */
        template <typename T>
        [[nodiscard]] registry<T> &registry_for();

        /** @brief The registry for `T`, or `nullptr` if nothing of that type has been registered. */
        template <typename T>
        [[nodiscard]] const registry<T> *find_registry() const noexcept;

        /** @brief Destroys every asset in every registry. Shutdown only. */
        void clear() noexcept;

        /** @brief Number of asset types with a registry. */
        [[nodiscard]] std::size_t type_count() const noexcept { return registries_.size(); }

    private:
        struct holder
        {
            virtual ~holder() = default;
            virtual void clear() noexcept = 0;
        };

        template <typename T>
        struct typed_holder final : holder
        {
            registry<T> value;
            void clear() noexcept override { value.clear(); }
        };

        std::unordered_map<std::type_index, std::unique_ptr<holder>> registries_;
    };

} // namespace catalyst::resource

#include <catalyst/resource/detail/registry_impl.hpp>
