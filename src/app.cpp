#include "core/pch.hpp"

#include "app.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "render/gltf.hpp"
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

        // ENCKE_CAMERA="px py pz tx ty tz" starts the camera at p looking at t,
        // both in metres from the test planet's pole, so a capture can frame
        // something other than the default view. Commas work as separators too.
        struct CameraPose
        {
            f64vec3 eye;
            f64vec3 target;
        };

        optional<CameraPose> initial_camera()
        {
            char const* const value = std::getenv("ENCKE_CAMERA");
            if (value == nullptr)
            {
                return nullopt;
            }

            array<f64, 6> numbers{};
            char const*   cursor = value;
            for (f64& number : numbers)
            {
                while (*cursor == ' ' || *cursor == ',')
                {
                    ++cursor;
                }

                char* end = nullptr;
                number    = std::strtod(cursor, &end);
                if (end == cursor)
                {
                    log::warn("ENCKE_CAMERA wants six numbers, \"px py pz tx ty tz\"; ignored");
                    return nullopt;
                }
                cursor = end;
            }

            return CameraPose{.eye    = f64vec3{numbers[0], numbers[1], numbers[2]},
                              .target = f64vec3{numbers[3], numbers[4], numbers[5]}};
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

        // Not fatal: without it the scene is only missing the helmet.
        optional<Model> helmet;
        {
            GltfModel file;
            if (load_gltf(asset_path("models/DamagedHelmet/DamagedHelmet.glb"), file))
            {
                helmet = renderer_.add_model(file);
            }
            if (!helmet.has_value())
            {
                log::warn("the helmet did not load; the table stays empty");
            }
        }

        scene_.build_test_planet(helmet.has_value() ? &*helmet : nullptr);

        // Yaw about +Y, then pitch, the way FlyCamera composes them, so
        // flying on from here does not snap.
        if (optional<CameraPose> const pose = initial_camera())
        {
            f64vec3 const forward = glm::normalize(pose->target - pose->eye);
            f64 const     yaw     = std::atan2(-forward.x, -forward.z);
            f64 const     pitch   = std::asin(std::clamp(forward.y, -1.0, 1.0));

            scene_.camera.position    = scene_.origin() + pose->eye;
            scene_.camera.orientation = glm::angleAxis(yaw, f64vec3{0.0, 1.0, 0.0}) *
                                        glm::angleAxis(pitch, f64vec3{1.0, 0.0, 0.0});
            scene_.previous_camera    = scene_.camera;
        }
        fly_.reset(scene_.camera);
        log::info("scene: %zu objects, %zu lights", scene_.objects.size(), scene_.lights.size());

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
        log::info("debug view: %s (keys 1-2 shade, 3-%u toggle windows, F1 toggles the UI)",
                  debug_view_name(renderer_.debug_view()), kDebugKeyCount);
        log::info("camera: hold right mouse to look; WASD move, E/Q up/down, "
                  "Shift fast, Ctrl slow, wheel scales speed");

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

        FlyInput const input{
            .look  = events.mouse_delta,
            .move  = f64vec3{axis(SDL_SCANCODE_D, SDL_SCANCODE_A),
                             axis(SDL_SCANCODE_E, SDL_SCANCODE_Q),
                             axis(SDL_SCANCODE_W, SDL_SCANCODE_S)},
            .wheel = events.wheel,
            .fast  = window_.key_down(SDL_SCANCODE_LSHIFT) || window_.key_down(SDL_SCANCODE_RSHIFT),
            .slow  = window_.key_down(SDL_SCANCODE_LCTRL) || window_.key_down(SDL_SCANCODE_RCTRL),
        };

        fly_.update(scene_.camera, input, seconds);
    }

    void App::draw_ui()
    {
        ui_.begin_frame();

        if (show_ui_)
        {
            stats_.draw(StatsWindow::Info{
                .device       = device_.properties().deviceName,
                .present_mode = swapchain_.present_mode_name(),
                .extent       = swapchain_.extent(),
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

    void App::run()
    {
        bool running = true;

        while (running)
        {
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

            scene_.update(seconds);
            fly(events, delta);

            draw_ui();

            // With animation pinned, exposure jumps straight to the metered
            // value, so two runs' captures match however long each took to
            // start.
            renderer_.set_frame_time(delta, fixed_time_.has_value());

            bool const capturing = capture_path_.has_value() && frames_drawn_ == capture_frame_;
            if (capturing)
            {
                renderer_.request_capture();
            }

            FrameResult const result = renderer_.draw(swapchain_, scene_, &ui_);

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
                    stats_.record(stamp, frame_ms, renderer_.blocked_ms(),
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
