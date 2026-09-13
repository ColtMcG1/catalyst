/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file loader.hpp
 * @brief The @ref catalyst::resource::loader seam and the @ref catalyst::resource::load call it
 * exists to make possible: name an asset, get a counted handle to it, and never write the
 * read-check-decode-insert sequence again.
 * @details Tier 1 left four steps for every asset type: resolve the name, ask the registry whether
 * it is already resident, read the bytes, decode them, insert. Four steps that are identical for
 * every type except the decode. This header is the seam that factors out the other three.
 *
 * **The seam is a trait, not a table.** `loader<T>` is a class template with no primary definition;
 * a type becomes loadable by specialising it. That is deliberately a compile-time registration and
 * not a runtime `register_loader<T>(...)` map:
 *
 *   - There is no global to initialise, so no static initialisation order to get wrong and no
 *     "loader not registered" failure that only happens in the shipped build.
 *   - `load<T>` resolves to a direct call. The registry and the vfs never see a `std::function`,
 *     and the decode is inlinable into the caller.
 *   - Asking for a type with no loader is a compile error naming the type, at the call site, rather
 *     than an `unsupported_format` at run time.
 *
 * The cost is the one thing a runtime table buys: loaders cannot be added by a plugin the
 * application did not compile against. When that is actually needed it is a `loader<T>`
 * specialisation that dispatches on a content type through a table of its own -- a decision that
 * belongs to the one asset type that needs it, not to every type in the system.
 *
 * **What the seam does not know.** Nothing here mentions PNG, JSON or any container. `loader<image>`
 * is specialised in image.hpp, next to the asset type it loads, and @ref registry / @ref vfs do not
 * include this header at all. That direction matters: the mount table's job is bytes, and it stays
 * ignorant of what the bytes are.
 *
 * **A loader may load.** @ref load_context hands a loader the same `vfs` and `store` the call
 * started from, so a material's loader can call `load<image>` for its textures and get the sharing,
 * the counting and the caching for free. Cycle detection for a declared dependency graph is not
 * here; a loader that recurses into itself will recurse forever, and the graph that would prevent
 * it is deferred with the rest of the dependency work. See docs/resource.md.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/events/task.hpp>
#include <catalyst/resource/error.hpp>
#include <catalyst/resource/handle.hpp>
#include <catalyst/resource/registry.hpp>
#include <catalyst/resource/uri/reference.hpp>
#include <catalyst/resource/vfs.hpp>

#include <concepts>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace catalyst::resource
{

    /**
     * @struct load_context
     * @brief What a loader is told besides the bytes.
     * @details Everything here is about *this* load, and the two references are the whole reason a
     * loader can have dependencies: a material's loader resolves a texture name against
     * @ref files -- relative to @ref name, which is what makes `"./albedo.png"` in a material mean
     * what its author meant -- and calls @ref load for it against @ref assets, so the texture is
     * shared and counted like anything else.
     *
     * The references are borrowed for the duration of the loader call and must not be stored. A
     * loader that wants to keep something past its return keeps a handle, which is exactly the
     * weak, re-resolvable reference the registry hands out for this purpose.
     */
    struct load_context
    {
        /** @brief The mount table the bytes came from, for a loader that needs more of them. */
        vfs &files;

        /** @brief The store the finished asset will go into, and the one a dependency should go into. */
        store &assets;

        /** @brief The canonical name being loaded. @ref resolve turns a reference written inside
         * this asset into a name the mount table can dispatch. */
        const uri &name;

        /**
         * @brief Resolves a reference found inside this asset against this asset's own name.
         * @details Not `vfs::resolve`, which resolves against `vfs::base` -- the URI a *batch* of
         * names came from. A reference written inside a material means what it means relative to
         * that material, so `"./albedo.png"` in `asset:/materials/stone.json` is
         * `asset:/materials/albedo.png` no matter what the vfs was last pointed at. Getting this
         * wrong is the classic dependency bug: it works until two materials in different
         * directories name the same relative path.
         *
         * The result is normalized, so it is the cache key the registry wants.
         */
        [[nodiscard]] std::expected<uri, error> resolve(std::string_view reference) const;
    };

    /**
     * @class loader
     * @brief Specialise to teach @ref load how to turn bytes into a `T`. No primary definition.
     * @details A specialisation provides one static function and, optionally, an options type:
     *
     * @code
     *   template <>
     *   struct loader<mesh>
     *   {
     *       using options = mesh_load_options;   // optional; defaults to no_load_options
     *
     *       static std::expected<mesh, error> decode(std::span<const std::byte> bytes,
     *                                                const load_context &ctx,
     *                                                const options &opt);
     *   };
     * @endcode
     *
     * `decode` is called with the complete bytes of the entry. It must not throw and must not touch
     * the registry for `T` itself -- @ref catalyst::resource::load does the insertion, because the
     * decision to cache under a name belongs to the caller's pipeline and not to the decoder.
     *
     * **It is `decode` and not `load` for a concrete reason.** A member named `load` would hide the
     * namespace-scope @ref catalyst::resource::load from every expression inside it, so the one
     * thing this seam most wants a loader to be able to do -- call `load<image>` for a dependency --
     * would need full qualification or fail to compile depending on the compiler. Naming the member
     * for what it does, which is turn bytes into an object, costs nothing and removes the trap. The
     * pipeline around it is the `load`.
     */
    template <typename T>
    struct loader;

    /**
     * @struct no_load_options
     * @brief The options type of a loader that declares none.
     * @details Spelled out and public because a loader author writes it: a `loader<T>` with no
     * options of its own still takes a third parameter, and that parameter has to have a name.
     */
    struct no_load_options
    {
    };

    namespace detail
    {
        template <typename T>
        concept declares_load_options = requires { typename loader<T>::options; };

        template <typename T>
        struct load_options_of
        {
            using type = no_load_options;
        };

        template <declares_load_options T>
        struct load_options_of<T>
        {
            using type = typename loader<T>::options;
        };

        /** @brief Stamps the asset's name onto a failure that did not already carry one. */
        [[nodiscard]] error with_name(error failure, const uri &name);

    } // namespace detail

    /**
     * @brief The options `loader<T>` takes, or an empty struct when it declares none.
     * @details Written out as a named alias so `load<T>` can default it and callers can spell it.
     */
    template <typename T>
    using load_options_t = typename detail::load_options_of<T>::type;

    /**
     * @concept loadable
     * @brief Satisfied by a type with a usable `loader<T>` specialisation.
     * @details The return type is pinned to `std::expected<T, error>` rather than merely being
     * convertible to it, so a loader that reports failure some other way is rejected here, where
     * the message names the loader, instead of somewhere inside @ref load.
     */
    template <typename T>
    concept loadable = requires(std::span<const std::byte> bytes, const load_context &ctx, const load_options_t<T> &opt) {
        { loader<T>::decode(bytes, ctx, opt) } -> std::same_as<std::expected<T, error>>;
    };

    // -----------------------------------------------------------------------------
    // load
    // -----------------------------------------------------------------------------

    /**
     * @brief Loads the asset named @p name, or returns the one already resident under that name.
     * @tparam T The asset type. Must have a `loader<T>`; see @ref loadable.
     * @param files The mount table to read through.
     * @param assets The store whose `registry<T>` owns the result.
     * @param name An already-resolved, normalized URI. Use `vfs::resolve` for anything else.
     * @param options Passed through to `loader<T>::decode`.
     * @return A handle carrying **one reference that the caller now owns**, or why not.
     *
     * @details The four steps, in order: `registry<T>::acquire`, and if that finds nothing,
     * `vfs::read`, `loader<T>::decode`, `registry<T>::insert`.
     *
     * **You own a reference either way.** A cache hit retains the existing asset before handing back
     * its handle, and a miss inserts at a count of one. Both paths hand the caller exactly one
     * reference to release, so a scene that loads the same texture twice releases it twice and the
     * count balances. That symmetry is the reason `acquire` exists as its own operation rather than
     * being spelled `find`.
     *
     * **The name is the identity, and options do not vary it.** Loading one URI twice with different
     * options hands back the first result both times -- the second call is a cache hit and the
     * loader never runs. That follows from the module's first choice, that a URI is the only name an
     * asset has (docs/resource.md), and it is the honest behaviour rather than a limitation to route
     * around: an asset that needs loading two ways needs two names. In practice this bites exactly
     * once, on @ref image_decode_options::srgb, and the fix there is a query parameter or a separate
     * cooked file, not a second cache key.
     *
     * Failures carry @p name in @ref error::uri, so a log line names the asset whichever of the four
     * steps produced it.
     */
    template <loadable T>
    [[nodiscard]] std::expected<asset_handle<T>, error> load(vfs &files, store &assets, const uri &name,
                                                             const load_options_t<T> &options = {});

    /**
     * @brief @ref load over a name that may be relative or unnormalized.
     * @details `vfs::resolve` first, so `"../textures/stone_d.png"` means what the manifest's author
     * meant and two spellings of one asset are one cache entry. Prefer the `uri` overload when you
     * are about to do several things with one name; this one re-resolves on every call.
     */
    template <loadable T>
    [[nodiscard]] std::expected<asset_handle<T>, error> load(vfs &files, store &assets, std::string_view reference,
                                                             const load_options_t<T> &options = {});

    /**
     * @brief The non-blocking form of @ref load.
     * @details Identical in every respect except that the read goes through `vfs::read_async`. Tier
     * 1's read completes on the calling thread, so today this differs from @ref load only in shape
     * -- which is the point: callers written against it get the Tier 3 worker pool without changing
     * a line.
     *
     * @warning @p files and @p assets are borrowed for the whole lifetime of the returned task, not
     * just the call that creates it. They must outlive the task, which for the expected usage --
     * awaiting it from a loop that owns both -- is free. @p name is taken by value because it will
     * not be.
     *
     * The insertion into the registry happens on whichever thread resumes the coroutine. A
     * `registry` is not internally synchronised, so that thread must be the one that owns the store;
     * see registry.hpp.
     */
    template <loadable T>
    [[nodiscard]] events::task<std::expected<asset_handle<T>, error>> load_async(vfs &files, store &assets, uri name,
                                                                                load_options_t<T> options = {});

} // namespace catalyst::resource

#include <catalyst/resource/detail/loader_impl.hpp>
