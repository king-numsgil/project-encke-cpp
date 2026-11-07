#include "core/pch.hpp"

#include "input/input_system.hpp"

#include "core/log.hpp"

#include <set>

namespace encke::input
{
    namespace
    {
        // Components past the type's are zeroed.
        f32vec3 mask(ActionType type, f32vec3 value)
        {
            switch (type)
            {
            case ActionType::Bool:
            case ActionType::Axis1D: return f32vec3{value.x, 0.0f, 0.0f};
            case ActionType::Axis2D: return f32vec3{value.x, value.y, 0.0f};
            case ActionType::Axis3D: return value;
            }
            return value;
        }

        // Buttons, keys and hat directions: what a lost focus lets go of.
        bool digital(ControlKind kind)
        {
            return kind == ControlKind::Key || kind == ControlKind::MouseButton || kind == ControlKind::JoyButton ||
                   kind == ControlKind::JoyHat;
        }
    }

    void InputSystem::set_map(InputMap map)
    {
        map_ = std::move(map);
        enabled_.assign(map_.contexts.size(), false);
        dirty_ = true;

        binding_states_.clear();
        for (Context const& context : map_.contexts)
        {
            vector<BindingState>& states = binding_states_.emplace_back(context.bindings.size());
            for (size_t index = 0; index < context.bindings.size(); ++index)
            {
                states[index].modifiers.resize(context.bindings[index].modifiers.size());
            }
        }

        states_.assign(map_.actions.size(), ActionState{});
        was_active_.assign(map_.actions.size(), false);
        action_modifiers_.clear();
        for (Action const& action : map_.actions)
        {
            action_modifiers_.emplace_back(action.modifiers.size());
        }
        live_.clear();
        dispatch_.clear();
    }


    void InputSystem::set_enabled(ContextId context, bool enabled)
    {
        if (enabled_[context.index] != enabled)
        {
            enabled_[context.index] = enabled;
            dirty_                  = true;
        }
    }

    void InputSystem::push(ControlEvent const& event)
    {
        queue_.push_back(event);
    }

    void InputSystem::release_all(u64 ns)
    {
        for (auto const& [control, value] : values_)
        {
            if (digital(control.kind) && value != 0.0f)
            {
                push(ControlEvent{.control = control, .value = 0.0f, .ns = ns});
            }
        }
    }

    void InputSystem::release_device(DeviceId const& device, u64 ns)
    {
        for (auto const& [control, value] : values_)
        {
            if (control.joystick() && control.device == device && value != 0.0f)
            {
                push(ControlEvent{.control = control, .value = 0.0f, .ns = ns});
            }
        }
    }

    f32 InputSystem::control_value(Control const& control) const
    {
        std::map<Control, f32> const& from  = control.relative() ? deltas_ : values_;
        auto const                    found = from.find(control);
        return found != from.end() ? found->second : 0.0f;
    }

    void InputSystem::rebuild()
    {
        dirty_ = false;
        dispatch_.clear();
        live_.clear();

        std::set<Control> consumed;
        for (u32 context = 0; context < map_.contexts.size(); ++context)
        {
            vector<Binding> const& bindings = map_.contexts[context].bindings;
            vector<Control>        claims;
            for (u32 index = 0; index < bindings.size(); ++index)
            {
                BindingRef const ref{.context = context, .binding = index};
                BindingState&    state = binding_state(ref);
                bool const       live  = enabled_[context] && !consumed.contains(bindings[index].control);

                if (live && !state.live)
                {
                    // From where its control is now, without an edge.
                    std::ranges::fill(state.modifiers, ModifierState{});
                    Control const& control = bindings[index].control;
                    f32 const      raw     = control.relative() ? 0.0f : control_value(control);
                    state.value    = apply(bindings[index].modifiers, f32vec3{raw, 0.0f, 0.0f}, state.modifiers);
                    state.actuated = magnitude(state.value) >= kActuation;
                }
                else if (!live)
                {
                    state.value    = f32vec3{0.0f};
                    state.actuated = false;
                }
                state.live = live;

                if (live)
                {
                    live_.push_back(ref);
                    dispatch_[bindings[index].control].push_back(ref);
                    if (bindings[index].consume)
                    {
                        claims.push_back(bindings[index].control);
                    }
                }
            }
            consumed.insert(claims.begin(), claims.end());
        }
    }

    void InputSystem::fire(ActionId action, u64 ns)
    {
        ActionState& state = states_[action.index];
        ++state.fired;
        state.fired_ns = ns;
    }

    void InputSystem::step(BindingRef ref, f32vec3 raw, u64 ns)
    {
        Binding const& b     = binding(ref);
        BindingState&  state = binding_state(ref);

        f32vec3 const value    = apply(b.modifiers, raw, state.modifiers);
        bool const    actuated = magnitude(value) >= kActuation;

        if (actuated && !state.actuated)
        {
            ++state.presses;
            if (!std::holds_alternative<Released>(b.trigger))
            {
                state.fire_value = value;
                fire(b.action, ns);
            }
        }
        else if (!actuated && state.actuated)
        {
            ++state.releases;
            if (std::holds_alternative<Released>(b.trigger))
            {
                state.fire_value = state.value;
                fire(b.action, ns);
            }
        }

        state.value    = value;
        state.actuated = actuated;
    }

    void InputSystem::update()
    {
        if (dirty_)
        {
            rebuild();
        }

        for (size_t index = 0; index < states_.size(); ++index)
        {
            was_active_[index] = states_[index].active;
            states_[index]     = ActionState{};
        }
        for (BindingRef const ref : live_)
        {
            BindingState& state = binding_state(ref);
            state.presses       = 0;
            state.releases      = 0;
        }
        deltas_.clear();

        for (ControlEvent const& event : queue_)
        {
            if (event.control.relative())
            {
                deltas_[event.control] += event.value;
                continue;
            }

            values_[event.control] = event.value;
            if (auto const found = dispatch_.find(event.control); found != dispatch_.end())
            {
                for (BindingRef const ref : found->second)
                {
                    step(ref, f32vec3{event.value, 0.0f, 0.0f}, event.ns);
                }
            }
        }

        // Deltas once, summed, stamped at the last event that carried one.
        u64 const last_ns = queue_.empty() ? 0 : queue_.back().ns;
        for (BindingRef const ref : live_)
        {
            Control const& control = binding(ref).control;
            if (control.relative())
            {
                step(ref, f32vec3{control_value(control), 0.0f, 0.0f}, last_ns);
            }
        }

        frame_events_.swap(queue_);
        queue_.clear();

        vector<f32vec3> sums(states_.size(), f32vec3{0.0f});
        for (BindingRef const ref : live_)
        {
            Binding const&      b      = binding(ref);
            BindingState const& state  = binding_state(ref);
            ActionState&        action = states_[b.action.index];
            f32vec3&            sum    = sums[b.action.index];

            if (std::holds_alternative<Down>(b.trigger))
            {
                sum += state.value;
                action.active = action.active || state.actuated;
            }
            else if (std::holds_alternative<Pressed>(b.trigger) ? state.presses > 0 : state.releases > 0)
            {
                sum += state.fire_value;
            }
        }

        for (size_t index = 0; index < states_.size(); ++index)
        {
            Action const& action = map_.actions[index];
            ActionState&  state  = states_[index];

            if (action.type == ActionType::Bool)
            {
                state.active = state.active || state.fired > 0;
                state.value  = f32vec3{state.active ? 1.0f : 0.0f, 0.0f, 0.0f};
                continue;
            }

            // Two keys that cancel leave an axis idle, held or not.
            state.value  = mask(action.type, apply(action.modifiers, sums[index], action_modifiers_[index]));
            state.active = magnitude(state.value) > 0.0f;
        }
    }

    void InputSystem::inject(ActionId action, f32vec3 value)
    {
        ActionType const type  = map_.actions[action.index].type;
        ActionState&     state = states_[action.index];

        state.value  = mask(type, value);
        state.active = magnitude(state.value) > 0.0f;
        if (type == ActionType::Bool)
        {
            state.value = f32vec3{state.active ? 1.0f : 0.0f, 0.0f, 0.0f};
        }
        if (state.active && !was_active_[action.index])
        {
            fire(action, 0);
        }
    }
}
