#include "core/pch.hpp"

#include "vulkan/depth.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "vulkan/allocator.hpp"
#include "vulkan/device.hpp"

#include <vk_mem_alloc.h>

namespace encke
{
    DepthTarget::~DepthTarget()
    {
        shutdown();
    }

    bool DepthTarget::init(VulkanAllocator const& allocator, VulkanDevice const& device,
                           VkExtent2D extent)
    {
        allocator_ = &allocator;
        device_    = &device;

        // D32_SFLOAT is not unconditionally required as a depth attachment --
        // the spec only guarantees one of it and X8_D24_UNORM_PACK32 -- so say
        // so plainly rather than failing later inside image creation.
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device.physical(), kFormat, &properties);
        if ((properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
        {
            log::error("D32_SFLOAT unsupported as a depth attachment on this device");
            return false;
        }

        return build(extent);
    }

    bool DepthTarget::resize(VkExtent2D extent)
    {
        if (view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_->handle(), view_, memory::vulkan_callbacks());
            view_ = VK_NULL_HANDLE;
        }

        if (image_ != VK_NULL_HANDLE)
        {
            vmaDestroyImage(allocator_->handle(), image_, allocation_);
            image_      = VK_NULL_HANDLE;
            allocation_ = nullptr;
        }

        return build(extent);
    }

    bool DepthTarget::build(VkExtent2D extent)
    {
        VkImageCreateInfo const image_info{
            .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .imageType             = VK_IMAGE_TYPE_2D,
            .format                = kFormat,
            .extent                = {extent.width, extent.height, 1},
            .mipLevels             = 1,
            .arrayLayers           = 1,
            .samples               = VK_SAMPLE_COUNT_1_BIT,
            .tiling                = VK_IMAGE_TILING_OPTIMAL,
            .usage                 = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
            .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo const alloc_info{
            .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        VkResult result = vmaCreateImage(allocator_->handle(), &image_info, &alloc_info,
                                         &image_, &allocation_, nullptr);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vmaCreateImage (depth)", result);
            return false;
        }

        VkImageViewCreateInfo const view_info{
            .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext      = nullptr,
            .flags      = 0,
            .image      = image_,
            .viewType   = VK_IMAGE_VIEW_TYPE_2D,
            .format     = kFormat,
            .components = {},
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };

        result = vkCreateImageView(device_->handle(), &view_info, memory::vulkan_callbacks(),
                                   &view_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateImageView (depth)", result);
            return false;
        }

        return true;
    }

    void DepthTarget::shutdown()
    {
        if (device_ == nullptr || allocator_ == nullptr)
        {
            return;
        }

        if (view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_->handle(), view_, memory::vulkan_callbacks());
            view_ = VK_NULL_HANDLE;
        }

        if (image_ != VK_NULL_HANDLE)
        {
            vmaDestroyImage(allocator_->handle(), image_, allocation_);
            image_      = VK_NULL_HANDLE;
            allocation_ = nullptr;
        }

        allocator_ = nullptr;
        device_    = nullptr;
    }
}
