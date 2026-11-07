#include "core/pch.hpp"

#include "input/layer.hpp"

#include "core/log.hpp"

#include <fstream>
#include <sstream>

#include <glaze/glaze.hpp>

// Names as a binding file spells them. Every alternative is listed, so a new
// modifier or trigger fails to compile until it has a name.
template<>
struct glz::meta<encke::input::ActionType>
{
    using enum encke::input::ActionType;
    static constexpr auto value = enumerate("bool", Bool, "1d", Axis1D, "2d", Axis2D, "3d", Axis3D);
};

template<>
struct glz::meta<encke::input::Half>
{
    using enum encke::input::Half;
    static constexpr auto value = enumerate("positive", Positive, "negative", Negative);
};

template<>
struct glz::meta<encke::input::Modifier>
{
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"dead_zone", "curve", "negate", "scale", "swizzle", "saturation",
                                           "split", "threshold"};
};

template<>
struct glz::meta<encke::input::Trigger>
{
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"down", "pressed", "released"};
};

namespace encke::input
{
    namespace
    {
        constexpr glz::opts kRead{.comments = true};
    }

    optional<Layer> parse_layer(string_view text, string_view source)
    {
        Layer layer;
        // glaze reads from a null-terminated buffer.
        string const buffer{text};
        if (glz::error_ctx const error = glz::read<kRead>(layer, buffer))
        {
            string const message = glz::format_error(error, buffer);
            log::error("%.*s: %s", static_cast<int>(source.size()), source.data(), message.c_str());
            return nullopt;
        }
        return layer;
    }

    optional<Layer> load_layer(std::filesystem::path const& path)
    {
        std::ifstream file{path, std::ios::binary};
        if (!file)
        {
            log::error("input: cannot open %s", path.string().c_str());
            return nullopt;
        }

        std::ostringstream text;
        text << file.rdbuf();
        return parse_layer(text.str(), path.string());
    }

    void merge(Layer& into, Layer const& over)
    {
        for (auto const& [name, action] : over.actions)
        {
            into.actions[name] = action;
        }

        for (auto const& [name, context] : over.contexts)
        {
            ContextFile& target = into.contexts[name];
            if (context.priority.has_value())
            {
                target.priority = context.priority;
            }
            for (auto const& [action, bindings] : context.bindings)
            {
                target.bindings[action] = bindings;
            }
        }
    }

    namespace
    {
        // One constructor per alternative, picked by index. Built straight
        // into the result: assigning over a variant already holding
        // something made GCC at -O3 see a Swizzle's string freed that was
        // never allocated (-Wfree-nonheap-object), a false positive.
        template<class Variant, size_t... I>
        Variant make_alternative(size_t index, std::index_sequence<I...>)
        {
            using Make = Variant (*)();
            static constexpr array<Make, sizeof...(I)> kMake{
                {+[]() -> Variant { return std::variant_alternative_t<I, Variant>{}; }...}};
            return kMake[index]();
        }

        template<class Variant>
        Variant make_alternative(size_t index)
        {
            return make_alternative<Variant>(index, std::make_index_sequence<std::variant_size_v<Variant>>{});
        }
    }

    string_view modifier_name(size_t index)
    {
        return glz::meta<Modifier>::ids[index];
    }

    string_view trigger_name(size_t index)
    {
        return glz::meta<Trigger>::ids[index];
    }

    string_view type_name(ActionType type)
    {
        switch (type)
        {
        case ActionType::Bool:   return "bool";
        case ActionType::Axis1D: return "1d";
        case ActionType::Axis2D: return "2d";
        case ActionType::Axis3D: return "3d";
        }
        return "?";
    }

    Modifier make_modifier(size_t index)
    {
        return make_alternative<Modifier>(index);
    }

    Trigger make_trigger(size_t index)
    {
        return make_alternative<Trigger>(index);
    }

    BindingFile normalized(BindingFile binding)
    {
        if (binding.modifiers.has_value() && binding.modifiers->empty())
        {
            binding.modifiers.reset();
        }
        if (binding.trigger.has_value() && std::holds_alternative<Down>(*binding.trigger))
        {
            binding.trigger.reset();
        }
        if (binding.consume == true)
        {
            binding.consume.reset();
        }
        return binding;
    }

    size_t component_of(BindingFile const& binding)
    {
        span<Modifier const> const modifiers = modifiers_of(binding);
        if (modifiers.empty())
        {
            return 0;
        }
        Swizzle const* const swizzle = std::get_if<Swizzle>(&modifiers.back());
        if (swizzle == nullptr || swizzle->order.empty() || swizzle->order[0] < 'x' || swizzle->order[0] > 'z')
        {
            return 0;
        }
        return static_cast<size_t>(swizzle->order[0] - 'x');
    }

    void set_component(BindingFile& binding, size_t component)
    {
        vector<Modifier> modifiers = binding.modifiers.value_or(vector<Modifier>{});
        if (!modifiers.empty() && std::holds_alternative<Swizzle>(modifiers.back()))
        {
            modifiers.pop_back();
        }
        // x and the chosen component trade places.
        constexpr array<char const*, 3> kOrder{{"xyz", "yxz", "zyx"}};
        if (component > 0 && component < kOrder.size())
        {
            modifiers.push_back(Swizzle{.order = kOrder[component]});
        }
        binding.modifiers = std::move(modifiers);
        binding           = normalized(std::move(binding));
    }

    span<Modifier const> modifiers_of(BindingFile const& binding)
    {
        return binding.modifiers.has_value() ? span<Modifier const>{*binding.modifiers} : span<Modifier const>{};
    }

    Trigger trigger_of(BindingFile const& binding)
    {
        return binding.trigger.value_or(Down{});
    }

    bool consumes(BindingFile const& binding)
    {
        return binding.consume.value_or(true);
    }

    string to_json(Layer const& layer)
    {
        // Absent optionals are left out.
        auto const json = glz::write<glz::opts{.prettify = true}>(layer);
        return json.has_value() ? *json : string{};
    }

    bool save_layer(Layer const& layer, std::filesystem::path const& path)
    {
        string const text = to_json(layer) + "\n";

        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!file)
            {
                log::error("input: cannot write %s", temporary.string().c_str());
                return false;
            }
        }

        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            log::error("input: cannot replace %s: %s", path.string().c_str(), error.message().c_str());
            return false;
        }
        log::info("input: saved %s", path.string().c_str());
        return true;
    }
}
