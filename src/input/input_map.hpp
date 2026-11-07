#pragma once

#include "input/control.hpp"
#include "input/layer.hpp"

// A merged layer, checked and resolved: names looked up, controls parsed,
// contexts in the order they run. The input system runs one; the editor
// will edit layers and compile again.
namespace encke::input
{
    struct ActionId
    {
        u32 index = 0;

        auto operator<=>(ActionId const&) const = default;
    };

    struct ContextId
    {
        u32 index = 0;

        auto operator<=>(ContextId const&) const = default;
    };

    struct Action
    {
        string           name;
        ActionType       type = ActionType::Bool;
        string           description;
        vector<Modifier> modifiers;
    };

    struct Binding
    {
        Control          control;
        ActionId         action;
        vector<Modifier> modifiers;
        Trigger          trigger = Down{};
        bool             consume = true;
    };

    struct Context
    {
        string          name;
        i32             priority = 0;
        vector<Binding> bindings;
    };

    struct InputMap
    {
        // By name.
        vector<Action> actions;
        // Highest priority first, then by name.
        vector<Context> contexts;

        optional<ActionId>  find_action(string_view name) const;
        optional<ContextId> find_context(string_view name) const;
    };

    // Logs every mistake, naming `source`, and refuses the whole map if there
    // is one: a binding to an action no layer declares, a control that does
    // not parse, a modifier's settings out of range.
    optional<InputMap> compile(Layer const& layer, string_view source);
}
