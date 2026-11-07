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

    // Fields that are absent take their default, and a layer the editor
    // writes leaves defaults out, so the files stay short. normalized()
    // makes a binding read the way it would be written.
    struct ActionFile
    {
        ActionType       type = ActionType::Bool;
        optional<string> description;
        // After the bindings are summed: a radial dead zone, say.
        optional<vector<Modifier>> modifiers;

        bool operator==(ActionFile const&) const = default;
    };

    struct BindingFile
    {
        string                     control;   // input/control's text form
        optional<vector<Modifier>> modifiers;
        optional<Trigger>          trigger;   // absent is down
        // Lower contexts do not see this control while this one is active.
        // Absent is true.
        optional<bool> consume;

        bool operator==(BindingFile const&) const = default;
    };

    struct ContextFile
    {
        // Higher runs first and consumes what it binds. Absent in a layer
        // leaves the one below's; absent in all is 0.
        optional<i32>    priority;
        optional<string> description;
        // Each action's list replaces the layer below's whole, so a layer
        // states an action's bindings in full or not at all. An empty list
        // unbinds it.
        std::map<string, vector<BindingFile>> bindings;

        bool operator==(ContextFile const&) const = default;
    };

    // The names the files use, by variant index, and a default-constructed
    // alternative by index: for the editor's lists.
    inline constexpr size_t kModifierCount = std::variant_size_v<Modifier>;
    inline constexpr size_t kTriggerCount  = std::variant_size_v<Trigger>;
    string_view modifier_name(size_t index);
    string_view trigger_name(size_t index);
    string_view type_name(ActionType type);
    Modifier    make_modifier(size_t index);
    Trigger     make_trigger(size_t index);

    // Defaults left out: no modifiers, the down trigger, consuming.
    BindingFile normalized(BindingFile binding);

    // Which component of its action a binding drives: 0 x, 1 y, 2 z. A
    // control's value enters in x, and a swizzle last in the chain moves it;
    // the component is where that swizzle sends x, else x. Setting it
    // replaces that last swizzle, or removes it for x.
    size_t component_of(BindingFile const& binding);
    void   set_component(BindingFile& binding, size_t component);

    span<Modifier const> modifiers_of(BindingFile const& binding);
    Trigger              trigger_of(BindingFile const& binding);
    bool                 consumes(BindingFile const& binding);

    struct Layer
    {
        // An action here replaces the layer below's of the same name.
        std::map<string, ActionFile>  actions;
        std::map<string, ContextFile> contexts;

        bool operator==(Layer const&) const = default;
    };

    // Reads one file; logs what is wrong and where. Comments are allowed,
    // unknown keys are not.
    optional<Layer> parse_layer(string_view text, string_view source);
    optional<Layer> load_layer(std::filesystem::path const& path);

    // Lays `over` on `into`.
    void merge(Layer& into, Layer const& over);

    string to_json(Layer const& layer);

    // Writes the layer as JSON, through a temporary file renamed over
    // `path`, so a failed write leaves the old file. Comments are not kept.
    bool save_layer(Layer const& layer, std::filesystem::path const& path);
}
