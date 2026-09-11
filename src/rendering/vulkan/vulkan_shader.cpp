/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Shader modules of the Vulkan backend. Only SPIR-V is accepted; the bytecode is copied so `get_bytecode`
 * can hand it back, and the `VkShaderModule` is created eagerly so malformed blobs fail at `create_shader`.
 */

#include "vulkan_backend.hpp"
#include "vulkan_convert.hpp"

#include <cstring>

namespace catalyst::rendering::detail::vulkan
{

    void release_shader_objects(device_state &dev, shader_state &s) noexcept
    {
        if (s.module)
            vkDestroyShaderModule(dev.device, s.module, nullptr);
        s.module = VK_NULL_HANDLE;
    }

    namespace
    {
        constexpr std::uint32_t spirv_magic = 0x07230203u;

        bool looks_like_spirv(std::span<const std::byte> code) noexcept
        {
            if (code.size() < 4 || code.size() % 4 != 0)
                return false;
            std::uint32_t magic = 0;
            std::memcpy(&magic, code.data(), sizeof(magic));
            return magic == spirv_magic;
        }
    } // namespace

} // namespace catalyst::rendering::detail::vulkan

namespace catalyst::rendering::detail
{

    using namespace vulkan;

    resource_id create_shader(resource_id device, const shader_desc &desc)
    {
        device_state *dev = find_device(device);
        if (!dev || desc.bytecode.empty())
            return 0;

        if (desc.bytecode_format != shader_bytecode_format::spirv)
        {
            logging::error<detail::render_log>("create_shader: the Vulkan backend only accepts SPIR-V bytecode");
            return 0;
        }
        if (!looks_like_spirv(desc.bytecode))
        {
            logging::error<detail::render_log>(
                "create_shader: bytecode is not SPIR-V (bad magic number or size not a multiple of 4)");
            return 0;
        }

        shader_state s;
        s.owner = device;
        s.stage = desc.stage;
        s.bytecode_format = desc.bytecode_format;
        s.entry_point = desc.entry_point && *desc.entry_point ? desc.entry_point : "main";
        s.debug_name = copy_name(desc.debug_name);
        s.bytecode.assign(desc.bytecode.begin(), desc.bytecode.end());

        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = s.bytecode.size();
        info.pCode = reinterpret_cast<const std::uint32_t *>(s.bytecode.data());

        const VkResult result = vkCreateShaderModule(dev->device, &info, nullptr, &s.module);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("create_shader: vkCreateShaderModule failed ({})",
                                               result_string(result));
            return 0;
        }

        set_debug_name(*dev, VK_OBJECT_TYPE_SHADER_MODULE, handle_bits(s.module), s.debug_name);

        const resource_id id = allocate_id();
        reg().shaders.emplace(id, std::move(s));
        return id;
    }

    void destroy_shader(resource_id id) noexcept
    {
        shader_state *s = find(reg().shaders, id);
        if (!s)
            return;
        // Pipelines keep no reference to their modules once created, so this needs no deferral.
        if (device_state *dev = find_device(s->owner))
            release_shader_objects(*dev, *s);
        reg().shaders.erase(id);
    }

    bool is_shader_valid(resource_id id) noexcept
    {
        return find(reg().shaders, id) != nullptr;
    }

    std::span<const std::byte> get_bytecode(resource_id id) noexcept
    {
        const shader_state *s = find(reg().shaders, id);
        if (!s)
            return {};
        return s->bytecode;
    }

    shader_stage get_shader_stage(resource_id id) noexcept
    {
        const shader_state *s = find(reg().shaders, id);
        return s ? s->stage : shader_stage::vertex;
    }

} // namespace catalyst::rendering::detail
