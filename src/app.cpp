#include "pch.hpp"

#include "app.hpp"

#include "log.hpp"

namespace encke
{
    namespace
    {
        constexpr char kTitle[]  = "encke";
        constexpr i32  kWidth    = 1280;
        constexpr i32  kHeight   = 720;

        // Nothing is presented yet, so the loop would otherwise spin a core.
        constexpr Uint32 kIdleDelayMs = 16;
    }

    bool App::init()
    {
        log::init();

        return window_.init(kTitle, kWidth, kHeight) && vulkan_.init(window_);
    }

    void App::on_resize()
    {
        i32vec2 const size = window_.pixel_size();

        // Zero on either axis is not a legal swapchain extent. It happens while
        // minimised and transiently during a drag on some window managers.
        if (size.x == 0 || size.y == 0)
        {
            log::info("drawable %dx%d -- skipping", size.x, size.y);
            return;
        }

        log::info("drawable resized to %dx%d", size.x, size.y);
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

            if (events.resized)
            {
                on_resize();
            }

            if (window_.minimized())
            {
                // Wait for the next event instead of burning cycles on frames
                // that cannot be presented.
                SDL_WaitEvent(nullptr);
                continue;
            }

            SDL_Delay(kIdleDelayMs);
        }

        log::info("shutting down");
    }
}
