/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Graphics and compute pipelines of the Vulkan backend. Graphics pipelines target dynamic rendering (attachment
 * formats baked in, no VkRenderPass), keep viewport and scissor dynamic, and all share the device's pipeline layout.
 */

#include "vulkan_backend.hpp"
#include "vulkan_convert.hpp"

#include <array>
#include <vector>

namespace catalyst::rendering::detail::vulkan
{

    void release_pipeline_objects(device_state &dev, pipeline_state &p) noexcept
    {
        if (p.pipeline)
            vkDestroyPipeline(dev.device, p.pipeline, nullptr);
        p.pipeline = VK_NULL_HANDLE;
    }

    namespace
    {
        const shader_state *shader_for(resource_id device, const shader &handle, shader_stage stage) noexcept
        {
            const shader_state *s = find(reg().shaders, handle.id());
            if (!s || s->owner != device || s->stage != stage || !s->module)
                return nullptr;
            return s;
        }

        VkPipelineShaderStageCreateInfo stage_info(const shader_state &s) noexcept
        {
            VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            info.stage = to_vk_shader_stage(s.stage);
            info.module = s.module;
            info.pName = s.entry_point.c_str();
            return info;
        }

        VkPipelineColorBlendAttachmentState blend_attachment(const blend_state &b) noexcept
        {
            VkPipelineColorBlendAttachmentState a{};
            a.blendEnable = b.enabled ? VK_TRUE : VK_FALSE;
            a.srcColorBlendFactor = to_vk_blend_factor(b.src_color);
            a.dstColorBlendFactor = to_vk_blend_factor(b.dst_color);
            a.colorBlendOp = to_vk_blend_op(b.color_op);
            a.srcAlphaBlendFactor = to_vk_blend_factor(b.src_alpha);
            a.dstAlphaBlendFactor = to_vk_blend_factor(b.dst_alpha);
            a.alphaBlendOp = to_vk_blend_op(b.alpha_op);
            a.colorWriteMask = to_vk_color_mask(b.write_mask);
            return a;
        }

        resource_id register_pipeline(device_state &dev, resource_id owner, pipeline_type type, VkPipeline pipeline,
                                      const char *debug_name)
        {
            if (debug_name)
                set_debug_name(dev, VK_OBJECT_TYPE_PIPELINE, handle_bits(pipeline), debug_name);

            pipeline_state p;
            p.owner = owner;
            p.type = type;
            p.pipeline = pipeline;

            const resource_id id = allocate_id();
            reg().pipelines.emplace(id, p);
            return id;
        }
    } // namespace

} // namespace catalyst::rendering::detail::vulkan

namespace catalyst::rendering::detail
{

    using namespace vulkan;

    resource_id create_graphics_pipeline(resource_id device, const graphics_pipeline_desc &desc)
    {
        device_state *dev = find_device(device);
        if (!dev)
            return 0;

        const shader_state *vs = shader_for(device, desc.vertex_shader, shader_stage::vertex);
        if (!vs)
            return 0;
        const shader_state *fs = nullptr;
        if (desc.fragment_shader)
        {
            fs = shader_for(device, desc.fragment_shader, shader_stage::fragment);
            if (!fs)
                return 0;
        }

        std::vector<VkPipelineShaderStageCreateInfo> stages;
        stages.push_back(stage_info(*vs));
        if (fs)
            stages.push_back(stage_info(*fs));

        // Vertex input.
        std::vector<VkVertexInputBindingDescription> bindings;
        bindings.reserve(desc.vertex_input.bindings.size());
        for (const vertex_binding &b : desc.vertex_input.bindings)
            bindings.push_back({b.binding, b.stride_bytes, to_vk_input_rate(b.input_rate)});

        std::vector<VkVertexInputAttributeDescription> attributes;
        attributes.reserve(desc.vertex_input.attributes.size());
        for (const vertex_attribute &a : desc.vertex_input.attributes)
            attributes.push_back({a.location, a.binding, to_vk_format(a.element_format), a.offset_bytes});

        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertex_input.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
        vertex_input.pVertexBindingDescriptions = bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        input_assembly.topology = to_vk_topology(desc.topology);
        input_assembly.primitiveRestartEnable = VK_FALSE;

        // Viewport and scissor are dynamic; only the counts matter here.
        VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.depthClampEnable = (desc.rasterizer.depth_clamp && dev->features.depth_clamp) ? VK_TRUE : VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = to_vk_polygon_mode(desc.rasterizer.fill);
        if (rasterizer.polygonMode != VK_POLYGON_MODE_FILL && !dev->features.fill_mode_non_solid)
        {
            logging::warn<detail::render_log>(
                "create_graphics_pipeline: wireframe fill is not supported by this adapter; using solid");
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        }
        rasterizer.cullMode = to_vk_cull_mode(desc.rasterizer.cull);
        rasterizer.frontFace = to_vk_front_face(desc.rasterizer.front);
        rasterizer.depthBiasEnable =
            (desc.rasterizer.depth_bias != 0.0f || desc.rasterizer.depth_bias_slope != 0.0f) ? VK_TRUE : VK_FALSE;
        rasterizer.depthBiasConstantFactor = desc.rasterizer.depth_bias;
        rasterizer.depthBiasSlopeFactor = desc.rasterizer.depth_bias_slope;
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = to_vk_sample_count(desc.sample_count);

        const format depth_format = resolve_format(*dev, desc.depth_format);
        VkPipelineDepthStencilStateCreateInfo depth_stencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth_stencil.depthTestEnable =
            (depth_format != format::unknown && desc.depth_stencil.depth_test) ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable =
            (depth_format != format::unknown && desc.depth_stencil.depth_write) ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = to_vk_compare_op(desc.depth_stencil.depth_compare);
        depth_stencil.depthBoundsTestEnable = VK_FALSE;
        depth_stencil.stencilTestEnable = VK_FALSE;

        // Colour attachments and blending.
        std::array<VkFormat, max_color_attachments> color_formats{};
        std::array<VkPipelineColorBlendAttachmentState, max_color_attachments> blend_attachments{};
        const std::uint32_t color_count = static_cast<std::uint32_t>(desc.color_formats.size());
        for (std::uint32_t i = 0; i < color_count; ++i)
        {
            color_formats[i] = to_vk_format(desc.color_formats[i]);
            blend_attachments[i] = blend_attachment(desc.color_blend.empty() ? blend_opaque() : desc.color_blend[i]);
        }

        VkPipelineColorBlendStateCreateInfo color_blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        color_blend.logicOpEnable = VK_FALSE;
        color_blend.attachmentCount = color_count;
        color_blend.pAttachments = blend_attachments.data();

        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = color_count;
        rendering.pColorAttachmentFormats = color_formats.data();
        rendering.depthAttachmentFormat = to_vk_format(depth_format);
        rendering.stencilAttachmentFormat = is_stencil_format(depth_format) ? to_vk_format(depth_format)
                                                                            : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = static_cast<std::uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport_state;
        info.pRasterizationState = &rasterizer;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth_stencil;
        info.pColorBlendState = &color_blend;
        info.pDynamicState = &dynamic;
        info.layout = dev->pipeline_layout;
        info.renderPass = VK_NULL_HANDLE;
        info.subpass = 0;

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("create_graphics_pipeline: vkCreateGraphicsPipelines failed ({})",
                                               result_string(result));
            return 0;
        }

        return register_pipeline(*dev, device, pipeline_type::graphics, pipeline, desc.debug_name);
    }

    resource_id create_compute_pipeline(resource_id device, const compute_pipeline_desc &desc)
    {
        device_state *dev = find_device(device);
        if (!dev)
            return 0;

        const shader_state *cs = shader_for(device, desc.compute_shader, shader_stage::compute);
        if (!cs)
            return 0;

        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = stage_info(*cs);
        info.layout = dev->pipeline_layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("create_compute_pipeline: vkCreateComputePipelines failed ({})",
                                               result_string(result));
            return 0;
        }

        return register_pipeline(*dev, device, pipeline_type::compute, pipeline, desc.debug_name);
    }

    void destroy_pipeline(resource_id id) noexcept
    {
        pipeline_state *p = find(reg().pipelines, id);
        if (!p)
            return;
        if (device_state *dev = find_device(p->owner))
        {
            const VkDevice d = dev->device;
            const VkPipeline pipeline = p->pipeline;
            defer_release(*dev, [d, pipeline] { vkDestroyPipeline(d, pipeline, nullptr); });
        }
        reg().pipelines.erase(id);
    }

    bool is_pipeline_valid(resource_id id) noexcept
    {
        return find(reg().pipelines, id) != nullptr;
    }

    pipeline_type get_pipeline_type(resource_id id) noexcept
    {
        const pipeline_state *p = find(reg().pipelines, id);
        return p ? p->type : pipeline_type::graphics;
    }

} // namespace catalyst::rendering::detail
