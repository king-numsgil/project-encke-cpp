#include "core/pch.hpp"

#include "vulkan/swapchain.hpp"

#include "core/log.hpp"
#include "vulkan/context.hpp"
#include "vulkan/device.hpp"

#include <algorithm>
#include <limits>

namespace encke
{
    namespace
    {
        VkSurfaceFormatKHR choose_format(vector<VkSurfaceFormatKHR> const& available)
        {
            for (VkSurfaceFormatKHR const& candidate : available)
            {
                if (candidate.format == VK_FORMAT_B8G8R8A8_SRGB &&
                    candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                {
                    return candidate;
                }
            }

            return available.front();
        }

        // FIFO is the only mode guaranteed present. MAILBOX gives lower
        // latency without tearing where the driver offers it.
        VkPresentModeKHR choose_present_mode(vector<VkPresentModeKHR> const& available)
        {
            for (VkPresentModeKHR const mode : available)
            {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
                {
                    return mode;
                }
            }

            return VK_PRESENT_MODE_FIFO_KHR;
        }

        VkExtent2D choose_extent(VkSurfaceCapabilitiesKHR const& capabilities, i32vec2 size)
        {
            // A non-sentinel currentExtent means the surface dictates the size
            // and the swapchain must match it exactly.
            if (capabilities.currentExtent.width != std::numeric_limits<u32>::max())
            {
                return capabilities.currentExtent;
            }

            VkExtent2D extent{
                .width  = static_cast<u32>(size.x),
                .height = static_cast<u32>(size.y),
            };

            extent.width = std::clamp(extent.width,
                                      capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(extent.height,
                                       capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
            return extent;
        }
    }

    VulkanSwapchain::~VulkanSwapchain()
    {
        shutdown();
    }

    bool VulkanSwapchain::init(VulkanContext const& context, VulkanDevice const& device,
                               i32vec2 size)
    {
        context_ = &context;
        device_  = &device;
        return build(size);
    }

    bool VulkanSwapchain::recreate(i32vec2 size)
    {
        destroy_views();

        if (swapchain_ != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(device_->handle(), swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }

        return build(size);
    }

    bool VulkanSwapchain::build(i32vec2 size)
    {
        VkPhysicalDevice const physical = device_->physical();
        VkSurfaceKHR const     surface  = context_->surface();

        VkSurfaceCapabilitiesKHR capabilities{};
        VkResult result =
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
            return false;
        }

        u32 format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
        vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());

        u32 mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count, nullptr);
        vector<VkPresentModeKHR> modes(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count, modes.data());

        if (formats.empty() || modes.empty())
        {
            log::error("surface reports no formats or present modes");
            return false;
        }

        VkSurfaceFormatKHR const surface_format = choose_format(formats);
        VkPresentModeKHR const   present_mode   = choose_present_mode(modes);

        extent_ = choose_extent(capabilities, size);
        format_ = surface_format.format;

        if (extent_.width == 0 || extent_.height == 0)
        {
            log::error("refusing to build a zero-sized swapchain");
            return false;
        }

        // One more than the minimum lets the driver hand back an image while
        // another is still being presented. maxImageCount == 0 means no limit.
        u32 image_count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount)
        {
            image_count = capabilities.maxImageCount;
        }

        u32 const families[] = {*device_->families().graphics, *device_->families().present};
        bool const shared    = families[0] == families[1];

        VkSwapchainCreateInfoKHR const info{
            .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .pNext            = nullptr,
            .flags            = 0,
            .surface          = surface,
            .minImageCount    = image_count,
            .imageFormat      = surface_format.format,
            .imageColorSpace  = surface_format.colorSpace,
            .imageExtent      = extent_,
            .imageArrayLayers = 1,
            .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = shared ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
            .queueFamilyIndexCount = shared ? 0u : 2u,
            .pQueueFamilyIndices   = shared ? nullptr : families,
            .preTransform          = capabilities.currentTransform,
            .compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode           = present_mode,
            .clipped               = VK_TRUE,
            .oldSwapchain          = VK_NULL_HANDLE,
        };

        result = vkCreateSwapchainKHR(device_->handle(), &info, nullptr, &swapchain_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateSwapchainKHR", result);
            return false;
        }

        u32 actual_count = 0;
        vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &actual_count, nullptr);
        images_.resize(actual_count);
        vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &actual_count, images_.data());

        views_.resize(actual_count, VK_NULL_HANDLE);
        for (u32 index = 0; index < actual_count; ++index)
        {
            VkImageViewCreateInfo const view_info{
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext    = nullptr,
                .flags    = 0,
                .image    = images_[index],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = format_,
                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                .subresourceRange = {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel   = 0,
                    .levelCount     = 1,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
            };

            result = vkCreateImageView(device_->handle(), &view_info, nullptr, &views_[index]);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkCreateImageView", result);
                return false;
            }
        }

        log::info("swapchain %ux%u, %u images, %s",
                  extent_.width, extent_.height, actual_count,
                  present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
        return true;
    }

    void VulkanSwapchain::destroy_views()
    {
        if (device_ == nullptr)
        {
            return;
        }

        for (VkImageView& view : views_)
        {
            if (view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(device_->handle(), view, nullptr);
                view = VK_NULL_HANDLE;
            }
        }

        views_.clear();
        images_.clear();
    }

    void VulkanSwapchain::shutdown()
    {
        destroy_views();

        if (swapchain_ != VK_NULL_HANDLE && device_ != nullptr)
        {
            vkDestroySwapchainKHR(device_->handle(), swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }

        context_ = nullptr;
        device_  = nullptr;
    }
}
