#pragma once

VK_DEFINE_HANDLE(VmaAllocation)

namespace encke
{
    class VulkanAllocator;
    class VulkanDevice;

    class Buffer
    {
    public:
        Buffer() = default;
        ~Buffer();

        Buffer(Buffer const&)            = delete;
        Buffer& operator=(Buffer const&) = delete;
        Buffer(Buffer&&)                 = delete;
        Buffer& operator=(Buffer&&)      = delete;

        // Device-local. With `data` it is filled once through a staging copy;
        // without, its contents are undefined until the GPU writes them.
        bool init_device(VulkanAllocator const& allocator, VulkanDevice const& device,
                         VkDeviceSize size, VkBufferUsageFlags usage,
                         void const* data = nullptr);

        // Host-visible and persistently mapped, for data rewritten every frame.
        // Written sequentially from the CPU. Reading it back is legal but may
        // be uncached; only the one-off frame capture does.
        bool init_mapped(VulkanAllocator const& allocator, VkDeviceSize size,
                         VkBufferUsageFlags usage);

        void shutdown();

        VkBuffer     handle() const { return buffer_; }
        VkDeviceSize size() const { return size_; }

        // Null unless created with init_mapped.
        void* mapped() const { return mapped_; }

    private:
        VulkanAllocator const* allocator_  = nullptr;
        VkBuffer               buffer_     = VK_NULL_HANDLE;
        VmaAllocation          allocation_ = nullptr;
        VkDeviceSize           size_       = 0;
        void*                  mapped_     = nullptr;
    };
}
