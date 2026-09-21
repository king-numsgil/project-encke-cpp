#pragma once

namespace encke
{
    // What the event pump saw this frame. Consumed and discarded each tick.
    struct FrameEvents
    {
        bool quit_requested = false;

        // The drawable changed size. Once a swapchain exists this is the
        // signal to recreate it; the new size is Window::pixel_size().
        bool resized = false;

        // Digit key 1..9 pressed this frame, as 0..8; -1 when none. Drives
        // the renderer's debug views.
        i32 debug_view = -1;

        // F1 pressed this frame: show or hide the UI overlay.
        bool toggle_ui = false;
    };

    class Window
    {
    public:
        Window() = default;
        ~Window();

        Window(Window const&)            = delete;
        Window& operator=(Window const&) = delete;
        Window(Window&&)                 = delete;
        Window& operator=(Window&&)      = delete;

        bool init(char const* title, i32 width, i32 height);
        void shutdown();

        FrameEvents poll();

        // Sees every event poll() drains, before poll() interprets it. This is
        // how the UI gets its input without the window knowing about it.
        void set_event_hook(function<void(SDL_Event const&)> hook) { hook_ = std::move(hook); }

        SDL_Window* handle() const { return window_; }

        // Pixels, not logical units -- these differ on a scaled display and
        // the swapchain wants pixels.
        i32vec2 pixel_size() const;

        // A minimised window reports a zero-sized drawable, which is not a
        // legal swapchain extent. Callers skip rendering while this holds.
        bool minimized() const { return minimized_; }

    private:
        SDL_Window* window_        = nullptr;
        bool        sdl_started_   = false;
        bool        minimized_     = false;

        function<void(SDL_Event const&)> hook_;
    };
}
