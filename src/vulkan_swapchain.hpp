#pragma once

namespace encke
{
    class VulkanContext;
    class VulkanDevice;

    class VulkanSwapchain
    {
    public:
        VulkanSwapchain() = default;
        ~VulkanSwapchain();

        VulkanSwapchain(VulkanSwapchain const&)            = delete;
        VulkanSwapchain& operator=(VulkanSwapchain const&) = delete;
        VulkanSwapchain(VulkanSwapchain&&)                 = delete;
        VulkanSwapchain& operator=(VulkanSwapchain&&)      = delete;

        bool init(VulkanContext const& context, VulkanDevice const& device, i32vec2 size);

        // Tears down the views and builds a new chain for the new size. The
        // caller must have waited for the device to go idle first.
        bool recreate(i32vec2 size);

        void shutdown();

        VkSwapchainKHR handle() const { return swapchain_; }
        VkFormat       format() const { return format_; }
        VkExtent2D     extent() const { return extent_; }
        u32            image_count() const { return static_cast<u32>(images_.size()); }
        VkImage        image(u32 index) const { return images_[index]; }
        VkImageView    view(u32 index) const { return views_[index]; }

    private:
        bool build(i32vec2 size);
        void destroy_views();

        VulkanContext const* context_ = nullptr;
        VulkanDevice const*  device_  = nullptr;

        VkSwapchainKHR      swapchain_ = VK_NULL_HANDLE;
        VkFormat            format_    = VK_FORMAT_UNDEFINED;
        VkExtent2D          extent_{};
        vector<VkImage>     images_;
        vector<VkImageView> views_;
    };
}
