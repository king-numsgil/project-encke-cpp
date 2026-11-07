#include "core/pch.hpp"

#include "input/bindings.hpp"

#include "core/log.hpp"

#include <algorithm>

namespace encke::input
{
    namespace
    {
        // A context's list for an action in a layer, if the layer has one.
        vector<BindingFile> const* find(Layer const& layer, string_view context, string_view action)
        {
            auto const in = layer.contexts.find(string{context});
            if (in == layer.contexts.end())
            {
                return nullptr;
            }
            auto const list = in->second.bindings.find(string{action});
            return list != in->second.bindings.end() ? &list->second : nullptr;
        }
    }

    bool Bindings::load(std::filesystem::path const& defaults, std::filesystem::path const& user)
    {
        optional<Layer> base = load_layer(defaults);
        if (!base.has_value())
        {
            return false;
        }

        Layer           edits;
        std::error_code error;
        if (!user.empty() && std::filesystem::exists(user, error))
        {
            optional<Layer> read = load_layer(user);
            if (!read.has_value())
            {
                return false;
            }
            edits = std::move(*read);
            log::info("input: %s over the defaults", user.string().c_str());
        }

        defaults_path_ = defaults;
        user_path_     = user;
        set(std::move(*base), std::move(edits));
        return true;
    }

    void Bindings::set(Layer defaults, Layer user)
    {
        defaults_ = std::move(defaults);
        user_     = std::move(user);
        merged_   = defaults_;
        merge(merged_, user_);
        dirty_ = false;
        ++revision_;
    }

    optional<InputMap> Bindings::compile() const
    {
        string const source = user_path_.empty() ? defaults_path_.string()
                                                 : defaults_path_.string() + " + " + user_path_.string();
        return input::compile(merged_, source);
    }

    span<BindingFile const> Bindings::bindings(string_view context, string_view action) const
    {
        vector<BindingFile> const* const list = find(merged_, context, action);
        return list != nullptr ? span<BindingFile const>{*list} : span<BindingFile const>{};
    }

    bool Bindings::edited(string_view context, string_view action) const
    {
        return find(user_, context, action) != nullptr;
    }

    void Bindings::set_bindings(string const& context, string const& action, vector<BindingFile> bindings)
    {
        for (BindingFile& binding : bindings)
        {
            binding = normalized(std::move(binding));
        }

        vector<BindingFile> const* const base = find(defaults_, context, action);
        bool const same = base != nullptr ? *base == bindings : bindings.empty();
        if (same)
        {
            if (auto const in = user_.contexts.find(context); in != user_.contexts.end())
            {
                in->second.bindings.erase(action);
            }
        }
        else
        {
            user_.contexts[context].bindings[action] = std::move(bindings);
        }
        changed();
    }

    void Bindings::reset(string const& context, string const& action)
    {
        if (auto const in = user_.contexts.find(context); in != user_.contexts.end())
        {
            in->second.bindings.erase(action);
        }
        changed();
    }

    bool Bindings::add_action(string const& name, ActionFile const& action)
    {
        if (name.empty() || merged_.actions.contains(name))
        {
            return false;
        }
        user_.actions[name] = action;
        changed();
        return true;
    }

    bool Bindings::add_context(string const& name, i32 priority, string const& description)
    {
        if (name.empty() || merged_.contexts.contains(name))
        {
            return false;
        }
        ContextFile& context = user_.contexts[name];
        context.priority     = priority;
        if (!description.empty())
        {
            context.description = description;
        }
        changed();
        return true;
    }

    bool Bindings::removable_action(string_view name) const
    {
        string const key{name};
        return user_.actions.contains(key) && !defaults_.actions.contains(key);
    }

    bool Bindings::removable_context(string_view name) const
    {
        string const key{name};
        return user_.contexts.contains(key) && !defaults_.contexts.contains(key);
    }

    bool Bindings::remove_action(string const& name)
    {
        if (!removable_action(name))
        {
            return false;
        }
        user_.actions.erase(name);
        for (auto& entry : user_.contexts)
        {
            entry.second.bindings.erase(name);
        }
        changed();
        return true;
    }

    bool Bindings::remove_context(string const& name)
    {
        if (!removable_context(name))
        {
            return false;
        }
        user_.contexts.erase(name);
        changed();
        return true;
    }

    bool Bindings::save_user()
    {
        if (user_path_.empty())
        {
            log::error("input: no file for the user's bindings in this run");
            return false;
        }
        if (!save_layer(user_, user_path_))
        {
            return false;
        }
        dirty_ = false;
        return true;
    }

    bool Bindings::save_defaults()
    {
        if (!save_layer(merged_, defaults_path_))
        {
            return false;
        }

        std::error_code error;
        bool const      had_user = std::filesystem::exists(user_path_, error);
        defaults_                = merged_;
        user_                    = Layer{};
        if (had_user && !save_layer(user_, user_path_))
        {
            // The defaults hold everything; an old user file would only
            // repeat it, harmlessly.
            dirty_ = true;
            return false;
        }
        dirty_ = false;
        return true;
    }

    void Bindings::prune()
    {
        std::erase_if(user_.contexts, [](auto const& entry) {
            ContextFile const& context = entry.second;
            return context.bindings.empty() && !context.priority.has_value() && !context.description.has_value();
        });
    }

    void Bindings::changed()
    {
        prune();
        merged_ = defaults_;
        merge(merged_, user_);
        dirty_ = true;
        ++revision_;
    }
}
