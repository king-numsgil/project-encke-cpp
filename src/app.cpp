#include "core/pch.hpp"

#include "app.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "platform/cpu.hpp"
#include "render/config.hpp"
#include "render/pixels.hpp"
#include "ui/image_window.hpp"

#include <algorithm>
#include <cstdlib>

#include <imgui.h>

namespace encke
{
    namespace
    {
        constexpr char kTitle[] = "encke";
        constexpr i32  kWidth   = 1280;
        constexpr i32  kHeight  = 720;

        // Keys 1 and 2 pick how the frame is shaded; the keys after them
        // toggle one visualisation window each. ENCKE_DEBUG_VIEW uses the
        // same numbering from zero.
        constexpr u32 kShadingModeCount = 2;
        constexpr u32 kDebugKeyCount    = kShadingModeCount + kDebugWindowCount;

        // The motion gain slider's range. Motion is per-frame displacement:
        // the top end suits an uncapped couple of thousand frames a second,
        // the bottom a frame-limited 60.
        constexpr f32 kMotionGainMin = 10.0f;
        constexpr f32 kMotionGainMax = 100000.0f;

        // Longest frame the camera and exposure will integrate over at once.
        constexpr f64 kMaxFrameSeconds = 0.1;

        // The frame limiter's rate, when it is on.
        constexpr f64 kFrameLimitHz = 60.0;

        // Where each debug window first opens: beside the stats window and
        // clear of one another at the default 1280x720, so opening them all
        // does not stack them.
        f32vec2 debug_window_position(u32 index)
        {
            constexpr f32 kLeft   = 470.0f;
            constexpr f32 kTop    = 10.0f;
            constexpr f32 kStepX  = 410.0f;
            constexpr f32 kStepY  = 270.0f;

            return f32vec2{kLeft + kStepX * static_cast<f32>(index % 2u),
                           kTop + kStepY * static_cast<f32>(index / 2u)};
        }

        char const* debug_view_name(DebugView view)
        {
            switch (view)
            {
            case DebugView::Lit:        return "lit (clustered)";
            case DebugView::BruteForce: return "lit (brute force)";
            }
            return "unknown";
        }

        char const* debug_window_title(DebugWindow window)
        {
            switch (window)
            {
            case DebugWindow::ClusterHeat: return "Lights per cluster";
            case DebugWindow::Normals:     return "Normals";
            case DebugWindow::Motion:      return "Motion vectors";
            case DebugWindow::Cascades:    return "Shadow cascades";
            }
            return "unknown";
        }

        // ENCKE_FIXED_TIME pins animation to one instant, so two runs render
        // byte-identical frames. Without it, comparing captures across runs
        // compares different camera positions rather than what changed.
        optional<f64> fixed_time()
        {
            char const* const value = std::getenv("ENCKE_FIXED_TIME");
            if (value == nullptr)
            {
                return nullopt;
            }
            return std::strtod(value, nullptr);
        }

        // ENCKE_DEBUG_VIEW picks a starting shading mode or opens a window, so
        // either can be captured without a keypress. Same numbering as the
        // keys, from zero.
        optional<u32> initial_debug_key()
        {
            char const* const value = std::getenv("ENCKE_DEBUG_VIEW");
            if (value == nullptr)
            {
                return nullopt;
            }

            long const parsed = std::strtol(value, nullptr, 10);
            if (parsed < 0 || parsed >= static_cast<long>(kDebugKeyCount))
            {
                return nullopt;
            }
            return static_cast<u32>(parsed);
        }

        char const* tonemap_name(Tonemap tonemap)
        {
            switch (tonemap)
            {
            case Tonemap::Aces:       return "ACES (Narkowicz fit)";
            case Tonemap::AgX:        return "AgX";
            case Tonemap::PbrNeutral: return "PBR Neutral";
            }
            return "unknown";
        }

        // ENCKE_TONEMAP picks the starting curve, numbered as Tonemap; T
        // cycles it.
        optional<Tonemap> initial_tonemap()
        {
            char const* const value = std::getenv("ENCKE_TONEMAP");
            if (value == nullptr)
            {
                return nullopt;
            }

            long const parsed = std::strtol(value, nullptr, 10);
            if (parsed < 0 || parsed >= static_cast<long>(kTonemapCount))
            {
                return nullopt;
            }
            return static_cast<Tonemap>(parsed);
        }

        // ENCKE_EV100 pins the exposure the frame is tonemapped at, so two
        // captures differ only in what is being compared.
        optional<f32> fixed_ev100()
        {
            char const* const value = std::getenv("ENCKE_EV100");
            if (value == nullptr)
            {
                return nullopt;
            }
            return static_cast<f32>(std::strtod(value, nullptr));
        }

        // ENCKE_CAMERA="px py pz tx ty tz [ux uy uz]" starts the camera at p
        // looking at t, its up toward u, all in the test scene's frame: metres
        // from the pole, +Y up by the transform convention. u defaults to that
        // +Y. Commas work as separators too.
        struct CameraPose
        {
            f64vec3 eye;
            f64vec3 target;
            f64vec3 up;
        };

        optional<CameraPose> initial_camera()
        {
            char const* const value = std::getenv("ENCKE_CAMERA");
            if (value == nullptr)
            {
                return nullopt;
            }

            array<f64, 9> numbers{{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0}};
            char const*   cursor = value;
            for (size_t index = 0; index < numbers.size(); ++index)
            {
                while (*cursor == ' ' || *cursor == ',')
                {
                    ++cursor;
                }

                char* end = nullptr;
                f64 const number = std::strtod(cursor, &end);
                if (end == cursor)
                {
                    // Six is complete, the up is optional; anything else is not.
                    if (index == 6 && *cursor == '\0')
                    {
                        break;
                    }
                    log::warn("ENCKE_CAMERA wants \"px py pz tx ty tz\" with an optional "
                              "\"ux uy uz\"; ignored");
                    return nullopt;
                }
                numbers[index] = number;
                cursor         = end;
            }

            return CameraPose{.eye    = f64vec3{numbers[0], numbers[1], numbers[2]},
                              .target = f64vec3{numbers[3], numbers[4], numbers[5]},
                              .up     = f64vec3{numbers[6], numbers[7], numbers[8]}};
        }
    }

    App::~App()
    {
        // Members are destroyed after this body runs, so this is the last
        // chance to be sure no command buffer is still executing.
        device_.wait_idle();
    }

    bool App::init()
    {
        log::init();
        log::info("allocator: %s", memory::backend_name());

        CpuInfo const cpu = query_cpu();
        log::info("cpu: %u physical cores, %u logical processors", cpu.physical_cores,
                  cpu.logical_processors);

        // Must precede SDL_Init, which window_.init performs.
        if (!memory::install_sdl_allocator())
        {
            return false;
        }

        if (!window_.init(kTitle, kWidth, kHeight))
        {
            return false;
        }

        if (!vulkan_.init(window_) || !device_.init(vulkan_))
        {
            return false;
        }

        if (!allocator_.init(vulkan_, device_))
        {
            return false;
        }

        if (!swapchain_.init(vulkan_, device_, window_.pixel_size()))
        {
            return false;
        }

        if (!renderer_.init(allocator_, device_, swapchain_))
        {
            return false;
        }

        if (!ui_.init(window_, allocator_, device_, renderer_.bindless(), swapchain_,
                      Renderer::kFramesInFlight))
        {
            return false;
        }
        window_.set_event_hook([this](SDL_Event const& event) { ui_.process_event(event); });

        assets_.start();
        scene_.build_test_planet(assets_, config::kTerrainFinestLod);

        // One core left for this thread and the driver's.
        u32 const workers = std::max(cpu.physical_cores, 2u) - 1;
        pool_.start(workers, "terrain");
        log::info("worker pool: %u threads", pool_.size());

        // The test scene's frame is the world's rotated by nothing, only
        // moved to the pole, so its directions pass through unchanged.
        if (optional<CameraPose> const pose = initial_camera())
        {
            // The test scene's camera is a root, so its local transform is
            // its world one.
            Transform& camera = scene_.registry.get<Transform>(scene_.camera);
            camera.position   = scene_.origin() + pose->eye;
            fly_.aim(camera, pose->target - pose->eye, pose->up);
        }
        log::info("scene: %zu entities, %zu renderables, %zu lights",
                  scene_.registry.view<Transform>().size(),
                  scene_.registry.view<Renderable>().size(), scene_.registry.view<Light>().size());

        fixed_time_ = fixed_time();
        if (fixed_time_.has_value())
        {
            log::info("animation pinned to t=%.3f s", *fixed_time_);
        }

        // Any overlay makes two captures differ, which defeats comparing the
        // clustered and brute-force views byte for byte.
        if (std::getenv("ENCKE_NO_UI") != nullptr)
        {
            show_ui_ = false;
        }

        if (optional<u32> const key = initial_debug_key())
        {
            on_debug_key(*key);
        }

        if (optional<Tonemap> const tonemap = initial_tonemap())
        {
            renderer_.set_tonemap(*tonemap);
        }
        log::info("tonemap: %s (T cycles)", tonemap_name(renderer_.tonemap()));

        // Read back from the renderer rather than grabbed off the desktop, so
        // other windows and the compositor cannot get into it.
        if (char const* const path = std::getenv("ENCKE_CAPTURE"))
        {
            capture_path_ = string{path};

            // Late enough for pinned exposure to have metered and every
            // frame in flight to have been through the renderer once.
            constexpr u32 kDefaultCaptureFrame = 10;
            char const* const frame = std::getenv("ENCKE_CAPTURE_FRAME");
            capture_frame_ = frame != nullptr ? static_cast<u32>(std::strtoul(frame, nullptr, 10))
                                              : kDefaultCaptureFrame;
            log::info("capturing frame %u to %s, then quitting", capture_frame_, path);
        }

        if (optional<f32> const ev100 = fixed_ev100())
        {
            renderer_.set_fixed_ev100(ev100);
            log::info("exposure pinned to EV100 %.2f", static_cast<f64>(*ev100));
        }

        if (std::getenv("ENCKE_NO_GEOMORPH") != nullptr)
        {
            renderer_.set_geomorph(false);
            log::info("geomorph off");
        }
        log::info("debug view: %s (keys 1-2 shade, 3-%u toggle windows, F1 toggles the UI)",
                  debug_view_name(renderer_.debug_view()), kDebugKeyCount);
        log::info("camera: hold right mouse to look; WASD move, Space/Ctrl up/down, "
                  "Q/E roll, Shift fast, Alt slow, wheel scales speed");

        return true;
    }

    bool App::swapchain_matches_window() const
    {
        i32vec2 const    size    = window_.pixel_size();
        VkExtent2D const current = swapchain_.extent();

        return static_cast<u32>(size.x) == current.width &&
               static_cast<u32>(size.y) == current.height;
    }

    bool App::rebuild_swapchain()
    {
        i32vec2 const size = window_.pixel_size();

        // Zero on either axis is not a legal extent. Happens while minimised
        // and transiently mid-drag on some window managers.
        if (size.x == 0 || size.y == 0)
        {
            return false;
        }

        if (!swapchain_.recreate(size))
        {
            return false;
        }

        return renderer_.on_swapchain_changed(swapchain_);
    }

    void App::on_debug_key(u32 key)
    {
        if (key < kShadingModeCount)
        {
            renderer_.set_debug_view(static_cast<DebugView>(key));
            log::info("debug view: %s", debug_view_name(renderer_.debug_view()));
            return;
        }

        u32 const window = key - kShadingModeCount;
        debug_windows_[window] = !debug_windows_[window];

        // Asking for a window is asking to see it, so a hidden UI comes back.
        if (debug_windows_[window])
        {
            show_ui_ = true;
        }
    }

    void App::set_looking(bool looking)
    {
        if (looking == looking_)
        {
            return;
        }

        looking_ = looking;
        window_.set_relative_mouse(looking);
        ui_.set_input_blocked(looking);
    }

    void App::fly(FrameEvents const& events, f64 seconds)
    {
        // A right click on a UI window belongs to the UI.
        if (events.look_pressed && !ui_.wants_mouse())
        {
            set_looking(true);
        }
        if (events.look_released)
        {
            set_looking(false);
        }

        if (!looking_)
        {
            return;
        }

        auto axis = [this](SDL_Scancode positive, SDL_Scancode negative) {
            return (window_.key_down(positive) ? 1.0 : 0.0) - (window_.key_down(negative) ? 1.0 : 0.0);
        };

        auto const either = [this](SDL_Scancode left, SDL_Scancode right) {
            return window_.key_down(left) || window_.key_down(right);
        };

        f64 const strafe_up = (window_.key_down(SDL_SCANCODE_SPACE) ? 1.0 : 0.0) -
                              (either(SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL) ? 1.0 : 0.0);

        FlyInput const input{
            .look  = events.mouse_delta,
            .move  = f64vec3{axis(SDL_SCANCODE_D, SDL_SCANCODE_A), strafe_up,
                             axis(SDL_SCANCODE_W, SDL_SCANCODE_S)},
            .roll  = axis(SDL_SCANCODE_Q, SDL_SCANCODE_E),
            .wheel = events.wheel,
            .fast  = either(SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT),
            .slow  = either(SDL_SCANCODE_LALT, SDL_SCANCODE_RALT),
        };

        if (Transform* const camera = scene_.registry.try_get<Transform>(scene_.camera))
        {
            fly_.update(*camera, input, seconds);
        }
    }

    void App::draw_ui()
    {
        ui_.begin_frame();

        if (show_ui_)
        {
            AssetWorker const&               asset_worker = assets_.worker();
            array<StatsWindow::Threads, 2> const threads{{
                {.name = "terrain", .loads = pool_.loads(), .queued = pool_.outstanding()},
                {.name   = "assets",
                 .loads  = span<ThreadLoad const>{&asset_worker.load(), 1},
                 .queued = asset_worker.outstanding()},
            }};

            stats_.draw(StatsWindow::Info{
                .device       = device_.properties().deviceName,
                .present_mode = swapchain_.present_mode_name(),
                .extent       = swapchain_.extent(),
                .limit_frames = &limit_frames_,
                .limit_hz     = kFrameLimitHz,
                .threads      = threads,
            });

            for (u32 index = 0; index < kDebugWindowCount; ++index)
            {
                auto const window = static_cast<DebugWindow>(index);

                function<void()> controls;
                if (window == DebugWindow::Motion)
                {
                    controls = [this] {
                        ImGui::SliderFloat("gain", &motion_gain_, kMotionGainMin, kMotionGainMax,
                                           "%.0f", ImGuiSliderFlags_Logarithmic);
                    };
                }

                image_window(debug_window_title(window), debug_windows_[index],
                             renderer_.debug_window_image(window), swapchain_.extent(),
                             debug_window_position(index), controls);
            }
        }

        ui_.end_frame();

        // After the UI, which may have closed a window this frame: the
        // renderer must draw exactly the images the draw data samples.
        for (u32 index = 0; index < kDebugWindowCount; ++index)
        {
            renderer_.set_debug_window_open(static_cast<DebugWindow>(index),
                                            show_ui_ && debug_windows_[index]);
        }
        renderer_.set_motion_gain(motion_gain_);
    }

    void App::save_capture()
    {
        // The copy is in the frame just submitted.
        device_.wait_idle();

        optional<Renderer::Capture> capture = renderer_.take_capture();
        if (!capture.has_value())
        {
            log::error("no capture: the swapchain cannot be copied from");
            return;
        }

        bool const bgra = capture->format == VK_FORMAT_B8G8R8A8_SRGB ||
                          capture->format == VK_FORMAT_B8G8R8A8_UNORM;
        bool const rgba = capture->format == VK_FORMAT_R8G8B8A8_SRGB ||
                          capture->format == VK_FORMAT_R8G8B8A8_UNORM;
        if (!bgra && !rgba)
        {
            log::error("no capture: swapchain format %d is not 8-bit RGBA or BGRA",
                       static_cast<int>(capture->format));
            return;
        }

        Pixels pixels{.width = capture->width, .height = capture->height,
                      .rgba = std::move(capture->pixels)};
        for (size_t at = 0; at < pixels.rgba.size(); at += 4)
        {
            if (bgra)
            {
                std::swap(pixels.rgba[at], pixels.rgba[at + 2]);
            }
            pixels.rgba[at + 3] = 255;   // the swapchain's alpha means nothing
        }

        if (save_png(*capture_path_, pixels))
        {
            log::info("captured %ux%u to %s", pixels.width, pixels.height, capture_path_->c_str());
        }
    }

    f64 App::limit_frame_rate()
    {
        using clock = std::chrono::steady_clock;

        if (!limit_frames_)
        {
            next_frame_.reset();
            return 0.0;
        }

        auto const period = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<f64>(1.0 / kFrameLimitHz));
        auto const start  = clock::now();

        if (next_frame_.has_value() && start < *next_frame_)
        {
            // SDL_DelayNS, not std::this_thread::sleep_for: on Windows SDL
            // waits on a high-resolution timer, where MinGW's sleep_for goes
            // through Sleep() and the default 15.6 ms tick.
            auto const wait = std::chrono::duration_cast<std::chrono::nanoseconds>(*next_frame_ - start);
            SDL_DelayNS(static_cast<u64>(wait.count()));
        }

        auto const woke = clock::now();

        // Deadlines advance by whole periods, so oversleeping one frame is
        // made up by the next and the average holds the rate. A frame that
        // ran more than a period late restarts the schedule from now rather
        // than racing to catch up.
        if (!next_frame_.has_value() || woke - *next_frame_ > period)
        {
            next_frame_ = woke + period;
        }
        else
        {
            *next_frame_ += period;
        }

        return std::chrono::duration<f64, std::milli>(woke - start).count();
    }

    void App::run()
    {
        bool running = true;

        while (running)
        {
            // First, so input is polled as late as possible before the frame
            // that uses it.
            f64 const slept_ms = limit_frame_rate();

            FrameEvents const events = window_.poll();

            if (events.quit_requested)
            {
                running = false;
                continue;
            }

            if (events.toggle_ui)
            {
                show_ui_ = !show_ui_;
            }

            if (events.cycle_tonemap && !ui_.wants_text())
            {
                auto const next = static_cast<Tonemap>(
                    (static_cast<u32>(renderer_.tonemap()) + 1u) % kTonemapCount);
                renderer_.set_tonemap(next);
                log::info("tonemap: %s", tonemap_name(next));
            }

            // A digit typed into a UI text field is not a view switch.
            if (events.debug_view >= 0 && static_cast<u32>(events.debug_view) < kDebugKeyCount &&
                !ui_.wants_text())
            {
                on_debug_key(static_cast<u32>(events.debug_view));
            }

            if (window_.minimized())
            {
                // Nothing presentable; block rather than spin. The gap is not
                // a frame, so do not let it register as one.
                SDL_WaitEvent(nullptr);
                last_frame_.reset();
                last_tick_.reset();
                next_frame_.reset();
                continue;
            }

            // A resize event fires once at startup for the initial size, and
            // again for any move between displays. Rebuilding an identical
            // swapchain is wasteful, so compare before acting.
            if (events.resized && !swapchain_matches_window())
            {
                device_.wait_idle();
                if (!rebuild_swapchain())
                {
                    continue;
                }
            }

            // f64: the scene integrates animation over session-long times, and
            // f32 seconds lose millisecond resolution after a few hours.
            f64 const seconds =
                fixed_time_.value_or(static_cast<f64>(log::elapsed_ms()) / 1000.0);

            // Wall-clock time since this same point last iteration, for flying
            // and exposure adaptation. Stamp to stamp at one fixed point in
            // the loop, so it spans exactly one whole frame; measuring from
            // where the previous draw() returned would span only the poll and
            // the UI, a jittering sliver of it. Clamped, so a hitch does not
            // fling the camera. Not pinned by ENCKE_FIXED_TIME: a pinned
            // camera still has to move at a real speed when flown.
            auto const tick  = std::chrono::steady_clock::now();
            f64        delta = 0.0;
            if (last_tick_.has_value())
            {
                delta = std::min(std::chrono::duration<f64>(tick - *last_tick_).count(),
                                 kMaxFrameSeconds);
            }
            last_tick_ = tick;

            // Flown first: the camera is an entity, and Scene::update is what
            // composes its world transform for this frame.
            fly(events, delta);
            assets_.update();
            // Finished terrain chunks become meshes here, and the octree picks
            // the chunks this camera wants and swaps them in and out, all
            // before the scene composes world transforms. It reads the
            // camera's world transform from the last update, a frame behind.
            pool_.drain();
            terrain_.update(scene_, assets_, pool_);
            scene_.update(seconds, assets_);

            draw_ui();

            // With animation pinned, exposure jumps straight to the metered
            // value, so two runs' captures match however long each took to
            // start.
            renderer_.set_frame_time(delta, fixed_time_.has_value());

            // Not before streaming has settled: until then which materials
            // and terrain chunks have landed depends on timing, and captures
            // would differ.
            bool const capturing = capture_path_.has_value() && frames_drawn_ >= capture_frame_ &&
                                   terrain_.idle() && assets_.idle() && renderer_.streaming_idle();
            if (capturing)
            {
                renderer_.request_capture();
            }

            FrameResult const result = renderer_.draw(swapchain_, scene_, assets_, &ui_);

            if (result == FrameResult::Ok)
            {
                ++frames_drawn_;
                if (capturing)
                {
                    save_capture();
                    running = false;
                }
            }

            switch (result)
            {
            case FrameResult::Ok:
            {
                // Wall clock, not `seconds`: ENCKE_FIXED_TIME pins animation,
                // not the passage of real time the stats are measuring.
                auto const now = std::chrono::steady_clock::now();
                if (last_frame_.has_value())
                {
                    f64 const frame_ms =
                        std::chrono::duration<f64, std::milli>(now - *last_frame_).count();
                    f64 const stamp =
                        std::chrono::duration<f64>(now.time_since_epoch()).count();
                    // The limiter's sleep is idle time, like the renderer's
                    // blocking, and must not count as CPU busy.
                    stats_.record(stamp, frame_ms, renderer_.blocked_ms() + slept_ms,
                                  renderer_.gpu_timings());
                }
                last_frame_ = now;
                break;
            }

            case FrameResult::OutOfDate:
                device_.wait_idle();
                rebuild_swapchain();
                break;

            case FrameResult::Error:
                log::error("frame failed; stopping");
                running = false;
                break;
            }
        }

        // Everything below unwinds through destructors; they must not run
        // while the GPU is still reading the resources they free.
        device_.wait_idle();
        log::info("shutting down");
    }
}
