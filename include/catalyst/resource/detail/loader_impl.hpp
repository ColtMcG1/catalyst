/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file detail/loader_impl.hpp
 * @brief Definitions for the templates declared in loader.hpp. Included at the bottom of it; not a
 * header to include directly.
 * @details The sync and async bodies are deliberately written out twice rather than sharing a
 * common helper. They differ by one line -- `vfs::read` against `co_await vfs::read_async` -- and
 * the shape that would let them share it is a callback or a coroutine wrapper around the whole
 * pipeline, which costs more in indirection and in stack lifetime hazards than the eight duplicated
 * lines cost in maintenance.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <utility>

namespace catalyst::resource
{

    template <loadable T>
    std::expected<asset_handle<T>, error> load(vfs &files, store &assets, const uri &name,
                                               const load_options_t<T> &options)
    {
        registry<T> &reg = assets.registry_for<T>();

        // Resident already? `acquire` retains, so the handle that comes back carries the reference
        // this call promises whether it loaded anything or not.
        if (const asset_handle<T> existing = reg.acquire(name))
            return existing;

        auto bytes = files.read(name);
        if (!bytes)
            return std::unexpected(detail::with_name(std::move(bytes.error()), name));

        const load_context context{files, assets, name};
        auto object = loader<T>::decode(bytes->bytes(), context, options);
        if (!object)
            return std::unexpected(detail::with_name(std::move(object.error()), name));

        // A loader is allowed to have loaded something else under this name while it ran -- a
        // material pulling a texture that pulls the material back, say. `insert` converges on the
        // first one rather than overwriting, and retains it, so the contract above still holds.
        auto handle = reg.insert(name, std::move(*object));
        if (!handle)
            return std::unexpected(detail::with_name(std::move(handle.error()), name));

        return *handle;
    }

    template <loadable T>
    std::expected<asset_handle<T>, error> load(vfs &files, store &assets, std::string_view reference,
                                               const load_options_t<T> &options)
    {
        auto name = files.resolve(reference);
        if (!name)
            return std::unexpected(std::move(name.error()));

        return load<T>(files, assets, *name, options);
    }

    template <loadable T>
    events::task<std::expected<asset_handle<T>, error>> load_async(vfs &files, store &assets, uri name,
                                                                   load_options_t<T> options)
    {
        registry<T> &reg = assets.registry_for<T>();

        if (const asset_handle<T> existing = reg.acquire(name))
            co_return existing;

        auto bytes = co_await files.read_async(name);
        if (!bytes)
            co_return std::unexpected(detail::with_name(std::move(bytes.error()), name));

        const load_context context{files, assets, name};
        auto object = loader<T>::decode(bytes->bytes(), context, options);
        if (!object)
            co_return std::unexpected(detail::with_name(std::move(object.error()), name));

        auto handle = reg.insert(name, std::move(*object));
        if (!handle)
            co_return std::unexpected(detail::with_name(std::move(handle.error()), name));

        co_return *handle;
    }

} // namespace catalyst::resource
