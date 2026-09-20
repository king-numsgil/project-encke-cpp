#include "core/pch.hpp"

#include "vulkan/allocator.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "vulkan/context.hpp"
#include "vulkan/device.hpp"

// volk loads every entry point at runtime, so VMA must not expect linked
// symbols. It is handed vkGetInstanceProcAddr/vkGetDeviceProcAddr and fetches
// the rest itself. This is the one translation unit that defines the
// implementation.
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION           1003000
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace encke
{
    VulkanAllocator::~VulkanAllocator()
    {
        shutdown();
    }

    bool VulkanAllocator::init(VulkanContext const& context, VulkanDevice const& device)
    {
        VmaVulkanFunctions const functions{
            .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
        };

        VmaAllocatorCreateInfo const info{
            .flags            = 0,
            .physicalDevice   = device.physical(),
            .device           = device.handle(),
            .pAllocationCallbacks = memory::vulkan_callbacks(),
            .pVulkanFunctions = &functions,
            .instance         = context.instance(),
            .vulkanApiVersion = VK_API_VERSION_1_3,
        };

        VkResult const created = vmaCreateAllocator(&info, &allocator_);
        if (created != VK_SUCCESS)
        {
            log::vk_error("vmaCreateAllocator", created);
            return false;
        }

        log::info("allocator ready");
        return true;
    }

    void VulkanAllocator::shutdown()
    {
        if (allocator_ != nullptr)
        {
            // Complains loudly to the debug callback if anything is still live.
            vmaDestroyAllocator(allocator_);
            allocator_ = nullptr;
        }
    }
}
