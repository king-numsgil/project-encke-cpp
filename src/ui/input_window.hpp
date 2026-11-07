#pragma once

#include "input/bindings.hpp"
#include "input/capture.hpp"
#include "input/input_system.hpp"

#include <filesystem>

namespace encke
{
    class Joysticks;

    // F7: the bindings editor and the device monitor, as two tabs.
    //
    // Bindings: one context at a time, every action with its live value and
    // its bindings in that context. A binding lights while its control is
    // held, so it can be checked by pressing it even while its context is
    // off, as gameplay contexts are whenever the mouse is free for the UI.
    // `+` binds by pressing (input::Capture) or by typing a control's text;
    // clicking a binding edits its trigger, consumption and modifiers.
    // Edits go to the user's layer (input::Bindings), which the engine
    // compiles and runs from the next frame. Save writes it to input.json in
    // the user's folder; Save as defaults writes everything to the repo's
    // default.json.
    //
    // Devices: every joystick live, axes as bars with their raw value and the
    // range seen, buttons lit while held with their press count on hover,
    // hats by direction. Starts and stops the input log, which goes to a new
    // timestamped file in `log_directory`.
    class InputWindow
    {
    public:
        // `open` is cleared when the user closes it.
        void draw(bool& open, Joysticks& joysticks, std::filesystem::path const& log_directory,
                  input::InputSystem const& input, input::Bindings& bindings);

        // Waiting for a press to bind: the engine must not act on the keys
        // meanwhile, or binding Escape would quit.
        bool capturing() const { return capture_.active(); }

        // The context to keep on while the window is open, when Preview is
        // ticked: the caller enables it with its own.
        optional<string> preview_context() const
        {
            return preview_ && !context_.empty() ? optional<string>{context_} : nullopt;
        }

    private:
        void draw_bindings(input::InputSystem const& input, input::Bindings& bindings);
        void draw_capture(input::Bindings& bindings);
        void draw_new_action(input::Bindings& bindings);
        void draw_new_context(input::Bindings& bindings);

        // The popup editing one binding of an action with `components`
        // axes. True when it changed `binding`.
        bool edit_binding(input::BindingFile& binding, size_t components);

        // Adds `control` to the capture's action, or replaces the binding
        // it was rebinding.
        void bind(input::Bindings& bindings, string const& control);

        string context_;
        char   filter_[64] = {};
        bool   bound_only_ = true;
        bool   preview_    = false;

        input::Capture capture_;
        string         capture_action_;
        // Which binding a capture replaces; none appends.
        optional<size_t> capture_index_;
        char             typed_[128] = {};
        string           typed_error_;

        char   new_name_[64]         = {};
        int    new_type_             = 0;
        char   new_description_[128] = {};
        char   new_context_[64]      = {};
        int    new_priority_         = 0;
        string status_;
    };
}
