#include "core/pch.hpp"

#include "ui/image_window.hpp"

#include "ui/imgui_vulkan.hpp"

#include <imgui.h>

namespace encke
{
    void image_window(char const* title, bool& open, u32 bindless_handle, VkExtent2D extent,
                      f32vec2 first_position, function<void()> const& controls)
    {
        if (!open)
        {
            return;
        }

        ImGui::SetNextWindowPos(ImVec2{first_position.x, first_position.y}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2{400.0f, 260.0f}, ImGuiCond_FirstUseEver);

        if (!ImGui::Begin(title, &open))
        {
            ImGui::End();
            return;
        }

        if (controls)
        {
            controls();
        }

        // Fit inside what is left of the window, whichever axis runs out
        // first.
        ImVec2 const available = ImGui::GetContentRegionAvail();
        f32 const    aspect = static_cast<f32>(extent.width) / static_cast<f32>(extent.height);

        ImVec2 size{available.x, available.x / aspect};
        if (size.y > available.y)
        {
            size = ImVec2{available.y * aspect, available.y};
        }

        if (size.x >= 1.0f && size.y >= 1.0f)
        {
            ImGui::Image(ImGuiVulkan::texture_id(bindless_handle), size);
        }

        ImGui::End();
    }
}
