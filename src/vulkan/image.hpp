#pragma once

VK_DEFINE_HANDLE(VmaAllocation)

namespace encke
{
    class VulkanAllocator;
    class VulkanDevice;

    // A 2D, single-mip, device-local image with one view. Covers every
    // swapchain-sized target: G-buffer channels, the HDR accumulation buffer,
    // depth. Sized at creation and rebuilt wholesale on resize.
    class Image
    {
    public:
        struct Config
        {
            VkFormat           format = VK_FORMAT_UNDEFINED;
            VkImageUsageFlags  usage  = 0;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            char const*        name   = "image";
        };

        Image() = default;
        ~Image();

        Image(Image const&)            = delete;
        Image& operator=(Image const&) = delete;
        Image(Image&&)                 = delete;
        Image& operator=(Image&&)      = delete;

        bool init(VulkanAllocator const& allocator, VulkanDevice const& device,
                  Config const& config, VkExtent2D extent);

        // Destroys and rebuilds at the new size. The caller must have waited for
        // the device to go idle. The view changes, so any bindless slot pointing
        // at it must be rewritten.
        bool resize(VkExtent2D extent);

        void shutdown();

        VkImage     handle() const { return image_; }
        VkImageView view() const { return view_; }
        VkFormat    format() const { return config_.format; }

    private:
        bool build(VkExtent2D extent);
        void destroy();

        VulkanAllocator const* allocator_  = nullptr;
        VulkanDevice const*    device_     = nullptr;
        Config                 config_;
        VkImage                image_      = VK_NULL_HANDLE;
        VkImageView            view_       = VK_NULL_HANDLE;
        VmaAllocation          allocation_ = nullptr;
    };
}
