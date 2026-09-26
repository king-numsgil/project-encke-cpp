#pragma once

#include <chrono>

#include "assets/asset_manager.hpp"
#include "platform/window.hpp"
#include "platform/worker_pool.hpp"
#include "render/config.hpp"
#include "render/fly_camera.hpp"
#include "render/renderer.hpp"
#include "render/scene.hpp"
#include "terrain/planet.hpp"
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

        // The camera at `eye` looking at `target`, metres from the scene's
        // origin, its up toward `up`.
        void place_camera(f64vec3 const& eye, f64vec3 const& target, f64vec3 const& up);

        // The camera's pose in ENCKE_CAMERA's format.
        string camera_pose_text() const;

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

        terrain::TerrainOctree terrain_{terrain::OctreeSettings{
            .finest_lod   = config::kTerrainFinestLod,
            .split_factor = config::kTerrainSplitFactor,
            .cull_factor  = config::kTerrainCullFactor,
            .map_layout   = terrain::SurfaceMapLayout{.face = config::kSurfaceMapFace, .border = config::kSurfaceMapBorder},
            .map_lod      = config::kSurfaceMapLod,
            .impostor_hysteresis = config::kImpostorHysteresis,
        }};

        // Last, so its threads stop before anything above goes: completions
        // reach the scene and the assets, and jobs the builder's state.
        WorkerPool pool_;

        // Set from ENCKE_FIXED_TIME; pins animation so captures are comparable.
        optional<f64> fixed_time_;

        // Set from ENCKE_CAPTURE: save frame `capture_frame_` there as a PNG,
        // then quit.
        optional<string> capture_path_;
        u32              capture_frame_ = 0;
        u32              frames_drawn_  = 0;
        optional<u32>    capture_at_;   // once settled: the frame to capture

        // Set from ENCKE_CAPTURE_MOVE: once settled, freeze the terrain and
        // move the camera to that pose before capturing, so seams can be
        // seen from where the octree did not choose its chunks.
        bool capture_move_ = false;

        // The camera's pose when F2 froze the terrain, for F3 to copy.
        optional<string> frozen_pose_;

        // Set from ENCKE_CAPTURE_FLY="vx vy vz": once settled, fly at that
        // velocity, metres per second at a fixed 60 steps a second, with the
        // octree live, and capture after ENCKE_CAPTURE_FRAME frames of it.
        // Whatever the octree has swapped by then depends on timing, so two
        // runs differ; this is for seeing what only shows in motion.
        optional<f64vec3> capture_velocity_;
        bool              flying_      = false;
        u32               fly_frames_  = 0;

        // Temporal antialiasing, toggled in the stats window; ENCKE_NO_TAA
        // starts with it off.
        bool taa_ = true;

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
