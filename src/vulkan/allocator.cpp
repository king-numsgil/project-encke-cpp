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
        // Zeroed, then the members that matter assigned: both structs are long
        // and mostly optional, and a designated initializer naming only some
        // of them trips -Wmissing-field-initializers. Every entry point VMA
        // is not handed here, it fetches itself through these two.
        VmaVulkanFunctions functions{};
        functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        functions.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo info{};
        info.flags                = 0;
        info.physicalDevice       = device.physical();
        info.device               = device.handle();
        info.pAllocationCallbacks = memory::vulkan_callbacks();
        info.pVulkanFunctions     = &functions;
        info.instance             = context.instance();
        info.vulkanApiVersion     = VK_API_VERSION_1_3;

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
