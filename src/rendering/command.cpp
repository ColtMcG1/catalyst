/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Public command-pool and command-list API; validates handles and forwards to the active
 * backend.
 * @details Every function here takes the module lock (detail_sync.hpp) before it touches the
 * backend, and which mode it takes says what the call does. `create_*`, `destroy_*` and
 * `reset_command_pool` change the registry, so they take it exclusively. Everything on the
 * recording path takes it in shared mode: recording mutates the list's own record and nothing that
 * another thread can see, so N workers recording into N pools hold the shared lock together and
 * never wait for one another. That is the whole of what makes Tier 3's parallel recording defined
 * rather than merely usually-fine.
 */

#include <catalyst/rendering/command.hpp>

#include "detail_backend.hpp"
#include "detail_sync.hpp"

namespace catalyst::rendering
{

    // -------------------------------------------------------------------------
    // Command pools
    // -------------------------------------------------------------------------

    command_pool create_command_pool(const device &dev, const command_pool_desc &desc)
    {
        if (!dev)
            return {};
        const detail::exclusive_guard guard;
        return command_pool{detail::create_command_pool(dev.id(), desc)};
    }

    void destroy_command_pool(command_pool &pool) noexcept
    {
        if (!pool)
            return;
        {
            const detail::exclusive_guard guard;
            detail::destroy_command_pool(pool.id());
        }
        pool = command_pool{};
    }

    bool is_valid(const command_pool &pool) noexcept
    {
        if (!pool)
            return false;
        const detail::shared_guard guard;
        return detail::is_command_pool_valid(pool.id());
    }

    command_pool_desc get_command_pool_desc(const command_pool &pool) noexcept
    {
        if (!pool)
            return {};
        const detail::shared_guard guard;
        return detail::get_command_pool_desc(pool.id());
    }

    device get_device(const command_pool &pool) noexcept
    {
        if (!pool)
            return {};
        const detail::shared_guard guard;
        return device{detail::get_command_pool_device(pool.id())};
    }

    std::expected<void, error> reset_command_pool(const command_pool &pool)
    {
        if (!pool)
            return std::unexpected(make_error(error_code::invalid_argument, "reset_command_pool"));
        const detail::exclusive_guard guard;
        return detail::reset_command_pool(pool.id());
    }

    // -------------------------------------------------------------------------
    // Command lists
    // -------------------------------------------------------------------------

    command_list create_command_list(const command_pool &pool, const char *debug_name)
    {
        if (!pool)
            return {};
        const detail::exclusive_guard guard;
        return command_list{detail::create_command_list_in_pool(pool.id(), debug_name)};
    }

    command_list create_command_list(const device &dev, const command_list_desc &desc)
    {
        if (!dev)
            return {};
        const detail::exclusive_guard guard;
        return command_list{detail::create_command_list(dev.id(), desc)};
    }

    void destroy_command_list(command_list &cl) noexcept
    {
        if (!cl)
            return;
        {
            const detail::exclusive_guard guard;
            detail::destroy_command_list(cl.id());
        }
        cl = command_list{};
    }

    bool is_valid(const command_list &cl) noexcept
    {
        if (!cl)
            return false;
        const detail::shared_guard guard;
        return detail::is_command_list_valid(cl.id());
    }

    command_list_desc get_command_list_desc(const command_list &cl) noexcept
    {
        if (!cl)
            return {};
        const detail::shared_guard guard;
        return detail::get_command_list_desc(cl.id());
    }

    timeline_point last_submission(const command_list &cl) noexcept
    {
        if (!cl)
            return {};
        const detail::shared_guard guard;
        return detail::command_list_last_submission(cl.id());
    }

    bool begin_recording(const command_list &cl)
    {
        if (!cl)
            return false;
        const detail::shared_guard guard;
        return detail::begin_recording(cl.id());
    }

    bool end_recording(const command_list &cl)
    {
        if (!cl)
            return false;
        const detail::shared_guard guard;
        return detail::end_recording(cl.id());
    }

    bool is_recording(const command_list &cl) noexcept
    {
        if (!cl)
            return false;
        const detail::shared_guard guard;
        return detail::is_recording(cl.id());
    }

    void begin_render_pass(const command_list &cl, const render_pass_desc &desc) noexcept
    {
        if (!cl)
            return;
        const detail::shared_guard guard;
        detail::begin_render_pass(cl.id(), desc);
    }

    void end_render_pass(const command_list &cl) noexcept
    {
        if (!cl)
            return;
        const detail::shared_guard guard;
        detail::end_render_pass(cl.id());
    }

    void set_pipeline(const command_list &cl, const pipeline &p) noexcept
    {
        if (!cl || !p)
            return;
        const detail::shared_guard guard;
        detail::set_pipeline(cl.id(), p.id());
    }

    void set_viewport(const command_list &cl, const viewport &vp) noexcept
    {
        if (!cl)
            return;
        const detail::shared_guard guard;
        detail::set_viewport(cl.id(), vp);
    }

    void set_scissor(const command_list &cl, const scissor_rect &rect) noexcept
    {
        if (!cl)
            return;
        const detail::shared_guard guard;
        detail::set_scissor(cl.id(), rect);
    }

    void set_vertex_buffer(const command_list &cl, std::uint32_t binding, const buffer &b,
                           std::size_t offset_bytes) noexcept
    {
        if (!cl || !b)
            return;
        const detail::shared_guard guard;
        detail::set_vertex_buffer(cl.id(), binding, b.id(), offset_bytes);
    }

    void set_index_buffer(const command_list &cl, const buffer &b, index_type type, std::size_t offset_bytes) noexcept
    {
        if (!cl || !b)
            return;
        const detail::shared_guard guard;
        detail::set_index_buffer(cl.id(), b.id(), type, offset_bytes);
    }

    void set_uniform_buffer(const command_list &cl, std::uint32_t slot, const buffer &b, std::size_t offset_bytes,
                            std::size_t size_bytes) noexcept
    {
        if (!cl || !b)
            return;
        const detail::shared_guard guard;
        detail::set_uniform_buffer(cl.id(), slot, b.id(), offset_bytes, size_bytes);
    }

    void set_storage_buffer(const command_list &cl, std::uint32_t slot, const buffer &b, std::size_t offset_bytes,
                            std::size_t size_bytes) noexcept
    {
        if (!cl || !b)
            return;
        const detail::shared_guard guard;
        detail::set_storage_buffer(cl.id(), slot, b.id(), offset_bytes, size_bytes);
    }

    void set_texture(const command_list &cl, std::uint32_t slot, const texture &t) noexcept
    {
        if (!cl || !t)
            return;
        const detail::shared_guard guard;
        detail::set_texture(cl.id(), slot, t.id());
    }

    void set_sampler(const command_list &cl, std::uint32_t slot, const sampler &s) noexcept
    {
        if (!cl || !s)
            return;
        const detail::shared_guard guard;
        detail::set_sampler(cl.id(), slot, s.id());
    }

    void push_constants(const command_list &cl, std::uint32_t offset_bytes, std::span<const std::byte> data) noexcept
    {
        if (!cl || data.empty())
            return;
        if (offset_bytes > max_push_constant_bytes || data.size() > max_push_constant_bytes - offset_bytes)
            return;
        const detail::shared_guard guard;
        detail::push_constants(cl.id(), offset_bytes, data);
    }

    void draw(const command_list &cl, std::uint32_t vertex_count, std::uint32_t instance_count,
              std::uint32_t first_vertex, std::uint32_t first_instance) noexcept
    {
        if (!cl || vertex_count == 0 || instance_count == 0)
            return;
        const detail::shared_guard guard;
        detail::draw(cl.id(), vertex_count, instance_count, first_vertex, first_instance);
    }

    void draw_indexed(const command_list &cl, std::uint32_t index_count, std::uint32_t instance_count,
                      std::uint32_t first_index, std::int32_t vertex_offset, std::uint32_t first_instance) noexcept
    {
        if (!cl || index_count == 0 || instance_count == 0)
            return;
        const detail::shared_guard guard;
        detail::draw_indexed(cl.id(), index_count, instance_count, first_index, vertex_offset, first_instance);
    }

    void dispatch(const command_list &cl, std::uint32_t group_count_x, std::uint32_t group_count_y,
                  std::uint32_t group_count_z) noexcept
    {
        if (!cl || group_count_x == 0 || group_count_y == 0 || group_count_z == 0)
            return;
        const detail::shared_guard guard;
        detail::dispatch(cl.id(), group_count_x, group_count_y, group_count_z);
    }

    void copy_buffer(const command_list &cl, const buffer &src, std::size_t src_offset_bytes, const buffer &dst,
                     std::size_t dst_offset_bytes, std::size_t size_bytes) noexcept
    {
        if (!cl || !src || !dst || size_bytes == 0)
            return;
        const detail::shared_guard guard;
        detail::copy_buffer(cl.id(), src.id(), src_offset_bytes, dst.id(), dst_offset_bytes, size_bytes);
    }

} // namespace catalyst::rendering
