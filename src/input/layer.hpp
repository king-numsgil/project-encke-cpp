#pragma once

#include "input/modifiers.hpp"
#include "input/triggers.hpp"

#include <filesystem>
#include <map>

// A binding file, as written: actions, and contexts holding bindings by
// action. Files stack as layers: the defaults in the repo, then the user's
// edits, later mods. merge() lays one over another, and compile() in
// input/input_map turns the result into what the input system runs.
//
// Every name is text, so a layer can add actions and contexts the engine
// does not know; the engine asks for the ones it reads by name.
namespace encke::input
{
    enum class ActionType : u8
    {
        Bool,
        Axis1D,
        Axis2D,
        Axis3D,
    };

    struct ActionFile
    {
        ActionType type = ActionType::Bool;
        string     description;
        // After the bindings are summed: a radial dead zone, say.
        vector<Modifier> modifiers;
    };

    struct BindingFile
    {
        string           control;   // input/control's text form
        vector<Modifier> modifiers;
        Trigger          trigger = Down{};
        // Lower contexts do not see this control while this one is active.
        bool consume = true;
    };

    struct ContextFile
    {
        // Higher runs first and consumes what it binds. Absent in a layer
        // leaves the one below's; absent in all is 0.
        optional<i32> priority;
        // Each action's list replaces the layer below's whole, so a layer
        // states an action's bindings in full or not at all. An empty list
        // unbinds it.
        std::map<string, vector<BindingFile>> bindings;
    };

    struct Layer
    {
        // An action here replaces the layer below's of the same name.
        std::map<string, ActionFile>  actions;
        std::map<string, ContextFile> contexts;
    };

    // Reads one file; logs what is wrong and where. Comments are allowed,
    // unknown keys are not.
    optional<Layer> parse_layer(string_view text, string_view source);
    optional<Layer> load_layer(std::filesystem::path const& path);

    // Lays `over` on `into`.
    void merge(Layer& into, Layer const& over);

    string to_json(Layer const& layer);
}
