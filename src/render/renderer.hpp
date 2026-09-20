#pragma once

#include "render/mesh.hpp"
#include "render/pipeline.hpp"
#include "vulkan/depth.hpp"

namespace encke
{
    class VulkanAllocator;
    class VulkanDevice;
    class VulkanSwapchain;

    enum class FrameResult
    {
        Ok,
        // The swapchain no longer matches the surface. The caller must wait
        // for idle and recreate it; this frame produced nothing.
        OutOfDate,
        Error,
    };

    class Renderer
    {
    public:
        Renderer() = default;
        ~Renderer();

        Renderer(Renderer const&)            = delete;
        Renderer& operator=(Renderer const&) = delete;
        Renderer(Renderer&&)                 = delete;
        Renderer& operator=(Renderer&&)      = delete;

        bool init(VulkanAllocator const& allocator, VulkanDevice const& device,
                  VulkanSwapchain const& swapchain);
        void shutdown();

        // Rebuilds the per-image semaphores and the depth target after the
        // swapchain changes.
        bool on_swapchain_changed(VulkanSwapchain const& swapchain);

        FrameResult draw(VulkanSwapchain const& swapchain, f32 seconds);

    private:
        bool record(VkCommandBuffer command, VulkanSwapchain const& swapchain, u32 image_index,
                    f32 seconds);
        void destroy_image_semaphores();

        static constexpr u32 kFramesInFlight = 2;

        VulkanDevice const* device_ = nullptr;

        GraphicsPipeline pipeline_;
        Mesh             cube_;
        DepthTarget      depth_;

        VkCommandPool command_pool_ = VK_NULL_HANDLE;

        array<VkCommandBuffer, kFramesInFlight> commands_{};
        array<VkSemaphore, kFramesInFlight>     image_available_{};
        array<VkFence, kFramesInFlight>         in_flight_{};

        // Signalled by the submit that renders into a given swapchain image.
        // One per image, not per frame-in-flight: a frame index can map to a
        // different image each time, and reusing a pending signal is invalid.
        vector<VkSemaphore> render_finished_;

        u32 frame_ = 0;
    };
}
