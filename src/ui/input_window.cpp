#include "core/pch.hpp"

#include "ui/input_window.hpp"

#include "platform/joysticks.hpp"

#include <cstdio>

#include <imgui.h>

namespace encke
{
    namespace
    {
        constexpr int   kButtonsPerRow = 11;
        constexpr f32   kButtonSize    = 30.0f;
        constexpr ImU32 kHeld          = IM_COL32(60, 180, 75, 255);
        constexpr ImU32 kIdle          = IM_COL32(55, 55, 60, 255);

        // input-YYYYMMDD-HHMMSS.jsonl, local time, so sessions never
        // overwrite one another.
        std::filesystem::path log_file_name()
        {
            SDL_Time     now = 0;
            SDL_DateTime date{};
            if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &date, true))
            {
                return "input.jsonl";
            }

            char name[64] = {};
            std::snprintf(name, sizeof(name), "input-%04d%02d%02d-%02d%02d%02d.jsonl", date.year,
                          date.month, date.day, date.hour, date.minute, date.second);
            return name;
        }

        f32 axis_fraction(i16 value)
        {
            return (static_cast<f32>(value) + 32768.0f) / 65535.0f;
        }

        void draw_device(Joystick const& device)
        {
            char header[160] = {};
            std::snprintf(header, sizeof(header), "%s  (%04x:%04x, id %u)###joystick%u",
                          device.name.c_str(), device.vendor, device.product,
                          static_cast<unsigned>(device.id), static_cast<unsigned>(device.id));
            if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            ImGui::PushID(static_cast<int>(device.id));

            for (size_t axis = 0; axis < device.axes.size(); ++axis)
            {
                char overlay[64] = {};
                std::snprintf(overlay, sizeof(overlay), "%6d", static_cast<int>(device.axes[axis]));
                ImGui::Text("axis %-2zu", axis);
                ImGui::SameLine();
                ImGui::ProgressBar(axis_fraction(device.axes[axis]), ImVec2{220.0f, 0.0f}, overlay);
                ImGui::SameLine();
                ImGui::TextDisabled("%6d .. %6d", static_cast<int>(device.axis_min[axis]),
                                    static_cast<int>(device.axis_max[axis]));
            }

            for (size_t hat = 0; hat < device.hats.size(); ++hat)
            {
                ImGui::Text("hat %-2zu  %s", hat, hat_name(device.hats[hat]));
            }

            // Buttons as squares, lit while held. The press count is on
            // hover: it is what shows an encoder's pulses, which are too
            // short to see lit.
            for (size_t button = 0; button < device.buttons.size(); ++button)
            {
                if (button % kButtonsPerRow != 0)
                {
                    ImGui::SameLine();
                }

                char label[16] = {};
                std::snprintf(label, sizeof(label), "%zu", button);
                ImU32 const colour = device.buttons[button] != 0 ? kHeld : kIdle;
                ImGui::PushStyleColor(ImGuiCol_Button, colour);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colour);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, colour);
                ImGui::Button(label, ImVec2{kButtonSize, kButtonSize});
                ImGui::PopStyleColor(3);

                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("button %zu: %u presses", button, device.presses[button]);
                }
            }

            ImGui::PopID();
        }
    }

    void input_window(Joysticks& joysticks, bool& open, std::filesystem::path const& log_directory)
    {
        if (!open)
        {
            return;
        }

        ImGui::SetNextWindowPos(ImVec2{460.0f, 300.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Input devices (F7)", &open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (joysticks.logging())
            {
                if (ImGui::Button("Stop log"))
                {
                    joysticks.stop_log();
                }
                else
                {
                    ImGui::SameLine();
                    ImGui::Text("%llu lines", static_cast<unsigned long long>(joysticks.logged_lines()));
                    ImGui::TextDisabled("%s", joysticks.log_path().string().c_str());
                }
            }
            else if (ImGui::Button("Start log"))
            {
                joysticks.start_log(log_directory / log_file_name());
            }

            if (joysticks.devices().empty())
            {
                ImGui::TextUnformatted("No joysticks connected.");
            }
            for (Joystick const& device : joysticks.devices())
            {
                draw_device(device);
            }
        }
        ImGui::End();
    }
}
