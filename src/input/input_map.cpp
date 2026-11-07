#include "core/pch.hpp"

#include "input/input_map.hpp"

#include "core/log.hpp"

#include <algorithm>

namespace encke::input
{
    namespace
    {
        bool check_all(span<Modifier const> modifiers, string_view source, string_view where)
        {
            bool ok = true;
            for (Modifier const& modifier : modifiers)
            {
                if (optional<string> const problem = check(modifier))
                {
                    log::error("%.*s: %.*s: %s", static_cast<int>(source.size()), source.data(),
                               static_cast<int>(where.size()), where.data(), problem->c_str());
                    ok = false;
                }
            }
            return ok;
        }
    }

    optional<ActionId> InputMap::find_action(string_view name) const
    {
        auto const found = std::ranges::lower_bound(actions, name, {}, &Action::name);
        if (found == actions.end() || found->name != name)
        {
            return nullopt;
        }
        return ActionId{static_cast<u32>(found - actions.begin())};
    }

    optional<ContextId> InputMap::find_context(string_view name) const
    {
        auto const found = std::ranges::find(contexts, name, &Context::name);
        if (found == contexts.end())
        {
            return nullopt;
        }
        return ContextId{static_cast<u32>(found - contexts.begin())};
    }

    optional<InputMap> compile(Layer const& layer, string_view source)
    {
        InputMap map;
        bool     ok = true;

        // A std::map iterates by name, which find_action relies on.
        for (auto const& [name, action] : layer.actions)
        {
            ok = check_all(action.modifiers, source, "action " + name) && ok;
            map.actions.push_back(Action{.name        = name,
                                         .type        = action.type,
                                         .description = action.description,
                                         .modifiers   = action.modifiers});
        }

        for (auto const& [name, context] : layer.contexts)
        {
            Context compiled{.name = name, .priority = context.priority.value_or(0), .bindings = {}};
            for (auto const& [action_name, bindings] : context.bindings)
            {
                string const             where  = "context " + name + ", action " + action_name;
                optional<ActionId> const action = map.find_action(action_name);
                if (!action.has_value())
                {
                    log::error("%.*s: %s: no layer declares this action", static_cast<int>(source.size()),
                               source.data(), where.c_str());
                    ok = false;
                    continue;
                }

                for (BindingFile const& binding : bindings)
                {
                    optional<Control> const control = parse_control(binding.control);
                    if (!control.has_value())
                    {
                        log::error("%.*s: %s: \"%s\" is not a control", static_cast<int>(source.size()),
                                   source.data(), where.c_str(), binding.control.c_str());
                        ok = false;
                        continue;
                    }
                    ok = check_all(binding.modifiers, source, where) && ok;
                    compiled.bindings.push_back(Binding{.control   = *control,
                                                        .action    = *action,
                                                        .modifiers = binding.modifiers,
                                                        .trigger   = binding.trigger,
                                                        .consume   = binding.consume});
                }
            }
            map.contexts.push_back(std::move(compiled));
        }

        // Already by name; stable keeps that among equal priorities.
        std::ranges::stable_sort(map.contexts, std::ranges::greater{}, &Context::priority);

        if (!ok)
        {
            return nullopt;
        }
        return map;
    }
}
