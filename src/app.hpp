#pragma once

#include "vulkan_context.hpp"
#include "window.hpp"

namespace encke
{
    class App
    {
    public:
        App() = default;
        ~App() = default;

        App(App const&)            = delete;
        App& operator=(App const&) = delete;
        App(App&&)                 = delete;
        App& operator=(App&&)      = delete;

        bool init();
        void run();

    private:
        void on_resize();

        Window        window_;
        VulkanContext vulkan_;
    };
}
