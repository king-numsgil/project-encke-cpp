#pragma once

namespace encke
{
    class VulkanContext;

    struct QueueFamilies
    {
        optional<u32> graphics;
        optional<u32> present;

        bool complete() const { return graphics.has_value() && present.has_value(); }
    };

    // Physical device selection plus the logical device and its queues.
    class VulkanDevice
    {
    public:
        VulkanDevice() = default;
        ~VulkanDevice();

        VulkanDevice(VulkanDevice const&)            = delete;
        VulkanDevice& operator=(VulkanDevice const&) = delete;
        VulkanDevice(VulkanDevice&&)                 = delete;
        VulkanDevice& operator=(VulkanDevice&&)      = delete;

        bool init(VulkanContext const& context);
        void shutdown();

        // Blocks until the device is idle. Required before destroying anything
        // a command buffer might still reference.
        void wait_idle() const;

        // Records and runs one command buffer on the graphics queue, blocking
        // until it retires. For startup-time work -- staging uploads, layout
        // transitions -- never for anything per-frame.
        bool submit_immediate(function<void(VkCommandBuffer)> const& record) const;

        VkPhysicalDevice     physical() const { return physical_; }
        VkDevice             handle() const { return device_; }
        VkQueue              graphics_queue() const { return graphics_queue_; }
        VkQueue              present_queue() const { return present_queue_; }
        QueueFamilies const& families() const { return families_; }

        VkPhysicalDeviceProperties const& properties() const { return properties_; }

        // Meaningful bits in a timestamp written on the graphics queue. Zero
        // means that queue cannot write timestamps at all.
        u32 timestamp_valid_bits() const { return timestamp_valid_bits_; }

        // VK_KHR_swapchain_mutable_format is enabled, so the swapchain may
        // create views in a format other than the one it was made with.
        bool mutable_swapchain_format() const { return mutable_swapchain_format_; }

    private:
        VkPhysicalDevice physical_       = VK_NULL_HANDLE;
        VkDevice         device_         = VK_NULL_HANDLE;
        VkQueue          graphics_queue_ = VK_NULL_HANDLE;
        VkQueue          present_queue_  = VK_NULL_HANDLE;
        QueueFamilies    families_;

        VkPhysicalDeviceProperties properties_{};
        u32                        timestamp_valid_bits_     = 0;
        bool                       mutable_swapchain_format_ = false;

        // Transient pool for submit_immediate.
        VkCommandPool    upload_pool_    = VK_NULL_HANDLE;
    };
}
