#pragma once

namespace encke
{
    class Window;

    // volk loader, instance, optional validation, and the window surface.
    // No physical device or swapchain yet.
    class VulkanContext
    {
    public:
        VulkanContext() = default;
        ~VulkanContext();

        VulkanContext(VulkanContext const&)            = delete;
        VulkanContext& operator=(VulkanContext const&) = delete;
        VulkanContext(VulkanContext&&)                 = delete;
        VulkanContext& operator=(VulkanContext&&)      = delete;

        bool init(Window const& window);
        void shutdown();

        VkInstance   instance() const { return instance_; }
        VkSurfaceKHR surface() const { return surface_; }

        // False when validation was wanted but the layer is not installed.
        bool validation_active() const { return messenger_ != VK_NULL_HANDLE; }

    private:
        bool create_instance();
        bool create_messenger();
        bool create_surface(Window const& window);

        VkInstance               instance_  = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
        VkSurfaceKHR             surface_   = VK_NULL_HANDLE;
        bool                     validating_ = false;
    };

    // Vulkan's framebuffer origin is top-left with +Y down; GLM projects +Y up.
    // The flip lives here, in a negative-height viewport (core since Vulkan
    // 1.1), so projection matrices stay conventional and no call site has to
    // remember `proj[1][1] *= -1`.
    //
    // Not called yet -- there is no pipeline to bind it to. It exists so the
    // decision has one home when that lands.
    VkViewport flipped_viewport(f32 width, f32 height);
}
