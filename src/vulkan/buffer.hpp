#pragma once

VK_DEFINE_HANDLE(VmaAllocation)

namespace encke
{
    class VulkanAllocator;
    class VulkanDevice;

    // A device-local buffer, filled once at creation through a staging copy.
    // Nothing here supports updating after the fact; per-frame data will want a
    // different type with host-visible memory and no staging round trip.
    class Buffer
    {
    public:
        Buffer() = default;
        ~Buffer();

        Buffer(Buffer const&)            = delete;
        Buffer& operator=(Buffer const&) = delete;
        Buffer(Buffer&&)                 = delete;
        Buffer& operator=(Buffer&&)      = delete;

        bool init(VulkanAllocator const& allocator, VulkanDevice const& device,
                  void const* data, size_t size, VkBufferUsageFlags usage);

        void shutdown();

        VkBuffer handle() const { return buffer_; }

    private:
        VulkanAllocator const* allocator_  = nullptr;
        VkBuffer               buffer_     = VK_NULL_HANDLE;
        VmaAllocation          allocation_ = nullptr;
    };
}
