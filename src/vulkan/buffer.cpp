#include "core/pch.hpp"

#include "vulkan/buffer.hpp"

#include "core/log.hpp"
#include "vulkan/allocator.hpp"
#include "vulkan/device.hpp"

#include <cstring>

#include <vk_mem_alloc.h>

namespace encke
{
    namespace
    {
        // For every buffer the CPU writes. HOST_COHERENT is required, not
        // preferred: on non-coherent memory a CPU write can sit in a cache the
        // GPU does not see until vmaFlushAllocation, and nothing here flushes.
        // The spec guarantees a HOST_VISIBLE | HOST_COHERENT type exists, so
        // this cannot fail to find one, and VMA can still pick write-combined
        // or resizable-BAR device-local memory where those are coherent.
        VmaAllocationCreateInfo host_written()
        {
            return VmaAllocationCreateInfo{
                .flags          = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                  VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .usage          = VMA_MEMORY_USAGE_AUTO,
                .requiredFlags  = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                .preferredFlags = 0,
                .memoryTypeBits = 0,
                .pool           = nullptr,
                .pUserData      = nullptr,
                .priority       = 0.0f,
                .minAlignment   = 0,
            };
        }
    }

    Buffer::~Buffer()
    {
        shutdown();
    }

    bool Buffer::init_device(VulkanAllocator const& allocator, VulkanDevice const& device,
                             VkDeviceSize size, VkBufferUsageFlags usage, void const* data)
    {
        if (size == 0)
        {
            log::error("refusing to create an empty buffer");
            return false;
        }

        allocator_ = &allocator;
        size_      = size;

        VkBufferCreateInfo const device_info{
            .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .size                  = size,
            .usage                 = usage | (data != nullptr ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0u),
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
        };

        VmaAllocationCreateInfo const device_alloc{
            .flags = 0,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };

        VkResult result = vmaCreateBuffer(allocator.handle(), &device_info, &device_alloc,
                                          &buffer_, &allocation_, nullptr);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vmaCreateBuffer (device)", result);
            return false;
        }

        if (data == nullptr)
        {
            return true;
        }

        // Staging buffer: host-visible, written once sequentially, then copied
        // into device-local memory the GPU reads at full speed.
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

        VmaAllocationCreateInfo const staging_alloc = host_written();

        VkBuffer          staging            = VK_NULL_HANDLE;
        VmaAllocation     staging_allocation = nullptr;
        VmaAllocationInfo staging_mapped{};

        result = vmaCreateBuffer(allocator.handle(), &staging_info, &staging_alloc, &staging,
                                 &staging_allocation, &staging_mapped);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vmaCreateBuffer (staging)", result);
            shutdown();
            return false;
        }

        std::memcpy(staging_mapped.pMappedData, data, static_cast<size_t>(size));

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

    bool Buffer::init_mapped(VulkanAllocator const& allocator, VkDeviceSize size,
                             VkBufferUsageFlags usage)
    {
        if (size == 0)
        {
            log::error("refusing to create an empty buffer");
            return false;
        }

        allocator_ = &allocator;
        size_      = size;

        VkBufferCreateInfo const info{
            .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .size                  = size,
            .usage                 = usage,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
        };

        // Sequential write lets VMA pick write-combined memory, and on a
        // discrete GPU with resizable BAR it may land in device-local
        // host-visible memory. Either way the CPU must only ever write it.
        VmaAllocationCreateInfo const alloc = host_written();

        VmaAllocationInfo mapped{};
        VkResult const    result = vmaCreateBuffer(allocator.handle(), &info, &alloc, &buffer_,
                                                   &allocation_, &mapped);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vmaCreateBuffer (mapped)", result);
            return false;
        }

        mapped_ = mapped.pMappedData;
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

        mapped_    = nullptr;
        size_      = 0;
        allocator_ = nullptr;
    }
}
