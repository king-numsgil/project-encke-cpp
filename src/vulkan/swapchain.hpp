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

        // The view and format the UI draws through. UNORM over the sRGB
        // images where the device allows it, because UI colours are authored
        // already encoded; otherwise the same as view() and format().
        VkFormat    ui_format() const { return ui_format_; }
        VkImageView ui_view(u32 index) const
        {
            return ui_views_.empty() ? views_[index] : ui_views_[index];
        }

        // Images can be copied from (TRANSFER_SRC), for captures.
        bool readable() const { return readable_; }

        VkPresentModeKHR present_mode() const { return present_mode_; }
        char const*      present_mode_name() const;

    private:
        bool build(i32vec2 size);
        bool create_views(vector<VkImageView>& views, VkFormat format);
        void destroy_views();

        VulkanContext const* context_ = nullptr;
        VulkanDevice const*  device_  = nullptr;

        VkSwapchainKHR      swapchain_    = VK_NULL_HANDLE;
        VkFormat            format_       = VK_FORMAT_UNDEFINED;
        VkFormat            ui_format_    = VK_FORMAT_UNDEFINED;
        VkPresentModeKHR    present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
        VkExtent2D          extent_{};
        bool                readable_     = false;
        vector<VkImage>     images_;
        vector<VkImageView> views_;
        vector<VkImageView> ui_views_;
    };
}
