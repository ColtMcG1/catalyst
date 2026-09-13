/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Swapchains of the Vulkan backend.
 * @details Two flavours share one record:
 *   - Windowed: a VkSurfaceKHR + VkSwapchainKHR over the platform window. Presentation synchronisation is hidden
 *     behind the public acquire → submit → present flow: `acquire_next_image` signals an acquire semaphore, the next
 *     `submit` on the device waits on it and signals the image's render-finished semaphore, and `present` waits on
 *     that (adding one empty submission when more work was queued in between, so "everything submitted before
 *     present finishes first" holds).
 *   - Window-less (no native handle): a ring of ordinary render-target textures handed out in order, with `present`
 *     a no-op. This is what head-less tools and the test-suite use.
 */

#include "vulkan_backend.hpp"
#include "vulkan_convert.hpp"

#include <algorithm>
#include <string>

namespace catalyst::rendering::detail::vulkan
{

    namespace
    {
        constexpr texture_usage back_buffer_usage =
            texture_usage::render_target | texture_usage::transfer_src | texture_usage::transfer_dst;

        texture_desc back_buffer_desc(const swapchain_state &sc) noexcept
        {
            texture_desc d;
            d.dimension = texture_dimension::texture_2d;
            d.extent = {sc.desc.extent.width, sc.desc.extent.height, 1};
            d.pixel_format = sc.desc.pixel_format;
            d.usage = back_buffer_usage;
            return d;
        }

        std::string image_name(const swapchain_state &sc, std::uint32_t index)
        {
            return sc.debug_name.empty() ? std::string{} : sc.debug_name + " image " + std::to_string(index);
        }

        void destroy_images(device_state &dev, swapchain_state &sc) noexcept
        {
            for (const resource_id image : sc.images)
                destroy_swapchain_texture(dev, image);
            sc.images.clear();
        }

        void destroy_sync(device_state &dev, swapchain_state &sc) noexcept
        {
            for (VkSemaphore s : sc.acquire_semaphores)
                vkDestroySemaphore(dev.device, s, nullptr);
            for (VkSemaphore s : sc.render_finished)
                vkDestroySemaphore(dev.device, s, nullptr);
            for (VkSemaphore s : sc.present_ready)
                vkDestroySemaphore(dev.device, s, nullptr);
            sc.acquire_semaphores.clear();
            sc.acquire_points.clear();
            sc.render_finished.clear();
            sc.present_ready.clear();
            sc.acquire_slot = 0;
        }

        bool create_semaphores(device_state &dev, std::vector<VkSemaphore> &out, std::size_t count) noexcept
        {
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            out.assign(count, VK_NULL_HANDLE);
            for (VkSemaphore &s : out)
            {
                if (vkCreateSemaphore(dev.device, &info, nullptr, &s) != VK_SUCCESS)
                    return false;
            }
            return true;
        }

        bool create_sync(device_state &dev, swapchain_state &sc, std::uint32_t image_count) noexcept
        {
            destroy_sync(dev, sc);
            // One more acquire semaphore than images so the slot being reused always belongs to a retired frame.
            if (!create_semaphores(dev, sc.acquire_semaphores, image_count + 1) ||
                !create_semaphores(dev, sc.render_finished, image_count) ||
                !create_semaphores(dev, sc.present_ready, image_count))
            {
                logging::error<detail::render_log>("create_swapchain: vkCreateSemaphore failed");
                return false;
            }
            sc.acquire_points.assign(image_count + 1, timeline_point{});
            sc.acquire_slot = 0;
            return true;
        }

        // ---------------------------------------------------------------------
        // Window-less swapchains
        // ---------------------------------------------------------------------

        bool create_offscreen_images(resource_id id, swapchain_state &sc)
        {
            const texture_desc base = back_buffer_desc(sc);
            for (std::uint32_t i = 0; i < sc.desc.image_count; ++i)
            {
                texture_desc d = base;
                const std::string name = image_name(sc, i);
                d.debug_name = name_or_null(name);
                const resource_id image = create_texture_internal(sc.owner, d, {}, id);
                if (!image)
                    return false;
                sc.images.push_back(image);
            }
            sc.next_image = 0;
            sc.acquired = false;
            return true;
        }

        // ---------------------------------------------------------------------
        // Windowed swapchains
        // ---------------------------------------------------------------------

        bool create_surface(device_state &dev, swapchain_state &sc) noexcept
        {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            if (sc.desc.window.kind != platform::native_handle_kind::win32_hwnd)
            {
                logging::error<detail::render_log>("create_swapchain: unsupported native window handle kind {}",
                                                   static_cast<unsigned>(sc.desc.window.kind));
                return false;
            }

            VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            info.hinstance = sc.desc.window.extra ? static_cast<HINSTANCE>(sc.desc.window.extra)
                                                  : GetModuleHandleW(nullptr);
            info.hwnd = static_cast<HWND>(sc.desc.window.handle);

            const VkResult result = vkCreateWin32SurfaceKHR(dev.instance, &info, nullptr, &sc.surface);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("create_swapchain: vkCreateWin32SurfaceKHR failed ({})",
                                                   result_string(result));
                return false;
            }
#else
            logging::error<detail::render_log>(
                "create_swapchain: windowed swapchains are not implemented for this platform yet");
            return false;
#endif
            VkBool32 supported = VK_FALSE;
            // Presentation goes on the graphics queue: it is the family `find_queue_families` already
            // required to support present, and the only one guaranteed to.
            vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical_device, queue_for(dev, queue_kind::graphics).family,
                                                 sc.surface, &supported);
            if (!supported)
            {
                logging::error<detail::render_log>("create_swapchain: the device queue cannot present to this surface");
                return false;
            }
            return true;
        }

        bool choose_surface_format(device_state &dev, swapchain_state &sc)
        {
            std::uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical_device, sc.surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical_device, sc.surface, &count, formats.data());
            if (formats.empty())
                return false;

            const VkFormat wanted = to_vk_format(sc.desc.pixel_format);
            auto pick = [&](const VkSurfaceFormatKHR &f) {
                sc.vk_format = f.format;
                sc.color_space = f.colorSpace;
                sc.desc.pixel_format = from_vk_format(f.format);
            };

            for (const VkSurfaceFormatKHR &f : formats)
            {
                if (f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                {
                    pick(f);
                    return true;
                }
            }
            for (const VkSurfaceFormatKHR &f : formats)
            {
                if (from_vk_format(f.format) != format::unknown && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                {
                    logging::warn<detail::render_log>(
                        "create_swapchain: requested format {} is not presentable here; using {} instead",
                        static_cast<unsigned>(sc.desc.pixel_format),
                        static_cast<unsigned>(from_vk_format(f.format)));
                    pick(f);
                    return true;
                }
            }
            logging::error<detail::render_log>(
                "create_swapchain: no presentable surface format maps onto the public format set");
            return false;
        }

        VkPresentModeKHR choose_present_mode(device_state &dev, swapchain_state &sc)
        {
            if (sc.desc.vsync)
                return VK_PRESENT_MODE_FIFO_KHR; // Always available.

            std::uint32_t count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical_device, sc.surface, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical_device, sc.surface, &count, modes.data());

            for (VkPresentModeKHR preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            {
                if (std::find(modes.begin(), modes.end(), preferred) != modes.end())
                    return preferred;
            }
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        /** (Re)creates the VkSwapchainKHR and its images from `sc.desc`; on success the previous one is destroyed. */
        bool create_vk_swapchain(device_state &dev, resource_id id, swapchain_state &sc)
        {
            VkSurfaceCapabilitiesKHR caps{};
            VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physical_device, sc.surface, &caps);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>(
                    "create_swapchain: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed ({})",
                    result_string(result));
                return false;
            }

            VkExtent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX)
            {
                extent.width = std::clamp(sc.desc.extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
                extent.height =
                    std::clamp(sc.desc.extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
            }
            if (extent.width == 0 || extent.height == 0)
                return false; // Minimised: nothing to create until the window has a size again.

            std::uint32_t image_count = std::max(sc.desc.image_count, caps.minImageCount);
            if (caps.maxImageCount != 0)
                image_count = std::min(image_count, caps.maxImageCount);

            VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            texture_usage public_usage = texture_usage::render_target | texture_usage::transfer_dst;
            if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            {
                usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                public_usage |= texture_usage::transfer_src;
            }

            VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            if (!(caps.supportedCompositeAlpha & composite))
            {
                for (VkCompositeAlphaFlagBitsKHR candidate :
                     {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
                {
                    if (caps.supportedCompositeAlpha & candidate)
                    {
                        composite = candidate;
                        break;
                    }
                }
            }

            VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            info.surface = sc.surface;
            info.minImageCount = image_count;
            info.imageFormat = sc.vk_format;
            info.imageColorSpace = sc.color_space;
            info.imageExtent = extent;
            info.imageArrayLayers = 1;
            info.imageUsage = usage;
            // Presentation is always on the graphics family here, and the images are only ever
            // rendered to from graphics lists, so exclusive is correct and cheaper - concurrent can
            // cost a driver its framebuffer compression on some adapters.
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.preTransform = caps.currentTransform;
            info.compositeAlpha = composite;
            info.presentMode = sc.present_mode;
            info.clipped = VK_TRUE;
            info.oldSwapchain = sc.swapchain;

            VkSwapchainKHR created = VK_NULL_HANDLE;
            result = vkCreateSwapchainKHR(dev.device, &info, nullptr, &created);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("create_swapchain: vkCreateSwapchainKHR failed ({})",
                                                   result_string(result));
                return false;
            }

            destroy_images(dev, sc);
            if (sc.swapchain)
                vkDestroySwapchainKHR(dev.device, sc.swapchain, nullptr);
            sc.swapchain = created;
            set_debug_name(dev, VK_OBJECT_TYPE_SWAPCHAIN_KHR, handle_bits(sc.swapchain), sc.debug_name);

            std::uint32_t actual_count = 0;
            vkGetSwapchainImagesKHR(dev.device, sc.swapchain, &actual_count, nullptr);
            std::vector<VkImage> images(actual_count);
            vkGetSwapchainImagesKHR(dev.device, sc.swapchain, &actual_count, images.data());

            sc.desc.extent = {extent.width, extent.height};
            sc.desc.image_count = actual_count;

            texture_desc image_desc = back_buffer_desc(sc);
            image_desc.usage = public_usage;
            for (std::uint32_t i = 0; i < actual_count; ++i)
            {
                const resource_id texture =
                    register_presentable_image(sc.owner, id, i, images[i], sc.vk_format, image_desc, image_name(sc, i));
                if (!texture)
                    return false;
                sc.images.push_back(texture);
            }

            if (!create_sync(dev, sc, actual_count))
                return false;

            // Establish the "presentable images live in PRESENT_SRC outside a render pass" invariant.
            VkCommandBuffer cmd = begin_immediate(dev);
            if (!cmd)
                return false;
            std::vector<VkImageMemoryBarrier> barriers(actual_count, VkImageMemoryBarrier{});
            for (std::uint32_t i = 0; i < actual_count; ++i)
            {
                VkImageMemoryBarrier &b = barriers[i];
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.srcAccessMask = 0;
                b.dstAccessMask = 0;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = images[i];
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 nullptr, 0, nullptr, actual_count, barriers.data());
            if (!end_immediate(dev))
                return false;

            sc.acquired = false;
            sc.acquire_pending = false;
            sc.out_of_date = false;
            return true;
        }

        /** Empty submission that turns the pending acquire of `sc` into a render-finished signal. */
        bool convert_pending_acquire(device_state &dev, resource_id id, swapchain_state &sc) noexcept
        {
            const VkSemaphore waits[] = {sc.acquire_semaphores[sc.acquire_slot]};
            const VkPipelineStageFlags stages[] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
            const VkSemaphore signals[] = {sc.render_finished[sc.current_image]};
            const auto value = submit_batch(dev, {.kind = queue_kind::graphics,
                                                  .binary_waits = waits,
                                                  .wait_stages = stages,
                                                  .binary_signals = signals});
            if (!value)
                return false;

            const timeline_point point{rendering::device{sc.owner}, queue_kind::graphics, *value};
            sc.acquire_pending = false;
            sc.acquire_points[sc.acquire_slot] = point;
            sc.acquire_slot = (sc.acquire_slot + 1) % static_cast<std::uint32_t>(sc.acquire_semaphores.size());
            sc.render_finished_point = point;
            std::erase(dev.pending_acquires, id);
            return true;
        }
    } // namespace

    void release_swapchain_objects(device_state &dev, swapchain_state &sc) noexcept
    {
        destroy_images(dev, sc);
        destroy_sync(dev, sc);
        if (sc.swapchain)
            vkDestroySwapchainKHR(dev.device, sc.swapchain, nullptr);
        if (sc.surface)
            vkDestroySurfaceKHR(dev.instance, sc.surface, nullptr);
        sc.swapchain = VK_NULL_HANDLE;
        sc.surface = VK_NULL_HANDLE;
    }

    void collect_acquire_waits(device_state &dev, std::vector<VkSemaphore> &waits,
                               std::vector<VkPipelineStageFlags> &wait_stages, std::vector<VkSemaphore> &signals)
    {
        for (const resource_id id : dev.pending_acquires)
        {
            const swapchain_state *sc = find(reg().swapchains, id);
            if (!sc || !sc->acquire_pending)
                continue;
            waits.push_back(sc->acquire_semaphores[sc->acquire_slot]);
            wait_stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            signals.push_back(sc->render_finished[sc->current_image]);
        }
    }

    void complete_acquire_waits(device_state &dev, const timeline_point &point) noexcept
    {
        for (const resource_id id : dev.pending_acquires)
        {
            swapchain_state *sc = find(reg().swapchains, id);
            if (!sc || !sc->acquire_pending)
                continue;
            sc->acquire_pending = false;
            sc->acquire_points[sc->acquire_slot] = point;
            sc->acquire_slot = (sc->acquire_slot + 1) % static_cast<std::uint32_t>(sc->acquire_semaphores.size());
            sc->render_finished_point = point;
        }
        dev.pending_acquires.clear();
    }

} // namespace catalyst::rendering::detail::vulkan

namespace catalyst::rendering::detail
{

    using namespace vulkan;

    resource_id create_swapchain(resource_id device, const swapchain_desc &desc)
    {
        device_state *dev = find_device(device);
        if (!dev)
            return 0;

        swapchain_state s;
        s.owner = device;
        s.desc = desc;
        s.desc.debug_name = nullptr;
        s.debug_name = copy_name(desc.debug_name);
        s.windowed = desc.window.kind != platform::native_handle_kind::none && desc.window.handle != nullptr;

        const resource_id id = allocate_id();
        auto [it, inserted] = reg().swapchains.emplace(id, std::move(s));
        swapchain_state &sc = it->second;

        bool ok = false;
        if (sc.windowed)
        {
            ok = create_surface(*dev, sc) && choose_surface_format(*dev, sc);
            if (ok)
            {
                sc.present_mode = choose_present_mode(*dev, sc);
                ok = create_vk_swapchain(*dev, id, sc);
            }
        }
        else
        {
            ok = create_offscreen_images(id, sc);
        }

        if (!ok)
        {
            wait_all(*dev);
            release_swapchain_objects(*dev, sc);
            reg().swapchains.erase(id);
            return 0;
        }
        return id;
    }

    void destroy_swapchain(resource_id id) noexcept
    {
        swapchain_state *sc = find(reg().swapchains, id);
        if (!sc)
            return;
        if (device_state *dev = find_device(sc->owner))
        {
            wait_all(*dev);
            std::erase(dev->pending_acquires, id);
            release_swapchain_objects(*dev, *sc);
        }
        reg().swapchains.erase(id);
    }

    bool is_swapchain_valid(resource_id id) noexcept
    {
        return find(reg().swapchains, id) != nullptr;
    }

    swapchain_desc get_swapchain_desc(resource_id id) noexcept
    {
        const swapchain_state *sc = find(reg().swapchains, id);
        if (!sc)
            return {};
        swapchain_desc d = sc->desc;
        d.debug_name = name_or_null(sc->debug_name);
        return d;
    }

    bool resize_swapchain(resource_id id, extent2d extent)
    {
        swapchain_state *sc = find(reg().swapchains, id);
        if (!sc)
            return false;
        device_state *dev = find_device(sc->owner);
        if (!dev)
            return false;

        wait_all(*dev);
        std::erase(dev->pending_acquires, id);
        sc->acquired = false;
        sc->acquire_pending = false;
        sc->desc.extent = extent;

        if (sc->windowed)
        {
            if (!create_vk_swapchain(*dev, id, *sc))
            {
                sc->out_of_date = true; // Keep the old chain but hand out no images until a successful resize.
                return false;
            }
            return true;
        }

        destroy_images(*dev, *sc);
        return create_offscreen_images(id, *sc);
    }

    resource_id acquire_next_image(resource_id id)
    {
        swapchain_state *sc = find(reg().swapchains, id);
        if (!sc || sc->images.empty())
            return 0;
        device_state *dev = find_device(sc->owner);
        if (!dev)
            return 0;
        refresh_completed(*dev);
        collect_garbage(*dev);

        if (!sc->windowed)
        {
            const resource_id image = sc->images[sc->next_image];
            sc->next_image = (sc->next_image + 1) % static_cast<std::uint32_t>(sc->images.size());
            sc->acquired = true;
            return image;
        }

        if (sc->out_of_date)
            return 0;
        if (sc->acquired)
            return sc->images[sc->current_image]; // Acquired and not yet presented: same image again.

        const std::uint32_t slot = sc->acquire_slot;
        wait_point(*dev, sc->acquire_points[slot]); // The semaphore must not be in use any more.

        std::uint32_t index = 0;
        const VkResult result = vkAcquireNextImageKHR(dev->device, sc->swapchain, UINT64_MAX,
                                                      sc->acquire_semaphores[slot], VK_NULL_HANDLE, &index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            sc->out_of_date = true;
            return 0;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            logging::error<detail::render_log>("acquire_next_image: vkAcquireNextImageKHR failed ({})",
                                               result_string(result));
            return 0;
        }

        sc->current_image = index;
        sc->acquired = true;
        sc->acquire_pending = true;
        if (std::find(dev->pending_acquires.begin(), dev->pending_acquires.end(), id) == dev->pending_acquires.end())
            dev->pending_acquires.push_back(id);
        return sc->images[index];
    }

    bool present(resource_id id)
    {
        swapchain_state *sc = find(reg().swapchains, id);
        if (!sc || !sc->acquired)
            return false;
        device_state *dev = find_device(sc->owner);
        if (!dev)
            return false;

        if (!sc->windowed)
        {
            sc->acquired = false;
            return true;
        }

        if (sc->acquire_pending && !convert_pending_acquire(*dev, id, *sc))
            return false;

        VkSemaphore wait = sc->render_finished[sc->current_image];
        if (queue_for(*dev, queue_kind::graphics).last_submitted > sc->render_finished_point.value())
        {
            // Work was submitted after render_finished was signalled: chain a signal that covers it too.
            const VkSemaphore waits[] = {wait};
            const VkPipelineStageFlags stages[] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
            const VkSemaphore signals[] = {sc->present_ready[sc->current_image]};
            const auto chained = submit_batch(*dev, {.kind = queue_kind::graphics,
                                                     .binary_waits = waits,
                                                     .wait_stages = stages,
                                                     .binary_signals = signals});
            if (!chained)
                return false;
            wait = signals[0];
        }

        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &wait;
        info.swapchainCount = 1;
        info.pSwapchains = &sc->swapchain;
        info.pImageIndices = &sc->current_image;

        const VkResult result = vkQueuePresentKHR(queue_for(*dev, queue_kind::graphics).queue, &info);
        sc->acquired = false;

        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
            return true;
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            sc->out_of_date = true;
            return false;
        }
        logging::error<detail::render_log>("present: vkQueuePresentKHR failed ({})", result_string(result));
        return false;
    }

} // namespace catalyst::rendering::detail
