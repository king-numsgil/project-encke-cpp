#include "core/pch.hpp"

#include "app.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"

#include <cstdlib>

namespace encke
{
    namespace
    {
        constexpr char kTitle[] = "encke";
        constexpr i32  kWidth   = 1280;
        constexpr i32  kHeight  = 720;

        constexpr i64 kReportIntervalMs = 1000;

        constexpr u32 kDebugViewCount = 5;

        char const* debug_view_name(DebugView view)
        {
            switch (view)
            {
            case DebugView::Lit:         return "lit (clustered)";
            case DebugView::BruteForce:  return "lit (brute force)";
            case DebugView::ClusterHeat: return "lights per cluster";
            case DebugView::Normals:     return "normals";
            case DebugView::Motion:      return "motion vectors";
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

        // ENCKE_DEBUG_VIEW picks the starting view, so a debug view can be
        // captured without a keypress.
        DebugView initial_debug_view()
        {
            char const* const value = std::getenv("ENCKE_DEBUG_VIEW");
            if (value == nullptr)
            {
                return DebugView::Lit;
            }

            long const parsed = std::strtol(value, nullptr, 10);
            if (parsed < 0 || parsed >= static_cast<long>(kDebugViewCount))
            {
                return DebugView::Lit;
            }
            return static_cast<DebugView>(parsed);
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

        scene_.build_test_corridor();
        log::info("scene: %zu objects, %zu lights", scene_.objects.size(), scene_.lights.size());

        fixed_time_ = fixed_time();
        if (fixed_time_.has_value())
        {
            log::info("animation pinned to t=%.3f s", *fixed_time_);
        }

        renderer_.set_debug_view(initial_debug_view());
        log::info("debug view: %s (keys 1-%u to switch)", debug_view_name(renderer_.debug_view()),
                  kDebugViewCount);

        last_report_ms_ = log::elapsed_ms();
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

    void App::report_throughput()
    {
        i64 const now = log::elapsed_ms();
        i64 const span = now - last_report_ms_;
        if (span < kReportIntervalMs)
        {
            return;
        }

        double const fps = static_cast<double>(frames_) * 1000.0 / static_cast<double>(span);
        log::info("%llu frames in %lld ms (%.1f fps)",
                  static_cast<unsigned long long>(frames_),
                  static_cast<long long>(span), fps);

        frames_         = 0;
        last_report_ms_ = now;
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

            if (events.debug_view >= 0 && static_cast<u32>(events.debug_view) < kDebugViewCount)
            {
                renderer_.set_debug_view(static_cast<DebugView>(events.debug_view));
                log::info("debug view: %s", debug_view_name(renderer_.debug_view()));
            }

            if (window_.minimized())
            {
                // Nothing presentable; block rather than spin.
                SDL_WaitEvent(nullptr);
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
            scene_.update(seconds);

            switch (renderer_.draw(swapchain_, scene_))
            {
            case FrameResult::Ok:
                ++frames_;
                report_throughput();
                break;

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
