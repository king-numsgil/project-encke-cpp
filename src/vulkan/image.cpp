#include "core/pch.hpp"

#include "vulkan/image.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "vulkan/allocator.hpp"
#include "vulkan/device.hpp"

#include <vk_mem_alloc.h>

namespace encke
{
    Image::~Image()
    {
        shutdown();
    }

    bool Image::init(VulkanAllocator const& allocator, VulkanDevice const& device,
                     Config const& config, VkExtent2D extent)
    {
        allocator_ = &allocator;
        device_    = &device;
        config_    = config;

        // Fail with the image's name rather than deep inside vmaCreateImage.
        // Not every format supports every usage -- D32_SFLOAT is not even
        // guaranteed as a depth attachment on its own.
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device.physical(), config.format, &properties);

        struct Requirement
        {
            VkImageUsageFlags    usage;
            VkFormatFeatureFlags feature;
            char const*          what;
        };

        constexpr Requirement kRequirements[]{
            {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
             "colour attachment"},
            {VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "depth attachment"},
            {VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "sampled image"},
            {VK_IMAGE_USAGE_STORAGE_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "storage image"},
        };

        for (Requirement const& requirement : kRequirements)
        {
            if ((config.usage & requirement.usage) != 0 &&
                (properties.optimalTilingFeatures & requirement.feature) == 0)
            {
                log::error("%s: format %d unsupported as a %s", config.name,
                           static_cast<int>(config.format), requirement.what);
                return false;
            }
        }

        return build(extent);
    }

    bool Image::resize(VkExtent2D extent)
    {
        destroy();
        return build(extent);
    }

    bool Image::build(VkExtent2D extent)
    {
        VkImageCreateInfo const image_info{
            .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .imageType             = VK_IMAGE_TYPE_2D,
            .format                = config_.format,
            .extent                = {extent.width, extent.height, 1},
            .mipLevels             = 1,
            .arrayLayers           = 1,
            .samples               = VK_SAMPLE_COUNT_1_BIT,
            .tiling                = VK_IMAGE_TILING_OPTIMAL,
            .usage                 = config_.usage,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
            .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        // Render targets are large and replaced whole on resize; a dedicated
        // allocation avoids fragmenting a shared block every time.
        VmaAllocationCreateInfo const alloc_info{
            .flags          = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
            .usage          = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .requiredFlags  = 0,
            .preferredFlags = 0,
            .memoryTypeBits = 0,
            .pool           = nullptr,
            .pUserData      = nullptr,
            .priority       = 0.0f,
            .minAlignment   = 0,
        };

        VkResult result = vmaCreateImage(allocator_->handle(), &image_info, &alloc_info, &image_,
                                         &allocation_, nullptr);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vmaCreateImage", result);
            log::error("  while creating %s", config_.name);
            return false;
        }

        VkImageViewCreateInfo const view_info{
            .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext      = nullptr,
            .flags      = 0,
            .image      = image_,
            .viewType   = VK_IMAGE_VIEW_TYPE_2D,
            .format     = config_.format,
            .components = {},
            .subresourceRange = {
                .aspectMask     = config_.aspect,
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
            log::vk_error("vkCreateImageView", result);
            log::error("  while creating %s", config_.name);
            return false;
        }

        return true;
    }

    void Image::destroy()
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
    }

    void Image::shutdown()
    {
        if (device_ == nullptr || allocator_ == nullptr)
        {
            return;
        }

        destroy();
        allocator_ = nullptr;
        device_    = nullptr;
    }
}
