#include "core/pch.hpp"

#include "platform/window.hpp"

#include "core/log.hpp"

namespace encke
{
    Window::~Window()
    {
        shutdown();
    }

    bool Window::init(char const* title, i32 width, i32 height)
    {
        // Joysticks report as SDL_EVENT_JOYSTICK_* through the same pump,
        // including one ADDED event per device already connected.
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK))
        {
            log::sdl_error("SDL_Init");
            return false;
        }
        sdl_started_ = true;

        window_ = SDL_CreateWindow(title, width, height,
                                   SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr)
        {
            log::sdl_error("SDL_CreateWindow");
            return false;
        }

        i32vec2 const size = pixel_size();
        log::info("window \"%s\" %dx%d px", title, size.x, size.y);
        return true;
    }

    void Window::shutdown()
    {
        if (window_ != nullptr)
        {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }

        if (sdl_started_)
        {
            SDL_Quit();
            sdl_started_ = false;
        }
    }

    i32vec2 Window::pixel_size() const
    {
        if (window_ == nullptr)
        {
            return i32vec2{0, 0};
        }

        int width  = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window_, &width, &height))
        {
            log::sdl_error("SDL_GetWindowSizeInPixels");
            return i32vec2{0, 0};
        }

        return i32vec2{width, height};
    }

    void Window::set_relative_mouse(bool relative)
    {
        if (window_ != nullptr && !SDL_SetWindowRelativeMouseMode(window_, relative))
        {
            log::sdl_error("SDL_SetWindowRelativeMouseMode");
        }
    }

    bool Window::key_down(SDL_Scancode key) const
    {
        int         count = 0;
        bool const* keys  = SDL_GetKeyboardState(&count);
        return static_cast<int>(key) < count && keys[static_cast<size_t>(key)];
    }

    FrameEvents Window::poll()
    {
        FrameEvents events;

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (hook_)
            {
                hook_(event);
            }

            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                events.quit_requested = true;
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (event.window.windowID == SDL_GetWindowID(window_))
                {
                    events.quit_requested = true;
                }
                break;

            // RESIZED fires for logical size; PIXEL_SIZE_CHANGED is the one
            // that matters for a swapchain, and it also fires when the window
            // moves between displays of different scale.
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                events.resized = true;
                break;

            case SDL_EVENT_WINDOW_MINIMIZED:
                minimized_ = true;
                break;

            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_MAXIMIZED:
                minimized_ = false;
                events.resized = true;
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                events.focus_lost = true;
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                events.focus_gained = true;
                break;

            // Keys, the mouse and joysticks reach the game as actions,
            // through the hook (input/sdl_input).
            default:
                break;
            }
        }

        return events;
    }
}
