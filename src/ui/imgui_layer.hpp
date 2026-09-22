#pragma once

#include "render/renderer.hpp"
#include "ui/imgui_vulkan.hpp"

namespace encke
{
    class BindlessSet;
    class VulkanAllocator;
    class VulkanDevice;
    class VulkanSwapchain;
    class Window;

    // Dear ImGui and ImPlot: contexts, the SDL3 platform backend and our own
    // Vulkan renderer backend (ui/imgui_vulkan). Owns no windows of its own;
    // whoever draws UI does so between begin_frame() and end_frame(), and the
    // renderer records the result through the Overlay interface.
    class ImGuiLayer final : public Renderer::Overlay
    {
    public:
        ImGuiLayer() = default;
        ~ImGuiLayer() override;

        ImGuiLayer(ImGuiLayer const&)            = delete;
        ImGuiLayer& operator=(ImGuiLayer const&) = delete;
        ImGuiLayer(ImGuiLayer&&)                 = delete;
        ImGuiLayer& operator=(ImGuiLayer&&)      = delete;

        // `frames_in_flight` must match the renderer's: the backend keys its
        // per-frame buffers and deferred frees to the renderer's slot index.
        bool init(Window const& window, VulkanAllocator const& allocator,
                  VulkanDevice const& device, BindlessSet& bindless,
                  VulkanSwapchain const& swapchain, u32 frames_in_flight);
        void shutdown();

        void process_event(SDL_Event const& event);

        void begin_frame();
        void end_frame();

        void prepare(VkCommandBuffer command, u32 slot) override;
        void record(VkCommandBuffer command) override;

        // True while a UI text field is taking typed characters, so the app
        // should not also treat them as shortcuts.
        //
        // Not WantCaptureKeyboard: with keyboard navigation on, that is true
        // whenever any ImGui window has focus -- which the first window gets
        // on appearing -- and it swallowed every shortcut while the UI showed.
        bool wants_text() const;

        // True while the pointer is over, or dragging, a UI window.
        bool wants_mouse() const;

        // Stops the UI reacting to mouse and keyboard, for while the camera
        // has them: otherwise the hidden cursor still hovers windows and the
        // movement keys drive keyboard navigation.
        void set_input_blocked(bool blocked);

    private:
        ImGuiVulkan renderer_;

        // Kept alive for ImGui, which holds the pointer rather than a copy.
        string ini_path_;

        bool context_  = false;
        bool platform_ = false;
    };
}
