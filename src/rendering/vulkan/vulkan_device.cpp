/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Device layer of the Vulkan backend: instance and adapter selection, logical device and queue creation, the
 * shared pipeline layout, fence-based submission tracking with deferred resource release, the synchronous "immediate"
 * command buffer used for uploads, and the `destroy_device` cascade over every owned resource.
 */

#include "vulkan_backend.hpp"

#include "../detail_sync.hpp"
#include "vulkan_convert.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace catalyst::rendering::detail::vulkan
{

    // -------------------------------------------------------------------------
    // Diagnostics
    // -------------------------------------------------------------------------

    const char *result_string(VkResult result) noexcept
    {
        switch (result)
        {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT:    return "VK_ERROR_VALIDATION_FAILED_EXT";
        case VK_ERROR_INVALID_SHADER_NV:        return "VK_ERROR_INVALID_SHADER_NV";
        case VK_ERROR_UNKNOWN:                  return "VK_ERROR_UNKNOWN";
        default:                                return "VkResult(other)";
        }
    }

    void set_debug_name(device_state &dev, VkObjectType type, std::uint64_t handle, const char *name) noexcept
    {
        if (!dev.set_object_name || !name || !*name || handle == 0)
            return;
        VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        info.objectType = type;
        info.objectHandle = handle;
        info.pObjectName = name;
        dev.set_object_name(dev.device, &info);
    }

    format resolve_format(const device_state &dev, format f) noexcept
    {
        if (f == format::d24_unorm_s8_uint)
            return dev.depth_stencil_format;
        return f;
    }

    namespace
    {
        // ---------------------------------------------------------------------
        // Instance
        // ---------------------------------------------------------------------

        VKAPI_ATTR VkBool32 VKAPI_CALL debug_messenger_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                                VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                                                const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                                void * /*user*/)
        {
            // The layer's own severity picks the level, so a caller can filter validation chatter
            // out with `minimum_level` instead of by not enabling validation at all. Note this runs
            // on whichever thread tripped the layer, not on the one that created the device.
            const char *message = data && data->pMessage ? data->pMessage : "";
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                logging::error<detail::render_log>("validation: {}", message);
            else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                logging::warn<detail::render_log>("validation: {}", message);
            else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
                logging::info<detail::render_log>("validation: {}", message);
            else
                logging::debug<detail::render_log>("validation: {}", message);
            return VK_FALSE;
        }

        bool has_instance_layer(const char *name)
        {
            std::uint32_t count = 0;
            if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS)
                return false;
            std::vector<VkLayerProperties> layers(count);
            if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
                return false;
            return std::any_of(layers.begin(), layers.end(),
                               [name](const VkLayerProperties &l) { return std::strcmp(l.layerName, name) == 0; });
        }

        bool has_instance_extension(const char *name)
        {
            std::uint32_t count = 0;
            if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS)
                return false;
            std::vector<VkExtensionProperties> extensions(count);
            if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS)
                return false;
            return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties &e) {
                return std::strcmp(e.extensionName, name) == 0;
            });
        }

        bool has_device_extension(VkPhysicalDevice pd, const char *name)
        {
            std::uint32_t count = 0;
            if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr) != VK_SUCCESS)
                return false;
            std::vector<VkExtensionProperties> extensions(count);
            if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, extensions.data()) != VK_SUCCESS)
                return false;
            return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties &e) {
                return std::strcmp(e.extensionName, name) == 0;
            });
        }

        bool create_instance(device_state &dev)
        {
            std::uint32_t loader_version = VK_API_VERSION_1_0;
            vkEnumerateInstanceVersion(&loader_version);
            if (loader_version < VK_API_VERSION_1_3)
            {
                logging::error<detail::render_log>(
                    "create_device: the Vulkan loader only supports {}.{}, 1.3 is required",
                    VK_API_VERSION_MAJOR(loader_version), VK_API_VERSION_MINOR(loader_version));
                return false;
            }

            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.pApplicationName = dev.application_name.c_str();
            app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
            app.pEngineName = "Catalyst";
            app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
            app.apiVersion = VK_API_VERSION_1_3;

            std::vector<const char *> layers;
            std::vector<const char *> extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif

            if (dev.desc.enable_validation)
            {
                if (has_instance_layer("VK_LAYER_KHRONOS_validation"))
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                else
                    logging::error<detail::render_log>(
                        "create_device: validation requested but VK_LAYER_KHRONOS_validation is not installed");

                if (has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
                {
                    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                    dev.debug_utils = true;
                }
            }

            for (const char *ext : extensions)
            {
                if (!has_instance_extension(ext))
                {
                    logging::error<detail::render_log>("create_device: required instance extension {} is unavailable",
                                                       ext);
                    return false;
                }
            }

            VkDebugUtilsMessengerCreateInfoEXT messenger_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messenger_info.pfnUserCallback = debug_messenger_callback;

            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &app;
            info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
            info.ppEnabledLayerNames = layers.data();
            info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
            info.ppEnabledExtensionNames = extensions.data();
            if (dev.debug_utils)
                info.pNext = &messenger_info; // Also covers vkCreateInstance / vkDestroyInstance.

            const VkResult result = vkCreateInstance(&info, nullptr, &dev.instance);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("create_device: vkCreateInstance failed ({})",
                                                   result_string(result));
                return false;
            }

            if (dev.debug_utils)
            {
                auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(dev.instance, "vkCreateDebugUtilsMessengerEXT"));
                if (create)
                    create(dev.instance, &messenger_info, nullptr, &dev.messenger);
            }
            return true;
        }

        // ---------------------------------------------------------------------
        // Adapter selection
        // ---------------------------------------------------------------------

        struct adapter_families
        {
            /** Graphics + compute + present. Required; everything else falls back to it. */
            std::uint32_t graphics = 0;
            std::uint32_t compute = 0;
            std::uint32_t copy = 0;
            bool compute_dedicated = false;
            bool copy_dedicated = false;
        };

        struct adapter_candidate
        {
            VkPhysicalDevice physical_device = VK_NULL_HANDLE;
            adapter_families families{};
            int score = -1;
            VkPhysicalDeviceProperties properties{};
        };

        /**
         * Picks one family per queue kind.
         *
         * The rule for the two optional ones is the usual Vulkan idiom: a family is a *dedicated*
         * compute engine when it can compute and cannot draw, and a dedicated copy engine when it
         * can transfer and can do neither of the other two. Asking it that way rather than taking
         * the first family that merely advertises the capability is what keeps async compute from
         * silently resolving back onto the graphics engine on adapters that expose one family with
         * every bit set.
         *
         * Anything not found aliases graphics, which is always correct - a graphics queue accepts
         * every kind of work - and is reported through `queue_info::dedicated` so a caller can tell.
         */
        bool find_queue_families(VkPhysicalDevice pd, adapter_families &out)
        {
            std::uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());

            constexpr VkQueueFlags graphics_required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            bool found_graphics = false;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if ((families[i].queueFlags & graphics_required) != graphics_required)
                    continue;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
                if (!vkGetPhysicalDeviceWin32PresentationSupportKHR(pd, i))
                    continue;
#endif
                out.graphics = i;
                found_graphics = true;
                break;
            }
            if (!found_graphics)
                return false;

            out.compute = out.graphics;
            out.copy = out.graphics;
            out.compute_dedicated = false;
            out.copy_dedicated = false;

            for (std::uint32_t i = 0; i < count; ++i)
            {
                const VkQueueFlags flags = families[i].queueFlags;
                const bool draws = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
                const bool computes = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
                const bool transfers = (flags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT |
                                                 VK_QUEUE_COMPUTE_BIT)) != 0;

                if (!out.compute_dedicated && computes && !draws)
                {
                    out.compute = i;
                    out.compute_dedicated = true;
                }
                if (!out.copy_dedicated && transfers && !draws && !computes)
                {
                    out.copy = i;
                    out.copy_dedicated = true;
                }
            }
            return true;
        }

        bool has_required_features(VkPhysicalDevice pd)
        {
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            f12.pNext = &f13;
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &f12;
            vkGetPhysicalDeviceFeatures2(pd, &f2);
            // Timeline semaphores are what `timeline_point` is: core since 1.2, and universally
            // implemented on anything that reports 1.3, but a required feature is worth checking
            // rather than assuming.
            return f13.dynamicRendering && f12.descriptorBindingPartiallyBound && f12.timelineSemaphore;
        }

        bool pick_adapter(const device_state &dev, adapter_candidate &out)
        {
            std::uint32_t count = 0;
            if (vkEnumeratePhysicalDevices(dev.instance, &count, nullptr) != VK_SUCCESS || count == 0)
            {
                logging::error<detail::render_log>("create_device: no Vulkan physical devices found");
                return false;
            }
            std::vector<VkPhysicalDevice> devices(count);
            vkEnumeratePhysicalDevices(dev.instance, &count, devices.data());

            adapter_candidate best;
            for (VkPhysicalDevice pd : devices)
            {
                adapter_candidate c;
                c.physical_device = pd;
                vkGetPhysicalDeviceProperties(pd, &c.properties);

                if (c.properties.apiVersion < VK_API_VERSION_1_3)
                    continue;
                if (!find_queue_families(pd, c.families))
                    continue;
                if (!has_device_extension(pd, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                    continue;
                if (!has_required_features(pd))
                    continue;

                switch (c.properties.deviceType)
                {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   c.score = dev.desc.prefer_discrete_adapter ? 3 : 1; break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: c.score = 2; break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    c.score = 1; break;
                default:                                     c.score = 0; break;
                }

                if (c.score > best.score)
                    best = c;
            }

            if (best.score < 0)
            {
                logging::error<detail::render_log>(
                    "create_device: no adapter supports Vulkan 1.3 with dynamic rendering, partially bound "
                    "descriptors, timeline semaphores and a graphics+compute queue that can present");
                return false;
            }
            out = best;
            return true;
        }

        // ---------------------------------------------------------------------
        // Logical device
        // ---------------------------------------------------------------------

        bool create_logical_device(device_state &dev, const adapter_candidate &adapter)
        {
            dev.physical_device = adapter.physical_device;
            queue_for(dev, queue_kind::graphics).family = adapter.families.graphics;
            queue_for(dev, queue_kind::compute).family = adapter.families.compute;
            queue_for(dev, queue_kind::copy).family = adapter.families.copy;
            queue_for(dev, queue_kind::graphics).dedicated = true;
            queue_for(dev, queue_kind::compute).dedicated = adapter.families.compute_dedicated;
            queue_for(dev, queue_kind::copy).dedicated = adapter.families.copy_dedicated;
            dev.properties = adapter.properties;
            dev.adapter_name = adapter.properties.deviceName;
            vkGetPhysicalDeviceMemoryProperties(dev.physical_device, &dev.memory_properties);

            dev.dedicated_video_memory = 0;
            for (std::uint32_t i = 0; i < dev.memory_properties.memoryHeapCount; ++i)
            {
                if (dev.memory_properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    dev.dedicated_video_memory += dev.memory_properties.memoryHeaps[i].size;
            }

            // Integrated and software adapters render out of system RAM, so a device-local type that is also
            // host-visible covers the whole heap and GPU-only buffers can be written without a staging copy.
            // Confirm the type actually exists rather than trusting deviceType alone.
            const bool uma_adapter = dev.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                                     dev.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            dev.unified_memory = false;
            if (uma_adapter)
            {
                constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                for (std::uint32_t i = 0; i < dev.memory_properties.memoryTypeCount; ++i)
                {
                    if ((dev.memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted)
                    {
                        dev.unified_memory = true;
                        break;
                    }
                }
            }

            VkFormatProperties d24s8{};
            vkGetPhysicalDeviceFormatProperties(dev.physical_device, VK_FORMAT_D24_UNORM_S8_UINT, &d24s8);
            dev.depth_stencil_format = (d24s8.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                                           ? format::d24_unorm_s8_uint
                                           : format::d32_float_s8_uint;

            VkPhysicalDeviceFeatures supported{};
            vkGetPhysicalDeviceFeatures(dev.physical_device, &supported);
            dev.features.sampler_anisotropy = supported.samplerAnisotropy == VK_TRUE;
            dev.features.fill_mode_non_solid = supported.fillModeNonSolid == VK_TRUE;
            dev.features.depth_clamp = supported.depthClamp == VK_TRUE;

            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            f13.dynamicRendering = VK_TRUE;
            VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            f12.pNext = &f13;
            f12.descriptorBindingPartiallyBound = VK_TRUE;
            f12.timelineSemaphore = VK_TRUE;
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &f12;
            f2.features.samplerAnisotropy = supported.samplerAnisotropy;
            f2.features.fillModeNonSolid = supported.fillModeNonSolid;
            f2.features.depthClamp = supported.depthClamp;

            // One VkDeviceQueueCreateInfo per *distinct* family: naming a family twice is invalid,
            // and on an adapter with no dedicated engines all three kinds collapse onto one entry.
            const float priority = 1.0f;
            std::vector<VkDeviceQueueCreateInfo> queue_infos;
            for (const queue_state &q : dev.queues)
            {
                const bool already = std::any_of(queue_infos.begin(), queue_infos.end(),
                                                 [&q](const VkDeviceQueueCreateInfo &existing) {
                                                     return existing.queueFamilyIndex == q.family;
                                                 });
                if (already)
                    continue;

                VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queue_info.queueFamilyIndex = q.family;
                queue_info.queueCount = 1;
                queue_info.pQueuePriorities = &priority;
                queue_infos.push_back(queue_info);
            }

            // The same distinct set, kept for VK_SHARING_MODE_CONCURRENT: a buffer or image that
            // may be touched by more than one family has to say so at creation, and this backend
            // does not track ownership transfers.
            dev.families.clear();
            for (const VkDeviceQueueCreateInfo &q : queue_infos)
                dev.families.push_back(q.queueFamilyIndex);

            const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

            VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            info.pNext = &f2;
            info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_infos.size());
            info.pQueueCreateInfos = queue_infos.data();
            info.enabledExtensionCount = 1;
            info.ppEnabledExtensionNames = extensions;

            const VkResult result = vkCreateDevice(dev.physical_device, &info, nullptr, &dev.device);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("create_device: vkCreateDevice failed ({})", result_string(result));
                return false;
            }

            // A timeline semaphore per kind, even where two kinds share a VkQueue: a point then
            // means the same thing on every adapter, and nothing downstream has to special-case an
            // aliased queue.
            VkSemaphoreTypeCreateInfo timeline_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            timeline_type.initialValue = 0;
            VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            semaphore_info.pNext = &timeline_type;

            for (std::size_t i = 0; i < queue_kind_count; ++i)
            {
                queue_state &q = dev.queues[i];
                vkGetDeviceQueue(dev.device, q.family, 0, &q.queue);

                const VkResult semaphore_result =
                    vkCreateSemaphore(dev.device, &semaphore_info, nullptr, &q.timeline);
                if (semaphore_result != VK_SUCCESS)
                {
                    logging::error<detail::render_log>("create_device: vkCreateSemaphore (timeline) failed ({})",
                                                       result_string(semaphore_result));
                    return false;
                }
            }

            logging::info<detail::render_log>(
                "queues: graphics family {}, compute family {}{}, copy family {}{}",
                queue_for(dev, queue_kind::graphics).family, queue_for(dev, queue_kind::compute).family,
                queue_for(dev, queue_kind::compute).dedicated ? "" : " (aliased)",
                queue_for(dev, queue_kind::copy).family,
                queue_for(dev, queue_kind::copy).dedicated ? "" : " (aliased)");

            if (dev.debug_utils)
            {
                dev.set_object_name = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                    vkGetDeviceProcAddr(dev.device, "vkSetDebugUtilsObjectNameEXT"));
            }
            return true;
        }

        // ---------------------------------------------------------------------
        // Shared pipeline layout
        // ---------------------------------------------------------------------

        VkDescriptorType descriptor_type_for_set(std::uint32_t set) noexcept
        {
            switch (set)
            {
            case set_uniform_buffers: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case set_storage_buffers: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case set_textures:        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            default:                  return VK_DESCRIPTOR_TYPE_SAMPLER;
            }
        }

        std::uint32_t slots_for_set(std::uint32_t set) noexcept
        {
            switch (set)
            {
            case set_uniform_buffers: return max_uniform_buffer_slots;
            case set_storage_buffers: return max_storage_buffer_slots;
            case set_textures:        return max_texture_slots;
            default:                  return max_sampler_slots;
            }
        }

        bool create_pipeline_layout(device_state &dev)
        {
            for (std::uint32_t set = 0; set < descriptor_set_count; ++set)
            {
                const std::uint32_t slots = slots_for_set(set);
                std::vector<VkDescriptorSetLayoutBinding> bindings(slots);
                std::vector<VkDescriptorBindingFlags> flags(slots, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
                for (std::uint32_t i = 0; i < slots; ++i)
                {
                    bindings[i].binding = i;
                    bindings[i].descriptorType = descriptor_type_for_set(set);
                    bindings[i].descriptorCount = 1;
                    bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
                }

                VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
                    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
                flags_info.bindingCount = slots;
                flags_info.pBindingFlags = flags.data();

                VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                info.pNext = &flags_info;
                info.bindingCount = slots;
                info.pBindings = bindings.data();

                const VkResult result = vkCreateDescriptorSetLayout(dev.device, &info, nullptr, &dev.set_layouts[set]);
                if (result != VK_SUCCESS)
                {
                    logging::error<detail::render_log>("create_device: vkCreateDescriptorSetLayout failed ({})",
                                                       result_string(result));
                    return false;
                }
            }

            VkPushConstantRange push_range{};
            push_range.stageFlags = VK_SHADER_STAGE_ALL;
            push_range.offset = 0;
            push_range.size = max_push_constant_bytes;

            VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            info.setLayoutCount = descriptor_set_count;
            info.pSetLayouts = dev.set_layouts.data();
            info.pushConstantRangeCount = 1;
            info.pPushConstantRanges = &push_range;

            const VkResult result = vkCreatePipelineLayout(dev.device, &info, nullptr, &dev.pipeline_layout);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("create_device: vkCreatePipelineLayout failed ({})",
                                                   result_string(result));
                return false;
            }
            return true;
        }

        // ---------------------------------------------------------------------
        // Immediate command buffer
        // ---------------------------------------------------------------------

        bool create_immediate_objects(device_state &dev)
        {
            VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_info.queueFamilyIndex = queue_for(dev, queue_kind::graphics).family;
            if (vkCreateCommandPool(dev.device, &pool_info, nullptr, &dev.immediate_pool) != VK_SUCCESS)
                return false;

            VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            alloc.commandPool = dev.immediate_pool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            // No fence: the immediate buffer rides the graphics timeline like everything else.
            return vkAllocateCommandBuffers(dev.device, &alloc, &dev.immediate_cmd) == VK_SUCCESS;
        }

        // ---------------------------------------------------------------------
        // Teardown
        // ---------------------------------------------------------------------

        template <typename Map, typename Release>
        void release_owned(Map &map, resource_id device, Release &&release)
        {
            for (auto it = map.begin(); it != map.end();)
            {
                if (it->second.owner == device)
                {
                    release(it->second);
                    it = map.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        void destroy_device_objects(device_state &dev) noexcept
        {
            if (dev.device)
            {
                vkDeviceWaitIdle(dev.device);

                for (deferred_release &g : dev.garbage)
                    g.release();
                dev.garbage.clear();

                for (queue_state &q : dev.queues)
                {
                    if (q.timeline)
                        vkDestroySemaphore(dev.device, q.timeline, nullptr);
                    q.timeline = VK_NULL_HANDLE;
                }

                release_staging(dev);
                release_staging_ring(dev);
                release_transfer_context(dev);

                if (dev.immediate_pool)
                    vkDestroyCommandPool(dev.device, dev.immediate_pool, nullptr);

                if (dev.pipeline_layout)
                    vkDestroyPipelineLayout(dev.device, dev.pipeline_layout, nullptr);
                for (VkDescriptorSetLayout layout : dev.set_layouts)
                {
                    if (layout)
                        vkDestroyDescriptorSetLayout(dev.device, layout, nullptr);
                }

                vkDestroyDevice(dev.device, nullptr);
            }

            if (dev.instance)
            {
                if (dev.messenger)
                {
                    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                        vkGetInstanceProcAddr(dev.instance, "vkDestroyDebugUtilsMessengerEXT"));
                    if (destroy)
                        destroy(dev.instance, dev.messenger, nullptr);
                }
                vkDestroyInstance(dev.instance, nullptr);
            }

            dev.device = VK_NULL_HANDLE;
            dev.instance = VK_NULL_HANDLE;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Submission tracking
    //
    // One timeline semaphore per queue replaces the fence-per-submission scheme this backend used
    // to run. The bookkeeping is the same shape it always was - a monotonic counter, and a record
    // of which value each in-flight resource is waiting behind - except that the counter is now the
    // GPU's own, so it can be waited on by the CPU, waited on by another queue, and handed to the
    // caller as a `timeline_point` instead of being hidden behind a `bool`.
    //
    // What that removed: `VkFence` creation, the free-fence pool, the `in_flight` vector, and the
    // rule that fences signal in submission order (which is true, and which made the old
    // `end_immediate` wait for every earlier submission as a side effect nobody asked for).
    // -------------------------------------------------------------------------

    queue_state &queue_for(device_state &dev, queue_kind kind) noexcept
    {
        return dev.queues[static_cast<std::size_t>(kind)];
    }

    const queue_state &queue_for(const device_state &dev, queue_kind kind) noexcept
    {
        return dev.queues[static_cast<std::size_t>(kind)];
    }

    error to_error(device_state &dev, VkResult result, const char *operation) noexcept
    {
        error e;
        e.backend_result = static_cast<std::int32_t>(result);
        e.backend_result_name = result_string(result);
        e.operation = operation;

        switch (result)
        {
        case VK_ERROR_DEVICE_LOST:
            // Latched, and latched here rather than at each call site, because every other entry
            // point needs to start failing fast from this moment and none of them should have to
            // remember to check.
            if (!dev.lost)
            {
                dev.lost = true;
                logging::error<detail::render_log>("device lost during {}; every handle from it is now dead",
                                                   operation ? operation : "an unnamed operation");
            }
            e.code = error_code::device_lost;
            break;
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: e.code = error_code::out_of_device_memory; break;
        case VK_ERROR_OUT_OF_HOST_MEMORY:   e.code = error_code::out_of_host_memory; break;
        case VK_ERROR_SURFACE_LOST_KHR:     e.code = error_code::surface_lost; break;
        case VK_ERROR_OUT_OF_DATE_KHR:      e.code = error_code::swapchain_out_of_date; break;
        case VK_TIMEOUT:                    e.code = error_code::timeout; break;
        case VK_NOT_READY:                  e.code = error_code::not_ready; break;
        case VK_ERROR_FORMAT_NOT_SUPPORTED: e.code = error_code::unsupported_format; break;
        default:                            e.code = error_code::platform_error; break;
        }
        return e;
    }

    void refresh_completed(device_state &dev) noexcept
    {
        if (!dev.device || dev.lost)
            return;
        for (queue_state &q : dev.queues)
        {
            if (!q.timeline)
                continue;
            std::uint64_t value = 0;
            const VkResult result = vkGetSemaphoreCounterValue(dev.device, q.timeline, &value);
            if (result != VK_SUCCESS)
            {
                to_error(dev, result, "vkGetSemaphoreCounterValue");
                return;
            }
            // Never let it move backwards; a torn read would retire garbage still in use.
            if (value > q.completed)
                q.completed = value;
        }
    }

    void collect_garbage(device_state &dev) noexcept
    {
        refresh_completed(dev);

        // A lost device never signals again, so waiting for its timelines would leak everything.
        // Releasing now is safe: the driver has already abandoned the work that was reading it.
        const bool lost = dev.lost;

        std::erase_if(dev.garbage, [&](deferred_release &g) {
            if (!lost)
            {
                for (std::size_t i = 0; i < queue_kind_count; ++i)
                {
                    if (g.points[i] > dev.queues[i].completed)
                        return false;
                }
            }
            g.release();
            return true;
        });
    }

    std::expected<std::uint64_t, error> submit_batch(device_state &dev, const submit_batch_info &batch) noexcept
    {
        if (dev.lost)
            return std::unexpected(make_error(error_code::device_lost, "submit"));

        queue_state &q = queue_for(dev, batch.kind);
        const std::uint64_t signalled = q.last_submitted + 1;

        // Binary semaphores first, then timeline ones. Vulkan reads one parallel array of values
        // covering both, and simply ignores the entries belonging to binary semaphores - which is
        // why the swapchain's acquire and present semaphores can ride along in the same submission
        // as a cross-queue dependency without either knowing about the other.
        std::vector<VkSemaphore> waits{batch.binary_waits.begin(), batch.binary_waits.end()};
        std::vector<VkPipelineStageFlags> stages{batch.wait_stages.begin(), batch.wait_stages.end()};
        std::vector<std::uint64_t> wait_values(waits.size(), 0);

        const auto add_timeline_wait = [&](queue_kind source_kind, std::uint64_t value) {
            if (value == 0)
                return;
            const queue_state &source = queue_for(dev, source_kind);
            if (!source.timeline || value <= source.completed)
                return; // Already done: waiting on it would cost a semaphore slot for nothing.
            waits.push_back(source.timeline);
            stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            wait_values.push_back(value);
        };

        for (const timeline_point &point : batch.timeline_waits)
        {
            if (!point.valid())
                continue; // "No dependency", so the caller does not need a branch.
            add_timeline_wait(point.queue(), point.value());
        }

        // Read-after-write against the transfer queue, added for every submission that is not
        // itself a transfer. This is what lets `write_buffer` be asynchronous without making the
        // caller order it: the data is uploaded on the copy queue, and anything submitted
        // afterwards waits for it on the GPU. Free once the copy has completed, which by the next
        // frame it usually has.
        if (batch.kind != queue_kind::copy)
            add_timeline_wait(queue_kind::copy, dev.last_transfer);

        std::vector<VkSemaphore> signals{batch.binary_signals.begin(), batch.binary_signals.end()};
        std::vector<std::uint64_t> signal_values(signals.size(), 0);
        signals.push_back(q.timeline);
        signal_values.push_back(signalled);

        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = static_cast<std::uint32_t>(wait_values.size());
        timeline.pWaitSemaphoreValues = wait_values.data();
        timeline.signalSemaphoreValueCount = static_cast<std::uint32_t>(signal_values.size());
        timeline.pSignalSemaphoreValues = signal_values.data();

        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.pNext = &timeline;
        info.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size());
        info.pWaitSemaphores = waits.data();
        info.pWaitDstStageMask = stages.data();
        info.commandBufferCount = static_cast<std::uint32_t>(batch.commands.size());
        info.pCommandBuffers = batch.commands.data();
        info.signalSemaphoreCount = static_cast<std::uint32_t>(signals.size());
        info.pSignalSemaphores = signals.data();

        const VkResult result = vkQueueSubmit(q.queue, 1, &info, VK_NULL_HANDLE);
        if (result != VK_SUCCESS)
            return std::unexpected(to_error(dev, result, "vkQueueSubmit"));

        q.last_submitted = signalled;
        collect_garbage(dev);
        return signalled;
    }

    std::expected<void, error> wait_timeline(device_state &dev, queue_kind kind, std::uint64_t value,
                                             std::chrono::nanoseconds timeout) noexcept
    {
        if (value == 0)
            return {};
        if (dev.lost)
            return std::unexpected(make_error(error_code::device_lost, "wait"));

        queue_state &q = queue_for(dev, kind);
        if (value <= q.completed)
            return {};
        if (!q.timeline || value > q.last_submitted)
        {
            // Nothing will ever signal this: it names work that was never submitted.
            return std::unexpected(make_error(error_code::invalid_argument, "wait"));
        }

        // Vulkan takes an unsigned nanosecond count with UINT64_MAX meaning "no deadline", which is
        // exactly what a maximal or negative std::chrono duration means here.
        std::uint64_t ns = UINT64_MAX;
        if (timeout >= std::chrono::nanoseconds::zero() && timeout < std::chrono::nanoseconds::max())
            ns = static_cast<std::uint64_t>(timeout.count());

        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1;
        info.pSemaphores = &q.timeline;
        info.pValues = &value;

        const VkResult result = vkWaitSemaphores(dev.device, &info, ns);
        if (result == VK_SUCCESS)
        {
            if (value > q.completed)
                q.completed = value;
            collect_garbage(dev);
            return {};
        }
        return std::unexpected(to_error(dev, result, "vkWaitSemaphores"));
    }

    void wait_all(device_state &dev) noexcept
    {
        if (!dev.device)
            return;
        if (!dev.lost)
        {
            const VkResult result = vkDeviceWaitIdle(dev.device);
            if (result != VK_SUCCESS)
                to_error(dev, result, "vkDeviceWaitIdle");
        }
        // Everything submitted has completed, whether the driver told us the counter values or the
        // device died taking the work with it. Either way nothing is still reading a resource.
        for (queue_state &q : dev.queues)
            q.completed = q.last_submitted;
        collect_garbage(dev);
    }

    void defer_release(device_state &dev, std::function<void()> release)
    {
        refresh_completed(dev);

        deferred_release entry;
        bool outstanding = false;
        for (std::size_t i = 0; i < queue_kind_count; ++i)
        {
            entry.points[i] = dev.queues[i].last_submitted;
            if (!dev.lost && entry.points[i] > dev.queues[i].completed)
                outstanding = true;
        }

        if (!outstanding)
        {
            release();
            return;
        }
        entry.release = std::move(release);
        dev.garbage.push_back(std::move(entry));
    }

    VkCommandBuffer begin_immediate(device_state &dev) noexcept
    {
        if (dev.lost)
            return VK_NULL_HANDLE;
        if (vkResetCommandPool(dev.device, dev.immediate_pool, 0) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(dev.immediate_cmd, &begin) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        // Order against whatever earlier submissions are still touching the resources we are about to write.
        full_barrier(dev.immediate_cmd);
        return dev.immediate_cmd;
    }

    bool end_immediate(device_state &dev) noexcept
    {
        full_barrier(dev.immediate_cmd);
        if (vkEndCommandBuffer(dev.immediate_cmd) != VK_SUCCESS)
            return false;

        // Still a blocking round trip - that is Tier 4's problem, not this one. What changed is
        // that it now waits for exactly its own submission instead of for every fence issued
        // before it, which is what waiting on an ordered fence list used to mean.
        const VkCommandBuffer commands[] = {dev.immediate_cmd};
        const auto value = submit_batch(dev, {.kind = queue_kind::graphics, .commands = commands});
        if (!value)
            return false;

        return wait_timeline(dev, queue_kind::graphics, *value, std::chrono::nanoseconds::max()).has_value();
    }

    void full_barrier(VkCommandBuffer cmd) noexcept
    {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
    }

} // namespace catalyst::rendering::detail::vulkan

// -----------------------------------------------------------------------------
// Backend contract: device
// -----------------------------------------------------------------------------

namespace catalyst::rendering::detail
{

    using namespace vulkan;

    resource_id create_device(const device_desc &desc)
    {
        device_state dev;
        dev.desc = desc;
        dev.application_name = copy_name(desc.application_name);
        if (dev.application_name.empty())
            dev.application_name = "Catalyst";
        dev.desc.application_name = nullptr;

        adapter_candidate adapter;
        const bool ok = create_instance(dev) && pick_adapter(dev, adapter) && create_logical_device(dev, adapter) &&
                        create_pipeline_layout(dev) && create_immediate_objects(dev) &&
                        create_staging_ring(dev, dev.desc.staging_ring_bytes);
        if (!ok)
        {
            destroy_device_objects(dev);
            return 0;
        }

        const resource_id id = allocate_id();
        auto [it, inserted] = reg().devices.emplace(id, std::move(dev));
        device_state &stored = it->second;
        stored.desc.application_name = stored.application_name.c_str();
        set_debug_name(stored, VK_OBJECT_TYPE_DEVICE, reinterpret_cast<std::uint64_t>(stored.device),
                       stored.application_name);
        return id;
    }

    void destroy_device(resource_id id) noexcept
    {
        device_state *dev = find_device(id);
        if (!dev)
            return;

        wait_all(*dev);

        registry &r = reg();
        // Downloads and batches first: both hold command buffers and mapped memory that the pools
        // and the ring below are about to take away.
        release_owned(r.downloads, id, [&](download_state &d) { release_download_objects(*dev, d); });
        release_owned(r.transfer_batches, id, [](transfer_batch_state &) {});
        release_owned(r.command_lists, id, [&](command_list_state &cl) { release_command_list_objects(*dev, cl); });
        release_owned(r.command_pools, id, [&](command_pool_state &pool) { release_command_pool_objects(*dev, pool); });
        release_owned(r.swapchains, id, [&](swapchain_state &sc) { release_swapchain_objects(*dev, sc); });
        release_owned(r.pipelines, id, [&](pipeline_state &p) { release_pipeline_objects(*dev, p); });
        release_owned(r.samplers, id, [&](sampler_state &s) { release_sampler_objects(*dev, s); });
        release_owned(r.textures, id, [&](texture_state &t) { release_texture_objects(*dev, t); });
        release_owned(r.shaders, id, [&](shader_state &s) { release_shader_objects(*dev, s); });
        release_owned(r.buffers, id, [&](buffer_state &b) { release_buffer_objects(*dev, b); });

        destroy_device_objects(*dev);
        r.devices.erase(id);
    }

    bool is_device_valid(resource_id id) noexcept
    {
        return find_device(id) != nullptr;
    }

    device_info get_device_info(resource_id id) noexcept
    {
        device_info info;
        const device_state *dev = find_device(id);
        if (!dev)
            return info;
        info.backend = backend_kind::vulkan;
        info.adapter_name = dev->adapter_name.c_str();
        info.dedicated_video_memory_bytes = dev->dedicated_video_memory;
        info.max_memory_allocation_count = dev->properties.limits.maxMemoryAllocationCount;
        info.unified_memory = dev->unified_memory;
        return info;
    }

    bool is_device_lost(resource_id id) noexcept
    {
        const device_state *dev = find_device(id);
        return dev != nullptr && dev->lost;
    }

    void wait_idle(resource_id id) noexcept
    {
        // Called with no lock held - see detail_backend.hpp. `vkDeviceWaitIdle` is where teardown
        // and resize block, and holding the module lock across it would stall every other thread
        // for the length of the GPU's queue.
        VkDevice device = VK_NULL_HANDLE;
        {
            const exclusive_guard guard;
            device_state *dev = find_device(id);
            if (!dev || !dev->device)
                return;
            if (dev->lost)
            {
                wait_all(*dev); // Does not touch the driver; just settles the bookkeeping.
                return;
            }
            device = dev->device;
        }

        const VkResult result = vkDeviceWaitIdle(device);

        const exclusive_guard guard;
        device_state *dev = find_device(id);
        if (!dev)
            return; // Destroyed while we waited, which the threading rules forbid but do not prevent.
        if (result != VK_SUCCESS)
            to_error(*dev, result, "vkDeviceWaitIdle");
        for (queue_state &q : dev->queues)
            q.completed = q.last_submitted;
        vulkan::collect_garbage(*dev);
    }

    void collect_garbage(resource_id id) noexcept
    {
        if (device_state *dev = find_device(id))
            vulkan::collect_garbage(*dev);
    }

    // -------------------------------------------------------------------------
    // Backend contract: queues and timelines
    // -------------------------------------------------------------------------

    queue_info get_queue_info(resource_id device, queue_kind kind) noexcept
    {
        queue_info info;
        info.kind = kind;
        const device_state *dev = find_device(device);
        if (!dev)
            return info;

        const queue_state &q = queue_for(*dev, kind);
        info.dedicated = q.dedicated;
        info.family_index = q.family;
        return info;
    }

    std::uint64_t queue_last_submitted(resource_id device, queue_kind kind) noexcept
    {
        const device_state *dev = find_device(device);
        return dev ? queue_for(*dev, kind).last_submitted : 0;
    }

    std::uint64_t queue_completed(resource_id device, queue_kind kind) noexcept
    {
        device_state *dev = find_device(device);
        if (!dev)
            return 0;
        // Reading the counter is a cheap driver call and the caller asked for the truth, not for
        // whatever the last submission happened to leave cached.
        refresh_completed(*dev);
        return queue_for(*dev, kind).completed;
    }

    std::expected<void, error> queue_wait(resource_id device, queue_kind kind, std::uint64_t value,
                                          std::chrono::nanoseconds timeout) noexcept
    {
        if (value == 0)
            return {};

        // Called with no lock held. The pattern is resolve, unlock, block, relock: `vkWaitSemaphores`
        // is thread-safe by specification, so the only thing that needs the lock is reading the
        // handles out of the registry and writing the result back into it.
        VkDevice vk_device = VK_NULL_HANDLE;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        {
            const shared_guard guard;
            device_state *dev = find_device(device);
            if (!dev)
                return std::unexpected(make_error(error_code::invalid_argument, "wait"));
            if (dev->lost)
                return std::unexpected(make_error(error_code::device_lost, "wait"));

            const queue_state &q = queue_for(*dev, kind);
            if (value <= q.completed)
                return {};
            if (!q.timeline || value > q.last_submitted)
            {
                // Nothing will ever signal this: it names work that was never submitted.
                return std::unexpected(make_error(error_code::invalid_argument, "wait"));
            }
            vk_device = dev->device;
            semaphore = q.timeline;
        }

        // Vulkan takes an unsigned nanosecond count with UINT64_MAX meaning "no deadline", which is
        // exactly what a maximal or negative std::chrono duration means here.
        std::uint64_t ns = UINT64_MAX;
        if (timeout >= std::chrono::nanoseconds::zero() && timeout < std::chrono::nanoseconds::max())
            ns = static_cast<std::uint64_t>(timeout.count());

        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1;
        info.pSemaphores = &semaphore;
        info.pValues = &value;

        const VkResult result = vkWaitSemaphores(vk_device, &info, ns);

        const exclusive_guard guard;
        device_state *dev = find_device(device);
        if (!dev)
            return std::unexpected(make_error(error_code::invalid_argument, "wait"));
        if (result != VK_SUCCESS)
            return std::unexpected(to_error(*dev, result, "vkWaitSemaphores"));

        queue_state &q = queue_for(*dev, kind);
        if (value > q.completed)
            q.completed = value;
        vulkan::collect_garbage(*dev);
        return {};
    }

} // namespace catalyst::rendering::detail
