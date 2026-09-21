#pragma once

#include <chrono>

#include "platform/window.hpp"
#include "render/renderer.hpp"
#include "render/scene.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/stats_window.hpp"
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

        void draw_ui();

        // A digit key, 0-based: a shading mode, or a debug window toggle.
        void on_debug_key(u32 key);

        // Declaration order is destruction order reversed: UI, renderer,
        // swapchain, allocator, device, instance, window. The allocator must
        // outlive every buffer and image the renderer owns, and the UI's
        // backends need both the device and the SDL window.
        Window          window_;
        VulkanContext   vulkan_;
        VulkanDevice    device_;
        VulkanAllocator allocator_;
        VulkanSwapchain swapchain_;
        Renderer        renderer_;
        ImGuiLayer      ui_;

        StatsWindow stats_;
        bool        show_ui_ = true;

        array<bool, kDebugWindowCount> debug_windows_{};
        f32                            motion_gain_ = 5000.0f;

        Scene scene_;

        // Set from ENCKE_FIXED_TIME; pins animation so captures are comparable.
        optional<f64> fixed_time_;

        // Wall-clock stamp of the previous completed frame, for frame time.
        optional<std::chrono::steady_clock::time_point> last_frame_;
    };
}
