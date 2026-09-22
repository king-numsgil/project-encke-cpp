#include "core/pch.hpp"

#include "ui/imgui_layer.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "platform/window.hpp"
#include "vulkan/swapchain.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <implot.h>

namespace encke
{
    ImGuiLayer::~ImGuiLayer()
    {
        shutdown();
    }

    bool ImGuiLayer::init(Window const& window, VulkanAllocator const& allocator,
                          VulkanDevice const& device, BindlessSet& bindless,
                          VulkanSwapchain const& swapchain, u32 frames_in_flight)
    {
        memory::install_imgui_allocator();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        context_ = true;

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // Beside the executable, which lives in the gitignored build tree,
        // rather than wherever the working directory happens to be.
        char const* const base = SDL_GetBasePath();
        ini_path_              = string{base != nullptr ? base : ""} + "imgui.ini";
        io.IniFilename         = ini_path_.c_str();

        ImGui::StyleColorsDark();

        if (!ImGui_ImplSDL3_InitForVulkan(window.handle()))
        {
            log::error("ImGui_ImplSDL3_InitForVulkan failed");
            return false;
        }
        platform_ = true;

        VkFormat const ui_format = swapchain.ui_format();
        if (!renderer_.init(allocator, device, bindless, ui_format, frames_in_flight))
        {
            return false;
        }

        log::info("imgui %s, implot, drawing through the %s view", ImGui::GetVersion(),
                  ui_format == swapchain.format() ? "sRGB" : "UNORM");
        return true;
    }

    void ImGuiLayer::process_event(SDL_Event const& event)
    {
        if (platform_)
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
        }
    }

    void ImGuiLayer::begin_frame()
    {
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiLayer::end_frame()
    {
        ImGui::Render();
    }

    void ImGuiLayer::prepare(VkCommandBuffer command, u32 slot)
    {
        renderer_.prepare(ImGui::GetDrawData(), command, slot);
    }

    void ImGuiLayer::record(VkCommandBuffer command)
    {
        renderer_.record(ImGui::GetDrawData(), command);
    }

    bool ImGuiLayer::wants_text() const
    {
        return context_ && ImGui::GetIO().WantTextInput;
    }

    bool ImGuiLayer::wants_mouse() const
    {
        return context_ && ImGui::GetIO().WantCaptureMouse;
    }

    void ImGuiLayer::set_input_blocked(bool blocked)
    {
        if (!context_)
        {
            return;
        }

        constexpr ImGuiConfigFlags kBlock = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;

        ImGuiIO& io = ImGui::GetIO();
        if (blocked)
        {
            io.ConfigFlags |= kBlock;
        }
        else
        {
            io.ConfigFlags &= ~kBlock;
        }
    }

    void ImGuiLayer::shutdown()
    {
        // Backends first, contexts last: both backends read the context
        // while tearing down.
        renderer_.shutdown();

        if (platform_)
        {
            ImGui_ImplSDL3_Shutdown();
            platform_ = false;
        }

        if (context_)
        {
            ImPlot::DestroyContext();
            ImGui::DestroyContext();
            context_ = false;
        }
    }
}
