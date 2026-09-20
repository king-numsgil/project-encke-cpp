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

        VkPhysicalDevice     physical() const { return physical_; }
        VkDevice             handle() const { return device_; }
        VkQueue              graphics_queue() const { return graphics_queue_; }
        VkQueue              present_queue() const { return present_queue_; }
        QueueFamilies const& families() const { return families_; }

    private:
        VkPhysicalDevice physical_       = VK_NULL_HANDLE;
        VkDevice         device_         = VK_NULL_HANDLE;
        VkQueue          graphics_queue_ = VK_NULL_HANDLE;
        VkQueue          present_queue_  = VK_NULL_HANDLE;
        QueueFamilies    families_;
    };
}
