#pragma once

#include "input/input_map.hpp"

#include <map>

// The action layer: control events in, action states out, once a frame.
// Game code reads actions, never devices.
//
// Events are taken in the order the pump saw them, so a button that
// pressed and released between two frames still fires, and each binding's
// edges carry their event's stamp for the timing triggers to come. Mouse
// motion and the wheel are deltas, summed over the frame before their
// bindings see them.
//
// Contexts run highest priority first. A binding with `consume` hides its
// control from every lower context while its own is enabled; within one
// context, every binding on a control sees it. The set of live bindings
// changes only when a context is enabled or disabled, and a binding that
// comes to life starts from its control's current value without an edge,
// so a key already held does not press.
namespace encke::input
{
    struct ActionState
    {
        // Summed over the bindings that pass a value, then the action's own
        // modifiers. Components past the action's type are zero; a bool
        // action is 1 in x while active.
        f32vec3 value{0.0f};

        // A bool action: held, or fired this frame. An axis: nonzero.
        bool active = false;

        // How many times a trigger fired this frame, and the stamp of the
        // last: presses for `down` and `pressed`, releases for `released`.
        u32 fired    = 0;
        u64 fired_ns = 0;
    };

    class InputSystem
    {
    public:
        // Replaces the map. Every context starts disabled and every action
        // idle.
        void set_map(InputMap map);

        InputMap const& map() const { return map_; }

        // The ids the engine reads, looked up once. Logs and returns nothing
        // if no layer declares it, or declares it with another type.
        optional<ActionId>  require(string_view name, ActionType type) const;
        optional<ContextId> require_context(string_view name) const;

        void set_enabled(ContextId context, bool enabled);
        bool enabled(ContextId context) const { return enabled_[context.index]; }

        // Queued until update(), in order.
        void push(ControlEvent const& event);

        // Every held button, key and hat direction lets go at `ns`: the
        // window lost focus and its releases will not arrive. Axes keep
        // their value.
        void release_all(u64 ns);

        // Every control of a device that has gone goes to zero.
        void release_device(DeviceId const& device, u64 ns);

        // This frame's events into this frame's action states.
        void update();

        // Overrides an action for this frame, after update(): what a
        // scenario does in place of the devices. Fires when it becomes
        // active.
        void inject(ActionId action, f32vec3 value);

        ActionState const& state(ActionId action) const { return states_[action.index]; }

        bool    active(ActionId action) const { return state(action).active; }
        u32     fired(ActionId action) const { return state(action).fired; }
        f32     axis1(ActionId action) const { return state(action).value.x; }
        f32vec2 axis2(ActionId action) const { return f32vec2{state(action).value}; }
        f32vec3 axis3(ActionId action) const { return state(action).value; }

        // The value a control last reported; 0 if it never has. Relative
        // controls report this frame's sum.
        f32 control_value(Control const& control) const;

        // What update() took this frame, for the editor's press-to-bind.
        span<ControlEvent const> frame_events() const { return frame_events_; }

    private:
        struct BindingRef
        {
            u32 context = 0;
            u32 binding = 0;
        };

        struct BindingState
        {
            bool                  live     = false;
            bool                  actuated = false;
            f32vec3               value{0.0f};
            vector<ModifierState> modifiers;
            // This frame's edges, and the value at the last that fired.
            u32     presses  = 0;
            u32     releases = 0;
            f32vec3 fire_value{0.0f};
        };

        Binding const& binding(BindingRef ref) const { return map_.contexts[ref.context].bindings[ref.binding]; }
        BindingState&  binding_state(BindingRef ref) { return binding_states_[ref.context][ref.binding]; }

        // Which bindings see which controls, after the enabled contexts'
        // consumption.
        void rebuild();

        // One new value through a binding; counts its edges.
        void step(BindingRef ref, f32vec3 raw, u64 ns);

        void fire(ActionId action, u64 ns);

        InputMap map_;
        vector<bool> enabled_;
        bool         dirty_ = true;

        vector<vector<BindingState>>       binding_states_;
        vector<BindingRef>                 live_;
        std::map<Control, vector<BindingRef>> dispatch_;

        vector<ActionState>           states_;
        vector<bool>                  was_active_;
        vector<vector<ModifierState>> action_modifiers_;

        // Absolute controls' last values; relative ones' sums this frame.
        std::map<Control, f32> values_;
        std::map<Control, f32> deltas_;

        vector<ControlEvent> queue_;
        vector<ControlEvent> frame_events_;
    };
}
