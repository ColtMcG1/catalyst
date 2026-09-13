/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file registry_impl.hpp
 * @brief Definitions for the `catalyst::resource::registry` and `catalyst::resource::store`
 * templates. Included at the bottom of registry.hpp; never include this directly.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/resource/registry.hpp>

namespace catalyst::resource
{

    // -----------------------------------------------------------------------------
    // Slot plumbing
    // -----------------------------------------------------------------------------

    template <typename T>
    auto registry<T>::resolve(handle_type handle) noexcept -> slot *
    {
        return const_cast<slot *>(std::as_const(*this).resolve(handle));
    }

    template <typename T>
    auto registry<T>::resolve(handle_type handle) const noexcept -> const slot *
    {
        if (!handle)
            return nullptr;

        const std::uint32_t index = asset_index(handle);
        if (index >= slots_.size())
            return nullptr;

        const slot &s = slots_[index];
        if (!s.value.has_value() || s.generation != asset_generation(handle))
            return nullptr;

        return &s;
    }

    template <typename T>
    std::expected<std::uint32_t, error> registry<T>::take_slot()
    {
        if (!free_list_.empty())
        {
            const std::uint32_t index = free_list_.back();
            free_list_.pop_back();
            return index;
        }

        if (slots_.size() >= max_asset_slots)
            return std::unexpected(make_error(error_code::registry_full));

        slots_.emplace_back();
        return static_cast<std::uint32_t>(slots_.size() - 1);
    }

    template <typename T>
    void registry<T>::free_slot(std::uint32_t index) noexcept
    {
        slot &s = slots_[index];

        if (s.named)
            by_key_.erase(s.key);

        s.value.reset();
        s.key = uri{};
        s.named = false;
        s.references = 0;

        // Bump past this occupant so handles minted for it stop resolving. Wrapping back onto 0
        // would make a stale handle look null rather than stale, so skip it.
        ++s.generation;
        if (s.generation == 0)
            s.generation = 1;

        free_list_.push_back(index);
        --live_;
    }

    // -----------------------------------------------------------------------------
    // Insertion
    // -----------------------------------------------------------------------------

    template <typename T>
    std::expected<typename registry<T>::handle_type, error> registry<T>::insert(uri key, T value)
    {
        if (const auto it = by_key_.find(key); it != by_key_.end())
        {
            // Already loaded under this name: converge on the existing asset rather than replace it.
            slot &existing = slots_[it->second];
            ++existing.references;
            return handle_type{make_asset_id(it->second, existing.generation)};
        }

        auto index = take_slot();
        if (!index)
        {
            index.error().uri = key.string();
            return std::unexpected(index.error());
        }

        slot &s = slots_[*index];
        s.value.emplace(std::move(value));
        s.key = std::move(key);
        s.named = true;
        s.references = 1;
        ++live_;

        by_key_.emplace(s.key, *index);
        return handle_type{make_asset_id(*index, s.generation)};
    }

    template <typename T>
    std::expected<typename registry<T>::handle_type, error> registry<T>::insert_unnamed(T value)
    {
        auto index = take_slot();
        if (!index)
            return std::unexpected(index.error());

        slot &s = slots_[*index];
        s.value.emplace(std::move(value));
        s.named = false;
        s.references = 1;
        ++live_;

        return handle_type{make_asset_id(*index, s.generation)};
    }

    template <typename T>
    std::expected<void, error> registry<T>::replace(handle_type handle, T value)
    {
        slot *s = resolve(handle);
        if (!s)
            return std::unexpected(make_error(error_code::stale_handle));

        s->value.emplace(std::move(value));
        return {};
    }

    // -----------------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------------

    template <typename T>
    T *registry<T>::get(handle_type handle) noexcept
    {
        slot *s = resolve(handle);
        return s ? &*s->value : nullptr;
    }

    template <typename T>
    const T *registry<T>::get(handle_type handle) const noexcept
    {
        const slot *s = resolve(handle);
        return s ? &*s->value : nullptr;
    }

    template <typename T>
    typename registry<T>::handle_type registry<T>::find(const uri &key) const noexcept
    {
        const auto it = by_key_.find(key);
        if (it == by_key_.end())
            return handle_type{};

        return handle_type{make_asset_id(it->second, slots_[it->second].generation)};
    }

    template <typename T>
    typename registry<T>::handle_type registry<T>::acquire(const uri &key) noexcept
    {
        const auto it = by_key_.find(key);
        if (it == by_key_.end())
            return handle_type{};

        slot &s = slots_[it->second];
        ++s.references;
        return handle_type{make_asset_id(it->second, s.generation)};
    }

    template <typename T>
    std::optional<asset_info> registry<T>::info(handle_type handle) const noexcept
    {
        const slot *s = resolve(handle);
        if (!s)
            return std::nullopt;

        return asset_info{s->named ? &s->key : nullptr, s->references, s->generation};
    }

    // -----------------------------------------------------------------------------
    // Lifetime
    // -----------------------------------------------------------------------------

    template <typename T>
    bool registry<T>::retain(handle_type handle) noexcept
    {
        slot *s = resolve(handle);
        if (!s)
            return false;

        ++s->references;
        return true;
    }

    template <typename T>
    bool registry<T>::release(handle_type handle) noexcept
    {
        slot *s = resolve(handle);
        if (!s)
            return false;

        if (--s->references > 0)
            return false;

        free_slot(asset_index(handle));
        return true;
    }

    template <typename T>
    void registry<T>::clear() noexcept
    {
        slots_.clear();
        free_list_.clear();
        by_key_.clear();
        live_ = 0;
    }

    template <typename T>
    template <typename F>
    void registry<T>::for_each(F &&fn)
    {
        for (std::uint32_t index = 0; index < slots_.size(); ++index)
        {
            slot &s = slots_[index];
            if (!s.value.has_value())
                continue;

            fn(handle_type{make_asset_id(index, s.generation)}, *s.value);
        }
    }

    // -----------------------------------------------------------------------------
    // store
    // -----------------------------------------------------------------------------

    template <typename T>
    registry<T> &store::registry_for()
    {
        const std::type_index key{typeid(T)};

        auto it = registries_.find(key);
        if (it == registries_.end())
            it = registries_.emplace(key, std::make_unique<typed_holder<T>>()).first;

        return static_cast<typed_holder<T> *>(it->second.get())->value;
    }

    inline void store::clear() noexcept
    {
        for (auto &[type, held] : registries_)
            held->clear();
    }

    template <typename T>
    const registry<T> *store::find_registry() const noexcept
    {
        const auto it = registries_.find(std::type_index{typeid(T)});
        if (it == registries_.end())
            return nullptr;

        return &static_cast<const typed_holder<T> *>(it->second.get())->value;
    }

} // namespace catalyst::resource
