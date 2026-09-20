#pragma once

// VmaAllocator is an opaque handle; including vk_mem_alloc.h here would put its
// whole implementation surface in every translation unit.
VK_DEFINE_HANDLE(VmaAllocator)

namespace encke
{
    class VulkanContext;
    class VulkanDevice;

    // Owns the VMA allocator. Every device-memory allocation goes through it;
    // nothing in this project calls vkAllocateMemory directly.
    class VulkanAllocator
    {
    public:
        VulkanAllocator() = default;
        ~VulkanAllocator();

        VulkanAllocator(VulkanAllocator const&)            = delete;
        VulkanAllocator& operator=(VulkanAllocator const&) = delete;
        VulkanAllocator(VulkanAllocator&&)                 = delete;
        VulkanAllocator& operator=(VulkanAllocator&&)      = delete;

        bool init(VulkanContext const& context, VulkanDevice const& device);
        void shutdown();

        VmaAllocator handle() const { return allocator_; }

    private:
        VmaAllocator allocator_ = nullptr;
    };
}
