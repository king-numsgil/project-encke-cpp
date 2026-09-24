#pragma once

#include <chrono>

#include "assets/asset_manager.hpp"
#include "platform/window.hpp"
#include "render/fly_camera.hpp"
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

        // Right mouse button: while held, the mouse and movement keys fly
        // the camera and the UI ignores them.
        void set_looking(bool looking);
        void fly(FrameEvents const& events, f64 seconds);

        // Writes the frame the renderer just captured to capture_path_.
        void save_capture();

        // Sleeps until the next frame's deadline while the limiter is on;
        // returns the milliseconds slept.
        f64 limit_frame_rate();

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

        // After the renderer, so destroyed before it; the renderer only reads
        // it during draw(). Its worker stops on destruction.
        AssetManager assets_;

        Scene     scene_;
        FlyCamera fly_;
        bool      looking_ = false;

        // Set from ENCKE_FIXED_TIME; pins animation so captures are comparable.
        optional<f64> fixed_time_;

        // Set from ENCKE_CAPTURE: save frame `capture_frame_` there as a PNG,
        // then quit.
        optional<string> capture_path_;
        u32              capture_frame_ = 0;
        u32              frames_drawn_  = 0;

        // Wall-clock stamp of the previous completed frame, for frame time.
        optional<std::chrono::steady_clock::time_point> last_frame_;

        // Stamped at the top of each simulated frame; the gap between two is
        // the time step the camera and exposure advance by.
        optional<std::chrono::steady_clock::time_point> last_tick_;

        // The CPU frame limiter: present mode stays MAILBOX, and the loop
        // sleeps to hold kFrameLimitHz. `next_frame_` is the deadline the
        // next iteration may start at.
        bool                                            limit_frames_ = true;
        optional<std::chrono::steady_clock::time_point> next_frame_;
    };
}
