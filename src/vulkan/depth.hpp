#pragma once

VK_DEFINE_HANDLE(VmaAllocation)

namespace encke
{
    class VulkanAllocator;
    class VulkanDevice;

    // Swapchain-sized depth attachment, using a REVERSED-Z convention:
    // near maps to 1.0, far to 0.0, cleared to 0.0, compared with GREATER.
    //
    // Float depth has its exponent bits concentrated near zero, and a standard
    // 0..1 mapping spends that precision on the far plane where it is useless.
    // Reversing puts it at the near plane and largely removes z-fighting in the
    // distance. It is free to adopt now and invasive later -- every projection
    // matrix, clear value and compare op has to agree.
    class DepthTarget
    {
    public:
        static constexpr VkFormat kFormat = VK_FORMAT_D32_SFLOAT;

        // Reversed-Z: clear to the far plane, which is now zero.
        static constexpr f32 kClearDepth = 0.0f;

        DepthTarget() = default;
        ~DepthTarget();

        DepthTarget(DepthTarget const&)            = delete;
        DepthTarget& operator=(DepthTarget const&) = delete;
        DepthTarget(DepthTarget&&)                 = delete;
        DepthTarget& operator=(DepthTarget&&)      = delete;

        bool init(VulkanAllocator const& allocator, VulkanDevice const& device,
                  VkExtent2D extent);

        // Destroys and rebuilds at the new size. The caller must have waited
        // for the device to go idle.
        bool resize(VkExtent2D extent);

        void shutdown();

        VkImage     image() const { return image_; }
        VkImageView view() const { return view_; }

    private:
        bool build(VkExtent2D extent);

        VulkanAllocator const* allocator_  = nullptr;
        VulkanDevice const*    device_     = nullptr;
        VkImage                image_      = VK_NULL_HANDLE;
        VkImageView            view_       = VK_NULL_HANDLE;
        VmaAllocation          allocation_ = nullptr;
    };
}
