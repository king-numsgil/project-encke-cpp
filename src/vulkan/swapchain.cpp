#include "core/pch.hpp"

#include "vulkan/swapchain.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
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

        // The UNORM twin of an sRGB format, for writing already-encoded values
        // through a view the hardware will not encode again. UNDEFINED when
        // the format has no such twin, or is not sRGB in the first place.
        VkFormat unorm_twin(VkFormat format)
        {
            switch (format)
            {
            case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
            case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
            case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
            default: return VK_FORMAT_UNDEFINED;
            }
        }

        // FIFO is the only mode guaranteed present, and it is vsync. MAILBOX
        // gives lower latency without tearing where the driver offers it.
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
            vkDestroySwapchainKHR(device_->handle(), swapchain_, memory::vulkan_callbacks());
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
        present_mode_                           = choose_present_mode(modes);

        extent_ = choose_extent(capabilities, size);
        format_ = surface_format.format;

        VkFormat const twin    = unorm_twin(format_);
        bool const     mutable_format = device_->mutable_swapchain_format() && twin != VK_FORMAT_UNDEFINED;
        ui_format_             = mutable_format ? twin : format_;

        // The spec requires the list of every format a view may take whenever
        // the chain is created mutable.
        VkFormat const view_formats[]{format_, twin};
        VkImageFormatListCreateInfo const format_list{
            .sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
            .pNext           = nullptr,
            .viewFormatCount = static_cast<u32>(std::size(view_formats)),
            .pViewFormats    = view_formats,
        };

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
            .pNext            = mutable_format ? &format_list : nullptr,
            .flags            = mutable_format
                                    ? VkSwapchainCreateFlagsKHR{VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR}
                                    : VkSwapchainCreateFlagsKHR{0},
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
            .presentMode           = present_mode_,
            .clipped               = VK_TRUE,
            .oldSwapchain          = VK_NULL_HANDLE,
        };

        result = vkCreateSwapchainKHR(device_->handle(), &info, memory::vulkan_callbacks(),
                                      &swapchain_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateSwapchainKHR", result);
            return false;
        }

        u32 actual_count = 0;
        vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &actual_count, nullptr);
        images_.resize(actual_count);
        vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &actual_count, images_.data());

        if (!create_views(views_, format_))
        {
            return false;
        }

        // Without a distinct UI format the UI shares the main views rather
        // than holding a duplicate set.
        if (ui_format_ != format_ && !create_views(ui_views_, ui_format_))
        {
            return false;
        }

        log::info("swapchain %ux%u, %u images, %s%s", extent_.width, extent_.height, actual_count,
                  present_mode_name(), ui_format_ != format_ ? ", UNORM UI view" : "");
        return true;
    }

    bool VulkanSwapchain::create_views(vector<VkImageView>& views, VkFormat format)
    {
        views.resize(images_.size(), VK_NULL_HANDLE);
        for (size_t index = 0; index < images_.size(); ++index)
        {
            VkImageViewCreateInfo const view_info{
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext    = nullptr,
                .flags    = 0,
                .image    = images_[index],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = format,
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

            VkResult const result = vkCreateImageView(device_->handle(), &view_info,
                                                      memory::vulkan_callbacks(), &views[index]);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkCreateImageView", result);
                return false;
            }
        }

        return true;
    }

    char const* VulkanSwapchain::present_mode_name() const
    {
        switch (present_mode_)
        {
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR:         return "fifo (vsync)";
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "immediate";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo relaxed";
        default:                               return "other";
        }
    }

    void VulkanSwapchain::destroy_views()
    {
        if (device_ == nullptr)
        {
            return;
        }

        for (vector<VkImageView>* const set : {&views_, &ui_views_})
        {
            for (VkImageView& view : *set)
            {
                if (view != VK_NULL_HANDLE)
                {
                    vkDestroyImageView(device_->handle(), view, memory::vulkan_callbacks());
                    view = VK_NULL_HANDLE;
                }
            }
            set->clear();
        }

        images_.clear();
    }

    void VulkanSwapchain::shutdown()
    {
        destroy_views();

        if (swapchain_ != VK_NULL_HANDLE && device_ != nullptr)
        {
            vkDestroySwapchainKHR(device_->handle(), swapchain_, memory::vulkan_callbacks());
            swapchain_ = VK_NULL_HANDLE;
        }

        context_ = nullptr;
        device_  = nullptr;
    }
}
