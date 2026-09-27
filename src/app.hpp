#pragma once

#include <chrono>
#include <filesystem>

#include "assets/asset_manager.hpp"
#include "platform/window.hpp"
#include "physics/physics_world.hpp"
#include "platform/worker_pool.hpp"
#include "render/config.hpp"
#include "render/fly_camera.hpp"
#include "render/renderer.hpp"
#include "render/scene.hpp"
#include "render/walk_camera.hpp"
#include "scenario/scenario.hpp"
#include "terrain/planet.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/stats_window.hpp"
#include "vulkan/allocator.hpp"
#include "vulkan/context.hpp"
#include "vulkan/device.hpp"
#include "vulkan/swapchain.hpp"

namespace encke
{
    struct AppOptions
    {
        // `--scenario path`: run that file's steps, then quit.
        optional<std::filesystem::path> scenario;
        // `--out dir`: where its captures go; by default out/<name> beside it.
        optional<std::filesystem::path> output;
    };

    class App
    {
    public:
        App() = default;
        ~App();

        App(App const&)            = delete;
        App& operator=(App const&) = delete;
        App(App&&)                 = delete;
        App& operator=(App&&)      = delete;

        bool init(AppOptions const& options);

        // The process's exit code: nonzero if a scenario step failed.
        int run();

    private:
        enum class StepResult
        {
            Running,   // continues next frame
            Done,
            Failed,
        };

        void apply_settings(scenario::Settings const& settings);

        // Runs the scenario's steps until one has to wait for a frame, before
        // anything this frame is updated. Returns whether this frame is to
        // be captured.
        bool tick_scenario();
        StepResult run_step(scenario::Step const& step);

        // Rigid bodies, drawn with the scene's cube or sphere and crate.
        void spawn(scenario::Spawn const& spawn);

        // After a frame the scenario asked to capture has been drawn.
        void finish_capture();

        // Returns false if the window has no drawable area, in which case the
        // swapchain is left alone until it does.
        bool rebuild_swapchain();

        bool swapchain_matches_window() const;

        void draw_ui();

        // Refills debug_lines_ with what the toggles below show, from this
        // frame's world transforms, and hands them to the renderer.
        void collect_debug_lines();

        // A digit key, 0-based: a shading mode, or a debug window toggle.
        void on_debug_key(u32 key);

        // While looking, the mouse and movement keys drive the camera and the
        // UI ignores them: the mouse is held, hidden, reporting raw motion.
        void set_looking(bool looking);

        // Looking unless Alt is down or the window lost focus, in either
        // movement. Never under a scenario.
        void capture_mouse();

        void fly(FrameEvents const& events, f64 seconds);

        // On foot, a character standing where the camera is, or the jetpack,
        // flying from where the character's eyes were.
        void set_movement(Movement movement);

        // While looking, the keys and the mouse fill walk_input_.
        void read_walk_input(FrameEvents const& events);

        // Writes the frame the renderer just captured to `path` as a PNG.
        bool save_capture(std::filesystem::path const& path);

        // The camera at `eye` looking at `target`, metres from the scene's
        // origin, its up toward `up`.
        void place_camera(f64vec3 const& eye, f64vec3 const& target, f64vec3 const& up);

        // The camera's pose as a scenario step, rounded for reading.
        scenario::Camera camera_step() const;

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

        // Walking unless a scenario says otherwise; X takes the jetpack.
        // walk_input_ is this frame's, from the keys or a walk step.
        Movement   movement_ = Movement::Jetpack;
        WalkCamera walk_;
        WalkInput  walk_input_;
        bool       focused_ = true;

        terrain::TerrainOctree terrain_{terrain::OctreeSettings{
            .finest_lod   = config::kTerrainFinestLod,
            .split_factor = config::kTerrainSplitFactor,
            .cull_factor  = config::kTerrainCullFactor,
            .map_layout   = terrain::SurfaceMapLayout{.face = config::kSurfaceMapFace, .border = config::kSurfaceMapBorder},
            .map_lod      = config::kSurfaceMapLod,
            .impostor_hysteresis = config::kImpostorHysteresis,
        }};

        // Before the pool, so the pool's workers, whose collision jobs use
        // its samplers, stop first.
        physics::PhysicsWorld physics_;

        // Last, so its threads stop before anything above goes: completions
        // reach the scene and the assets, and jobs the builder's state.
        WorkerPool pool_;

        // Pins animation so captures are comparable; a scenario always does.
        optional<f64> fixed_time_;

        // The scenario being run, while it runs. Steps run in order; the
        // current one has seen `step_frames_` frames before this one.
        optional<scenario::Scenario> scenario_;
        std::filesystem::path        scenario_output_;
        size_t                       step_index_   = 0;
        u32                          step_frames_  = 0;
        bool                         step_started_ = false;
        bool                         scenario_failed_ = false;

        // The physics step a walk step ends at.
        u64 walk_until_ = 0;

        // The camera's pose when F2 froze the terrain, for F3 to copy.
        optional<scenario::Camera> frozen_pose_;

        // Temporal antialiasing, toggled in the stats window.
        bool taa_ = true;

        // Debug drawing: collision shapes (F5) and the octree's chunks (F6);
        // the wireframe (F4) is the renderer's. Rebuilt every frame.
        bool       show_collision_ = false;
        bool       show_octree_    = false;
        DebugLines debug_lines_;

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
