#pragma once

namespace encke
{
    // What the event pump saw this frame about the window. Consumed and
    // discarded each tick. Input is not here: it reaches the game as
    // actions (input/).
    struct FrameEvents
    {
        // The window was closed.
        bool quit_requested = false;

        // The drawable changed size. Once a swapchain exists this is the
        // signal to recreate it; the new size is Window::pixel_size().
        bool resized = false;

        // The window gained or lost keyboard focus this frame. Losing it
        // lets go of the mouse.
        bool focus_gained = false;
        bool focus_lost   = false;
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

        // Hides the cursor and reports raw motion without it hitting the
        // screen edge. For mouse look.
        void set_relative_mouse(bool relative);

        // Current state of a key, by physical position, so WASD stays WASD on
        // any keyboard layout.
        bool key_down(SDL_Scancode key) const;

    private:
        SDL_Window* window_        = nullptr;
        bool        sdl_started_   = false;
        bool        minimized_     = false;

        function<void(SDL_Event const&)> hook_;
    };
}
