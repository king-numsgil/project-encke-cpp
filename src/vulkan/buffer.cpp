#include "core/pch.hpp"

#include "vulkan/buffer.hpp"

#include "core/log.hpp"
#include "vulkan/allocator.hpp"
#include "vulkan/device.hpp"

#include <cstring>

#include <vk_mem_alloc.h>

namespace encke
{
    Buffer::~Buffer()
    {
        shutdown();
    }

    bool Buffer::init(VulkanAllocator const& allocator, VulkanDevice const& device,
                      void const* data, size_t size, VkBufferUsageFlags usage)
    {
        if (data == nullptr || size == 0)
        {
            log::error("refusing to create an empty buffer");
            return false;
        }

        allocator_ = &allocator;

        // Staging buffer: host-visible, sequentially written once, then copied
        // into device-local memory that the GPU can read at full speed.
        VkBufferCreateInfo const staging_info{
            .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .size                  = size,
            .usage                 = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
        };

        VmaAllocationCreateInfo const staging_alloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        VkBuffer          staging            = VK_NULL_HANDLE;
        VmaAllocation     staging_allocation = nullptr;
        VmaAllocationInfo staging_mapped{};

        VkResult result = vmaCreateBuffer(allocator.handle(), &staging_info, &staging_alloc,
                                          &staging, &staging_allocation, &staging_mapped);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vmaCreateBuffer (staging)", result);
            return false;
        }

        std::memcpy(staging_mapped.pMappedData, data, size);

        VkBufferCreateInfo const device_info{
            .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .size                  = size,
            .usage                 = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
        };

        VmaAllocationCreateInfo const device_alloc{
            .flags = 0,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        result = vmaCreateBuffer(allocator.handle(), &device_info, &device_alloc, &buffer_,
                                 &allocation_, nullptr);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vmaCreateBuffer (device)", result);
            vmaDestroyBuffer(allocator.handle(), staging, staging_allocation);
            return false;
        }

        bool const copied = device.submit_immediate([&](VkCommandBuffer command) {
            VkBufferCopy const region{.srcOffset = 0, .dstOffset = 0, .size = size};
            vkCmdCopyBuffer(command, staging, buffer_, 1, &region);
        });

        vmaDestroyBuffer(allocator.handle(), staging, staging_allocation);

        if (!copied)
        {
            shutdown();
            return false;
        }

        return true;
    }

    void Buffer::shutdown()
    {
        if (buffer_ != VK_NULL_HANDLE && allocator_ != nullptr)
        {
            vmaDestroyBuffer(allocator_->handle(), buffer_, allocation_);
            buffer_     = VK_NULL_HANDLE;
            allocation_ = nullptr;
        }

        allocator_ = nullptr;
    }
}
