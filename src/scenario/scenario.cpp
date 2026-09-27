#include "core/pch.hpp"

#include "scenario/scenario.hpp"

#include "core/log.hpp"

#include <fstream>
#include <sstream>

#include <glaze/glaze.hpp>

// Names as a scenario file spells them. Every enum and step is listed here,
// so a new one fails to compile until it has a name.
template<>
struct glz::meta<encke::Tonemap>
{
    using enum encke::Tonemap;
    static constexpr auto value = enumerate("aces", Aces, "agx", AgX, "neutral", PbrNeutral);
};

template<>
struct glz::meta<encke::DebugView>
{
    using enum encke::DebugView;
    static constexpr auto value = enumerate("clustered", Lit, "brute_force", BruteForce);
};

template<>
struct glz::meta<encke::DebugWindow>
{
    using enum encke::DebugWindow;
    static constexpr auto value = enumerate("cluster_heat", ClusterHeat, "normals", Normals,
                                            "motion", Motion, "cascades", Cascades);
};

template<>
struct glz::meta<encke::Wireframe>
{
    using enum encke::Wireframe;
    static constexpr auto value = enumerate("off", Off, "overlay", Overlay, "only", Only);
};

template<>
struct glz::meta<encke::Movement>
{
    using enum encke::Movement;
    static constexpr auto value = enumerate("walk", Walk, "jetpack", Jetpack);
};

template<>
struct glz::meta<encke::scenario::Shape>
{
    using enum encke::scenario::Shape;
    static constexpr auto value = enumerate("box", Box, "sphere", Sphere);
};

template<>
struct glz::meta<encke::scenario::Step>
{
    static constexpr std::string_view tag = "op";
    static constexpr auto ids = std::array{"settle", "capture", "camera", "fly", "wait",
                                           "freeze_terrain", "thaw_terrain", "interactive", "set",
                                           "spawn", "walk"};
};

namespace encke::scenario
{
    namespace
    {
        // Comments, since the files are written by hand. Unknown keys stay an
        // error: a misspelt setting would otherwise do nothing, silently.
        constexpr glz::opts kRead{.comments = true};

        // What the types cannot say, checked before anything runs.
        bool validate(Scenario const& scenario, string_view source)
        {
            bool ok = true;
            for (size_t index = 0; index < scenario.steps.size(); ++index)
            {
                Step const& step = scenario.steps[index];
                if (Capture const* const capture = std::get_if<Capture>(&step))
                {
                    std::filesystem::path const file{capture->file};
                    if (capture->file.empty() || file.is_absolute() || file.has_root_path())
                    {
                        log::error("%.*s: step %zu: capture wants a file name relative to the "
                                   "output folder, got \"%s\"",
                                   static_cast<int>(source.size()), source.data(), index,
                                   capture->file.c_str());
                        ok = false;
                    }
                }
                else if (Settings const* const settings = std::get_if<Settings>(&step))
                {
                    for (auto const& [name, set] : {std::pair{"srgb_ui", settings->srgb_ui.has_value()},
                                                    std::pair{"collision_lod", settings->collision_lod.has_value()}})
                    {
                        if (set)
                        {
                            log::error("%.*s: step %zu: %s is chosen at startup; set it in \"settings\"",
                                       static_cast<int>(source.size()), source.data(), index, name);
                            ok = false;
                        }
                    }
                }
            }
            return ok;
        }
    }

    optional<Scenario> parse(string_view text, string_view source)
    {
        Scenario scenario;
        // glaze reads from a null-terminated buffer.
        string const buffer{text};
        if (glz::error_ctx const error = glz::read<kRead>(scenario, buffer))
        {
            string const message = glz::format_error(error, buffer);
            log::error("%.*s: %s", static_cast<int>(source.size()), source.data(), message.c_str());
            return nullopt;
        }

        if (!validate(scenario, source))
        {
            return nullopt;
        }
        return scenario;
    }

    optional<Scenario> load(std::filesystem::path const& path)
    {
        std::ifstream file{path, std::ios::binary};
        if (!file)
        {
            log::error("scenario: cannot open %s", path.string().c_str());
            return nullopt;
        }

        std::ostringstream text;
        text << file.rdbuf();

        optional<Scenario> scenario = parse(text.str(), path.string());
        if (scenario.has_value() && scenario->name.empty())
        {
            scenario->name = path.stem().string();
        }
        return scenario;
    }

    string to_json(Step const& step)
    {
        // Optional fields that are empty are left out.
        auto const json = glz::write_json(step);
        return json.has_value() ? *json : string{};
    }

    string_view op_name(Step const& step)
    {
        return glz::meta<Step>::ids[step.index()];
    }
}
