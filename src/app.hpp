#pragma once

#include "platform/window.hpp"
#include "render/renderer.hpp"
#include "render/scene.hpp"
#include "vulkan/allocator.hpp"
#include "vulkan/context.hpp"
#include "vulkan/device.hpp"
#include "vulkan/swapchain.hpp"

namespace encke
{
    class App
    {
    public:
        App() = default;
        ~App();

        App(App const&)            = delete;
        App& operator=(App const&) = delete;
        App(App&&)                 = delete;
        App& operator=(App&&)      = delete;

        bool init();
        void run();

    private:
        // Returns false if the window has no drawable area, in which case the
        // swapchain is left alone until it does.
        bool rebuild_swapchain();

        bool swapchain_matches_window() const;

        void report_throughput();

        // Declaration order is destruction order reversed: renderer, then
        // swapchain, then allocator, then device, then instance, then window.
        // The allocator must outlive every buffer and image the renderer owns.
        Window          window_;
        VulkanContext   vulkan_;
        VulkanDevice    device_;
        VulkanAllocator allocator_;
        VulkanSwapchain swapchain_;
        Renderer        renderer_;

        Scene scene_;

        // Set from ENCKE_FIXED_TIME; pins animation so captures are comparable.
        optional<f64> fixed_time_;

        u64 frames_          = 0;
        i64 last_report_ms_  = 0;
    };
}
