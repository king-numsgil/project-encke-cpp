#include "core/pch.hpp"

#include "vulkan/texture.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "vulkan/allocator.hpp"
#include "vulkan/buffer.hpp"
#include "vulkan/device.hpp"

#include <algorithm>
#include <cstring>

#include <vk_mem_alloc.h>

namespace encke
{
    namespace
    {
        constexpr u32 kBytesPerTexel = 4;

        struct LevelTransition
        {
            u32                   level;
            u32                   count;
            VkImageLayout         from;
            VkImageLayout         to;
            VkPipelineStageFlags2 src_stage;
            VkAccessFlags2        src_access;
            VkPipelineStageFlags2 dst_stage;
            VkAccessFlags2        dst_access;
        };

        void transition(VkCommandBuffer command, VkImage image, LevelTransition const& t)
        {
            VkImageMemoryBarrier2 const barrier{
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext               = nullptr,
                .srcStageMask        = t.src_stage,
                .srcAccessMask       = t.src_access,
                .dstStageMask        = t.dst_stage,
                .dstAccessMask       = t.dst_access,
                .oldLayout           = t.from,
                .newLayout           = t.to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = image,
                .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, t.level, t.count, 0, 1},
            };

            VkDependencyInfo const dependency{
                .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext                    = nullptr,
                .dependencyFlags          = 0,
                .memoryBarrierCount       = 0,
                .pMemoryBarriers          = nullptr,
                .bufferMemoryBarrierCount = 0,
                .pBufferMemoryBarriers    = nullptr,
                .imageMemoryBarrierCount  = 1,
                .pImageMemoryBarriers     = &barrier,
            };

            vkCmdPipelineBarrier2(command, &dependency);
        }

        i32 level_size(u32 base, u32 level)
        {
            return static_cast<i32>(std::max(base >> level, 1u));
        }
    }

    Texture::~Texture()
    {
        shutdown();
    }

    VkDeviceSize Texture::upload_bytes(u32 width, u32 height)
    {
        return VkDeviceSize{width} * height * kBytesPerTexel;
    }

    bool Texture::init(VulkanAllocator const& allocator, VulkanDevice const& device,
                       Config const& config, u32 width, u32 height, span<u8 const> texels)
    {
        if (texels.size() != upload_bytes(width, height))
        {
            log::error("%s: %zu bytes of texels for %ux%u", config.name, texels.size(), width,
                       height);
            return false;
        }

        if (!create(allocator, device, config, width, height))
        {
            return false;
        }

        Buffer staging;
        if (!staging.init_mapped(allocator, texels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        {
            shutdown();
            return false;
        }
        std::memcpy(staging.mapped(), texels.data(), texels.size());

        bool const uploaded = device.submit_immediate([&](VkCommandBuffer command) {
            record_upload(command, staging.handle(), 0);
        });

        if (!uploaded)
        {
            shutdown();
            return false;
        }

        return true;
    }

    bool Texture::create(VulkanAllocator const& allocator, VulkanDevice const& device,
                         Config const& config, u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            log::error("%s: %ux%u is empty", config.name, width, height);
            return false;
        }

        // Mip generation blits with a linear filter, which the format must
        // support as a source and a destination as well as for sampling.
        // Every desktop driver does for RGBA8; fail by name if not.
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device.physical(), config.format, &properties);

        constexpr VkFormatFeatureFlags kNeeded =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & kNeeded) != kNeeded)
        {
            log::error("%s: format %d cannot be sampled and blitted with linear filtering",
                       config.name, static_cast<int>(config.format));
            return false;
        }

        allocator_  = &allocator;
        device_     = &device;
        width_      = width;
        height_     = height;
        mip_levels_ = static_cast<u32>(std::bit_width(std::max(width, height)));

        VkImageCreateInfo const image_info{
            .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .imageType             = VK_IMAGE_TYPE_2D,
            .format                = config.format,
            .extent                = {width, height, 1},
            .mipLevels             = mip_levels_,
            .arrayLayers           = 1,
            .samples               = VK_SAMPLE_COUNT_1_BIT,
            .tiling                = VK_IMAGE_TILING_OPTIMAL,
            // Transfer source as well, because each mip is blitted from the
            // level above it.
            .usage                 = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
            .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo const alloc_info{
            .flags          = 0,
            .usage          = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .requiredFlags  = 0,
            .preferredFlags = 0,
            .memoryTypeBits = 0,
            .pool           = nullptr,
            .pUserData      = nullptr,
            .priority       = 0.0f,
            .minAlignment   = 0,
        };

        VkResult result = vmaCreateImage(allocator.handle(), &image_info, &alloc_info, &image_,
                                         &allocation_, nullptr);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vmaCreateImage", result);
            log::error("  while creating %s", config.name);
            return false;
        }

        VkImageViewCreateInfo const view_info{
            .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext      = nullptr,
            .flags      = 0,
            .image      = image_,
            .viewType   = VK_IMAGE_VIEW_TYPE_2D,
            .format     = config.format,
            .components = {},
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = mip_levels_,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };

        result = vkCreateImageView(device.handle(), &view_info, memory::vulkan_callbacks(), &view_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateImageView", result);
            log::error("  while creating %s", config.name);
            shutdown();
            return false;
        }

        return true;
    }

    void Texture::record_upload(VkCommandBuffer command, VkBuffer staging, VkDeviceSize offset) const
    {
        constexpr VkPipelineStageFlags2 kCopy = VK_PIPELINE_STAGE_2_COPY_BIT;
        constexpr VkPipelineStageFlags2 kBlit = VK_PIPELINE_STAGE_2_BLIT_BIT;
        constexpr VkPipelineStageFlags2 kRead = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        constexpr VkAccessFlags2 kWrite    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        constexpr VkAccessFlags2 kSource   = VK_ACCESS_2_TRANSFER_READ_BIT;
        constexpr VkAccessFlags2 kSampled  = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        constexpr VkImageLayout  kDst      = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        constexpr VkImageLayout  kSrc      = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        constexpr VkImageLayout  kReadOnly = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;

        u32 const levels = mip_levels_;
        u32 const width  = width_;
        u32 const height = height_;

        transition(command, image_, {0, levels, VK_IMAGE_LAYOUT_UNDEFINED, kDst,
                                     VK_PIPELINE_STAGE_2_NONE, 0, kCopy | kBlit, kWrite});

        VkBufferImageCopy const region{
            .bufferOffset      = offset,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset       = {0, 0, 0},
            .imageExtent       = {width, height, 1},
        };
        vkCmdCopyBufferToImage(command, staging, image_, kDst, 1, &region);

        // Each level is read by the blit that fills the next, then handed
        // to the fragment stage. The last is written and never read here.
        for (u32 level = 1; level < levels; ++level)
        {
            VkPipelineStageFlags2 const filled = level == 1 ? kCopy : kBlit;
            transition(command, image_, {level - 1, 1, kDst, kSrc, filled, kWrite, kBlit, kSource});

            VkImageBlit const blit{
                .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1},
                .srcOffsets     = {{0, 0, 0},
                                   {level_size(width, level - 1), level_size(height, level - 1), 1}},
                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                .dstOffsets     = {{0, 0, 0},
                                   {level_size(width, level), level_size(height, level), 1}},
            };
            vkCmdBlitImage(command, image_, kSrc, image_, kDst, 1, &blit, VK_FILTER_LINEAR);

            transition(command, image_, {level - 1, 1, kSrc, kReadOnly, kBlit, 0, kRead, kSampled});
        }

        VkPipelineStageFlags2 const last = levels == 1 ? kCopy : kBlit;
        transition(command, image_, {levels - 1, 1, kDst, kReadOnly, last, kWrite, kRead, kSampled});
    }

    void Texture::shutdown()
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

        width_      = 0;
        height_     = 0;
        mip_levels_ = 0;
        allocator_  = nullptr;
        device_     = nullptr;
    }
}
