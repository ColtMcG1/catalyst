/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Shader modules. The rendering API consumes pre-compiled bytecode (SPIR-V, DXIL, ...); compiling from source and
 * reflecting the result are planned as a separate layer on top of this header.
 * @details A `shader` is one stage's worth of bytecode. Shaders are combined into a `pipeline` (see pipeline.hpp); they
 * are never bound directly to a command list. The bytecode is copied at creation so the caller's buffer may be freed
 * immediately afterwards.
 */

#pragma once

#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/types.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace catalyst::rendering
{

    /**
     * @enum shader_stage
     * @brief Programmable pipeline stage a shader module targets.
     */
    enum class shader_stage : std::uint8_t
    {
        vertex,
        fragment,
        compute,
    };

    [[nodiscard]] constexpr const char *to_string(shader_stage stage) noexcept
    {
        switch (stage)
        {
        case shader_stage::vertex:   return "vertex";
        case shader_stage::fragment: return "fragment";
        case shader_stage::compute:  return "compute";
        }
        return "unknown";
    }

    /**
     * @enum shader_bytecode_format
     * @brief Container format of the bytes handed to `create_shader`. Each backend accepts its native format only;
     * see `native_bytecode_format`.
     */
    enum class shader_bytecode_format : std::uint8_t
    {
        spirv,    ///< Vulkan
        dxil,     ///< D3D12 (Shader Model 6+)
        dxbc,     ///< D3D12 (legacy Shader Model 5.x)
        metallib, ///< Metal
    };

    /** @brief The bytecode format a backend consumes natively. */
    [[nodiscard]] constexpr shader_bytecode_format native_bytecode_format(backend_kind backend) noexcept
    {
        switch (backend)
        {
        case backend_kind::d3d12: return shader_bytecode_format::dxil;
        case backend_kind::metal: return shader_bytecode_format::metallib;
        case backend_kind::vulkan:
        case backend_kind::null:
            break;
        }
        return shader_bytecode_format::spirv;
    }

    /**
     * @struct shader_desc
     * @brief Creation parameters for a shader module.
     */
    struct shader_desc
    {
        shader_stage stage = shader_stage::vertex;
        shader_bytecode_format bytecode_format = shader_bytecode_format::spirv;
        /** Compiled shader blob. Copied by `create_shader`; must not be empty. */
        std::span<const std::byte> bytecode;
        /** Entry point symbol inside the blob. */
        const char *entry_point = "main";
        /** Optional label shown in graphics debuggers. Not owned; may be null. */
        const char *debug_name = nullptr;
    };

    struct shader_tag
    {
    };

    /**
     * @brief Handle to a shader module. See `create_shader`.
     */
    using shader = resource_handle<shader_tag>;

    /**
     * @brief Creates a shader module from bytecode. Returns an invalid handle when `dev` is invalid, the bytecode is empty
     * or the backend rejects it.
     */
    [[nodiscard]] shader create_shader(const device &dev, const shader_desc &desc);

    /**
     * @brief Releases the shader and resets `s` to an invalid handle. Pipelines already built from it stay valid.
     */
    void destroy_shader(shader &s) noexcept;

    [[nodiscard]] bool is_valid(const shader &s) noexcept;

    /**
     * @brief The backend's copy of the bytecode. Valid until the shader is destroyed; empty for an invalid handle.
     */
    [[nodiscard]] std::span<const std::byte> get_bytecode(const shader &s) noexcept;

    [[nodiscard]] shader_stage get_shader_stage(const shader &s) noexcept;

} // namespace catalyst::rendering
