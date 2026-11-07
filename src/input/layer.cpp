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

    string to_json(Layer const& layer)
    {
        auto const json = glz::write<glz::opts{.prettify = true}>(layer);
        return json.has_value() ? *json : string{};
    }
}
