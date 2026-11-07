#include "core/pch.hpp"

#include "ui/input_window.hpp"

#include "platform/joysticks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <imgui.h>

namespace encke
{
    namespace
    {
        constexpr int   kButtonsPerRow = 11;
        constexpr f32   kButtonSize    = 30.0f;
        constexpr ImU32 kHeld          = IM_COL32(60, 180, 75, 255);
        constexpr ImU32 kIdle          = IM_COL32(55, 55, 60, 255);
        constexpr ImVec4 kEdited{1.0f, 0.8f, 0.3f, 1.0f};
        constexpr ImVec4 kProblem{1.0f, 0.4f, 0.4f, 1.0f};

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

        void draw_devices(Joysticks& joysticks, std::filesystem::path const& log_directory)
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

        // A string_view as ImGui text, which wants a terminated string or a
        // range.
        void text(string_view s)
        {
            ImGui::TextUnformatted(s.data(), s.data() + s.size());
        }

        bool edit_float(char const* label, f32& value, f32 speed, f32 low, f32 high)
        {
            ImGui::SetNextItemWidth(120.0f);
            return ImGui::DragFloat(label, &value, speed, low, high, "%.3f");
        }

        // One modifier's settings. True when one changed.
        bool edit_modifier(input::Modifier& modifier)
        {
            return std::visit(
                [](auto& m) -> bool {
                    using M = std::decay_t<decltype(m)>;

                    if constexpr (std::is_same_v<M, input::DeadZone>)
                    {
                        bool changed = edit_float("lower", m.lower, 0.005f, 0.0f, 1.0f);
                        ImGui::SameLine();
                        changed |= edit_float("upper", m.upper, 0.005f, 0.0f, 1.0f);
                        ImGui::SameLine();
                        changed |= ImGui::Checkbox("radial", &m.radial);
                        return changed;
                    }
                    else if constexpr (std::is_same_v<M, input::Curve>)
                    {
                        return edit_float("exponent", m.exponent, 0.01f, 0.1f, 8.0f);
                    }
                    else if constexpr (std::is_same_v<M, input::Negate>)
                    {
                        return false;
                    }
                    else if constexpr (std::is_same_v<M, input::Scale>)
                    {
                        ImGui::SetNextItemWidth(240.0f);
                        return ImGui::DragFloat3("factor", m.factor.data(), 0.01f);
                    }
                    else if constexpr (std::is_same_v<M, input::Swizzle>)
                    {
                        char order[8] = {};
                        std::snprintf(order, sizeof(order), "%s", m.order.c_str());
                        ImGui::SetNextItemWidth(60.0f);
                        if (ImGui::InputText("order", order, sizeof(order)))
                        {
                            m.order = order;
                            return true;
                        }
                        return false;
                    }
                    else if constexpr (std::is_same_v<M, input::Saturation>)
                    {
                        bool changed = edit_float("input", m.input, 0.005f, 0.01f, 1.0f);
                        ImGui::SameLine();
                        changed |= edit_float("output", m.output, 0.005f, 0.0f, 1.0f);
                        return changed;
                    }
                    else if constexpr (std::is_same_v<M, input::Split>)
                    {
                        int half = static_cast<int>(m.half);
                        bool changed = ImGui::RadioButton("positive", &half, 0);
                        ImGui::SameLine();
                        changed |= ImGui::RadioButton("negative", &half, 1);
                        m.half = static_cast<input::Half>(half);
                        return changed;
                    }
                    else
                    {
                        static_assert(std::is_same_v<M, input::Threshold>);
                        bool changed = edit_float("press", m.press, 0.005f, 0.0f, 1.0f);
                        ImGui::SameLine();
                        changed |= edit_float("release", m.release, 0.005f, 0.0f, 1.0f);
                        return changed;
                    }
                },
                modifier);
        }

        // 1 for bool and 1D, else the axes it has.
        size_t components_of(input::ActionType type)
        {
            switch (type)
            {
            case input::ActionType::Bool:
            case input::ActionType::Axis1D: return 1;
            case input::ActionType::Axis2D: return 2;
            case input::ActionType::Axis3D: return 3;
            }
            return 1;
        }

        constexpr array<char const*, 3> kComponentNames{{"x", "y", "z"}};

        // The modifiers the user edits as such: on an action with more than
        // one component, all but the swizzle last in the chain, which the
        // component picker stands for.
        span<input::Modifier const> shown_modifiers(input::BindingFile const& binding, size_t components)
        {
            span<input::Modifier const> modifiers = input::modifiers_of(binding);
            if (components > 1 && !modifiers.empty() && std::holds_alternative<input::Swizzle>(modifiers.back()))
            {
                modifiers = modifiers.first(modifiers.size() - 1);
            }
            return modifiers;
        }

        // "key:W", then what is not a default: the trigger, modifiers by
        // name, no consume, and on a 2D or 3D action the component.
        string describe(input::BindingFile const& binding, size_t components)
        {
            string label = binding.control;
            if (binding.trigger.has_value())
            {
                label += " [" + string{input::trigger_name(binding.trigger->index())} + "]";
            }
            for (input::Modifier const& modifier : shown_modifiers(binding, components))
            {
                label += " " + string{input::modifier_name(modifier.index())};
            }
            if (!input::consumes(binding))
            {
                label += " (shared)";
            }
            if (components > 1)
            {
                label += " -> " + string{kComponentNames[std::min<size_t>(input::component_of(binding), 2)]};
            }
            return label;
        }

        bool matches(string_view name, char const* filter)
        {
            return filter[0] == '\0' || name.find(filter) != string_view::npos;
        }

        // Before the second item on a line and after: stays on the line if
        // a small button labelled `label` still fits, else wraps.
        void next_fitting(char const* label)
        {
            ImGuiStyle const& style = ImGui::GetStyle();
            f32 const width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < width)
            {
                ImGui::NewLine();
            }
        }
    }

    void InputWindow::draw(bool& open, Joysticks& joysticks, std::filesystem::path const& log_directory,
                           input::InputSystem const& input, input::Bindings& bindings)
    {
        if (!open)
        {
            capture_.cancel();
            return;
        }

        ImGui::SetNextWindowPos(ImVec2{460.0f, 60.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2{820.0f, 620.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Input (F7)", &open))
        {
            if (ImGui::BeginTabBar("input"))
            {
                if (ImGui::BeginTabItem("Bindings"))
                {
                    draw_bindings(input, bindings);
                    ImGui::EndTabItem();
                }
                else
                {
                    capture_.cancel();
                }
                if (ImGui::BeginTabItem("Devices"))
                {
                    draw_devices(joysticks, log_directory);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();

        if (!open)
        {
            capture_.cancel();
        }
    }

    void InputWindow::bind(input::Bindings& bindings, string const& control)
    {
        if (!input::parse_control(control).has_value())
        {
            typed_error_ = "\"" + control + "\" is not a control";
            return;
        }

        span<input::BindingFile const> const current = bindings.bindings(context_, capture_action_);
        vector<input::BindingFile>           list(current.begin(), current.end());
        if (capture_index_.has_value() && *capture_index_ < list.size())
        {
            list[*capture_index_].control = control;
        }
        else
        {
            input::BindingFile added{.control = control, .modifiers = {}, .trigger = {}, .consume = {}};

            // An axis on a 2D or 3D action drives the first component no
            // other axis does, so three axes bound in a row are x, y and z.
            // A button stays on x; two buttons usually share a component as
            // its two directions.
            auto const   action     = bindings.merged().actions.find(capture_action_);
            size_t const components = action != bindings.merged().actions.end() ? components_of(action->second.type) : 1;
            optional<input::Control> const parsed = input::parse_control(control);
            bool const axis = parsed.has_value() &&
                              (parsed->kind == input::ControlKind::JoyAxis || parsed->kind == input::ControlKind::MouseAxis);
            if (components > 1 && axis)
            {
                array<bool, 3> taken{};
                for (input::BindingFile const& binding : list)
                {
                    optional<input::Control> const other = input::parse_control(binding.control);
                    if (other.has_value() &&
                        (other->kind == input::ControlKind::JoyAxis || other->kind == input::ControlKind::MouseAxis))
                    {
                        taken[std::min<size_t>(input::component_of(binding), 2)] = true;
                    }
                }
                for (size_t component = 0; component < components; ++component)
                {
                    if (!taken[component])
                    {
                        input::set_component(added, component);
                        break;
                    }
                }
            }
            list.push_back(std::move(added));
        }
        bindings.set_bindings(context_, capture_action_, std::move(list));
        status_ = capture_action_ + ": " + control;

        capture_.cancel();
        capture_action_.clear();
        capture_index_.reset();
        typed_[0] = '\0';
        typed_error_.clear();
    }

    void InputWindow::draw_capture(input::Bindings& bindings)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(40, 70, 45, 255));
        if (ImGui::BeginChild("capture", ImVec2{0.0f, 0.0f},
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            ImGui::Text("Binding %s in %s: press a key or a joystick button, or move an axis. Escape cancels.",
                        capture_action_.c_str(), context_.c_str());
            ImGui::SetNextItemWidth(260.0f);
            bool const entered = ImGui::InputTextWithHint("##typed", "or type one: mouse:x, key:Left Shift",
                                                          typed_, sizeof(typed_),
                                                          ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Add") || entered)
            {
                bind(bindings, typed_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                capture_.cancel();
            }
            if (!typed_error_.empty())
            {
                ImGui::TextColored(kProblem, "%s", typed_error_.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void InputWindow::draw_new_action(input::Bindings& bindings)
    {
        if (!ImGui::BeginPopup("new action"))
        {
            return;
        }

        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("name", new_name_, sizeof(new_name_));
        ImGui::SetNextItemWidth(200.0f);
        ImGui::Combo("type", &new_type_, "bool\0" "1d\0" "2d\0" "3d\0");
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputText("description", new_description_, sizeof(new_description_));
        if (ImGui::Button("Create"))
        {
            input::ActionFile action{.type        = static_cast<input::ActionType>(new_type_),
                                     .description = nullopt,
                                     .modifiers   = nullopt};
            if (new_description_[0] != '\0')
            {
                action.description = string{new_description_};
            }
            if (bindings.add_action(new_name_, action))
            {
                status_ = "new action " + string{new_name_};
                new_name_[0]        = '\0';
                new_description_[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            else
            {
                status_ = "an action needs a name no other has";
            }
        }
        ImGui::EndPopup();
    }

    void InputWindow::draw_new_context(input::Bindings& bindings)
    {
        if (!ImGui::BeginPopup("new context"))
        {
            return;
        }

        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("name", new_context_, sizeof(new_context_));
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputInt("priority", &new_priority_);
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputText("description", new_description_, sizeof(new_description_));
        ImGui::TextDisabled("The engine enables only the contexts it knows; a new one\n"
                            "can be bound now and is live once the game enables it.");
        if (ImGui::Button("Create"))
        {
            if (bindings.add_context(new_context_, new_priority_, new_description_))
            {
                context_            = new_context_;
                status_             = "new context " + context_;
                new_context_[0]     = '\0';
                new_description_[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            else
            {
                status_ = "a context needs a name no other has";
            }
        }
        ImGui::EndPopup();
    }

    bool InputWindow::edit_binding(input::BindingFile& binding, size_t components)
    {
        bool changed = false;

        text(binding.control);
        ImGui::Separator();

        // Which part of the action it drives; the swizzle behind it is not
        // listed with the modifiers.
        size_t component = input::component_of(binding);
        if (components > 1)
        {
            ImGui::TextUnformatted("Drives");
            for (size_t index = 0; index < components; ++index)
            {
                ImGui::SameLine();
                if (ImGui::RadioButton(kComponentNames[index], component == index) && component != index)
                {
                    component = index;
                    changed   = true;
                }
            }
        }

        ImGui::TextUnformatted("Trigger");
        size_t const current = input::trigger_of(binding).index();
        for (size_t index = 0; index < input::kTriggerCount; ++index)
        {
            ImGui::SameLine();
            string const name{input::trigger_name(index)};
            if (ImGui::RadioButton(name.c_str(), current == index) && current != index)
            {
                binding.trigger = input::make_trigger(index);
                changed         = true;
            }
        }

        bool consume = input::consumes(binding);
        if (ImGui::Checkbox("Hide this control from lower contexts", &consume))
        {
            binding.consume = consume;
            changed         = true;
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Modifiers, in order");
        span<input::Modifier const> const shown = shown_modifiers(binding, components);
        vector<input::Modifier>           modifiers(shown.begin(), shown.end());
        optional<size_t>                  remove;
        for (size_t index = 0; index < modifiers.size(); ++index)
        {
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::SmallButton("x"))
            {
                remove = index;
            }
            ImGui::SameLine();
            text(input::modifier_name(modifiers[index].index()));
            ImGui::SameLine();
            changed |= edit_modifier(modifiers[index]);
            if (optional<string> const problem = input::check(modifiers[index]))
            {
                ImGui::TextColored(kProblem, "%s", problem->c_str());
            }
            ImGui::PopID();
        }
        if (remove.has_value())
        {
            modifiers.erase(modifiers.begin() + static_cast<std::ptrdiff_t>(*remove));
            changed = true;
        }

        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##add modifier", "add a modifier"))
        {
            for (size_t index = 0; index < input::kModifierCount; ++index)
            {
                // The component picker above is the swizzle there.
                bool const swizzle = std::holds_alternative<input::Swizzle>(input::make_modifier(index));
                if (components > 1 && swizzle)
                {
                    continue;
                }
                string const name{input::modifier_name(index)};
                if (ImGui::Selectable(name.c_str()))
                {
                    modifiers.push_back(input::make_modifier(index));
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        if (changed)
        {
            binding.modifiers = std::move(modifiers);
            if (components > 1)
            {
                // Puts the swizzle back last, or none for x.
                input::set_component(binding, component);
            }
        }
        return changed;
    }

    void InputWindow::draw_bindings(input::InputSystem const& input, input::Bindings& bindings)
    {
        input::Layer const& merged = bindings.merged();
        if (!merged.contexts.contains(context_))
        {
            context_ = merged.contexts.contains("on_foot") ? "on_foot"
                       : merged.contexts.empty()          ? string{}
                                                          : merged.contexts.begin()->first;
        }

        // Taken before anything draws, so the frame the press lands is the
        // frame it binds.
        if (optional<input::Control> const control = capture_.feed(input.frame_events()))
        {
            bind(bindings, input::format_control(*control));
        }

        // The context, and whether the engine has it on right now.
        optional<input::ContextId> const live_context = input.map().find_context(context_);
        bool const                       live         = live_context.has_value() && input.enabled(*live_context);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("Context", context_.c_str()))
        {
            for (auto const& [name, context] : merged.contexts)
            {
                char label[128] = {};
                std::snprintf(label, sizeof(label), "%s  (priority %d)", name.c_str(), context.priority.value_or(0));
                if (ImGui::Selectable(label, name == context_))
                {
                    context_ = name;
                    capture_.cancel();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (live)
        {
            ImGui::TextColored(ImVec4{0.4f, 0.9f, 0.5f, 1.0f}, preview_ ? "on (preview)" : "on");
        }
        else
        {
            ImGui::TextDisabled("off now");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Preview", &preview_);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Keeps this context on while the window is open, so its actions show live\n"
                              "values. On foot or the jetpack, the character moves as you press.");
        }
        if (bindings.removable_context(context_))
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("delete context"))
            {
                ImGui::OpenPopup("delete context");
            }
        }
        if (ImGui::BeginPopup("delete context"))
        {
            ImGui::Text("Delete %s and every binding in it?", context_.c_str());
            if (ImGui::Button("Delete"))
            {
                status_ = "deleted context " + context_;
                bindings.remove_context(context_);
                capture_.cancel();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (auto const found = merged.contexts.find(context_);
            found != merged.contexts.end() && found->second.description.has_value())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", found->second.description->c_str());
        }

        // Saving.
        ImGui::BeginDisabled(!bindings.dirty());
        if (ImGui::Button("Save"))
        {
            status_ = bindings.save_user() ? "saved " + bindings.user_path().string() : "save failed; see the log";
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Your edits, to %s", bindings.user_path().string().c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Save as defaults"))
        {
            ImGui::OpenPopup("save defaults");
        }
        if (ImGui::BeginPopup("save defaults"))
        {
            ImGui::Text("Write every binding to %s\nand empty your edits? Comments in the file are lost.",
                        bindings.defaults_path().string().c_str());
            if (ImGui::Button("Write"))
            {
                status_ = bindings.save_defaults() ? "saved " + bindings.defaults_path().string()
                                                   : "save failed; see the log";
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("New action"))
        {
            ImGui::OpenPopup("new action");
        }
        draw_new_action(bindings);
        ImGui::SameLine();
        if (ImGui::Button("New context"))
        {
            ImGui::OpenPopup("new context");
        }
        draw_new_context(bindings);
        ImGui::SameLine();
        if (bindings.dirty())
        {
            ImGui::TextColored(kEdited, "unsaved");
            ImGui::SameLine();
        }
        ImGui::TextDisabled("%s", status_.c_str());

        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint("##filter", "filter actions", filter_, sizeof(filter_));
        ImGui::SameLine();
        ImGui::Checkbox("Bound here only", &bound_only_);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Hides the engine's actions this context does not bind. Your own actions\n"
                              "always show; off lists everything.");
        }

        if (capture_.active())
        {
            draw_capture(bindings);
        }

        if (context_.empty())
        {
            ImGui::TextUnformatted("No contexts.");
            return;
        }

        ImGuiTableFlags const flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("actions", 3, flags, ImVec2{0.0f, ImGui::GetContentRegionAvail().y}))
        {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Bindings in this context", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (auto const& [name, action] : merged.actions)
        {
            // Hidden only when untouched: one of the defaults' actions,
            // with no binding here and no edit, not being bound. The
            // user's own actions always show, so a new one can be bound.
            bool const removable = bindings.removable_action(name);
            bool const listed    = !bound_only_ || removable || !bindings.bindings(context_, name).empty() ||
                                bindings.edited(context_, name) || (capture_.active() && capture_action_ == name);
            if (!listed || !matches(name, filter_))
            {
                continue;
            }
            ImGui::PushID(name.c_str());
            ImGui::TableNextRow();
            size_t const components = components_of(action.type);

            // The name, marked when the user's layer changes it here.
            ImGui::TableNextColumn();
            bool const edited = bindings.edited(context_, name);
            if (edited)
            {
                ImGui::TextColored(kEdited, "%s *", name.c_str());
            }
            else
            {
                ImGui::TextUnformatted(name.c_str());
            }
            if (ImGui::IsItemHovered())
            {
                string const type{input::type_name(action.type)};
                ImGui::SetTooltip("%s\n%s%s%s", type.c_str(), action.description.value_or("").c_str(),
                                  edited ? "\n* your edit" : "",
                                  removable ? "\nright-click to delete" : "");
            }
            bool removed = false;
            if (removable && ImGui::BeginPopupContextItem("action menu"))
            {
                if (ImGui::Selectable("Delete action, with its bindings everywhere"))
                {
                    status_ = "deleted action " + name;
                    removed = true;
                }
                ImGui::EndPopup();
            }
            if (removed)
            {
                // The map iterated is the one this changes: stop here.
                ImGui::PopID();
                bindings.remove_action(name);
                break;
            }

            // Its value as the engine has it now, from every context.
            ImGui::TableNextColumn();
            if (optional<input::ActionId> const id = input.map().find_action(name))
            {
                input::ActionState const& state = input.state(*id);
                switch (action.type)
                {
                case input::ActionType::Bool:
                    if (state.active)
                    {
                        ImGui::TextColored(ImVec4{0.4f, 0.9f, 0.5f, 1.0f}, "on");
                    }
                    else
                    {
                        ImGui::TextDisabled("off");
                    }
                    break;
                case input::ActionType::Axis1D:
                    ImGui::Text("%+.2f", static_cast<f64>(state.value.x));
                    break;
                case input::ActionType::Axis2D:
                    ImGui::Text("%+.2f %+.2f", static_cast<f64>(state.value.x), static_cast<f64>(state.value.y));
                    break;
                case input::ActionType::Axis3D:
                    ImGui::Text("%+.2f %+.2f %+.2f", static_cast<f64>(state.value.x),
                                static_cast<f64>(state.value.y), static_cast<f64>(state.value.z));
                    break;
                }
            }

            // Its bindings, lit while their control is held.
            ImGui::TableNextColumn();
            span<input::BindingFile const> const current = bindings.bindings(context_, name);
            vector<input::BindingFile>           list(current.begin(), current.end());
            bool                                 changed = false;
            for (size_t index = 0; index < list.size(); ++index)
            {
                ImGui::PushID(static_cast<int>(index));

                optional<input::Control> const control = input::parse_control(list[index].control);
                f32 const value = control.has_value() && !control->relative() ? input.control_value(*control) : 0.0f;
                bool const held = std::abs(value) >= input::kActuation;
                string const label = describe(list[index], components);
                if (index > 0)
                {
                    // The label and its "x", which go together.
                    next_fitting((label + "  x").c_str());
                }
                if (held)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, kHeld);
                }
                if (ImGui::SmallButton(label.c_str()))
                {
                    ImGui::OpenPopup("binding");
                }
                if (held)
                {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered() && control.has_value() && control->kind == input::ControlKind::JoyAxis)
                {
                    ImGui::SetTooltip("%+.3f", static_cast<f64>(value));
                }

                if (ImGui::BeginPopup("binding"))
                {
                    changed |= edit_binding(list[index], components);
                    ImGui::Separator();
                    if (ImGui::Button("Rebind"))
                    {
                        capture_.start();
                        capture_action_ = name;
                        capture_index_  = index;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }

            bool const capturing_here = capture_.active() && capture_action_ == name && !capture_index_.has_value();
            if (!list.empty())
            {
                next_fitting("+");
            }
            if (ImGui::SmallButton(capturing_here ? "..." : "+"))
            {
                capture_.start();
                capture_action_ = name;
                capture_index_.reset();
                typed_[0] = '\0';
                typed_error_.clear();
            }
            if (edited)
            {
                next_fitting("reset");
                if (ImGui::SmallButton("reset"))
                {
                    bindings.reset(context_, name);
                }
            }

            if (changed)
            {
                bindings.set_bindings(context_, name, std::move(list));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
