#pragma once

#include "input/input_map.hpp"

#include <filesystem>

// What the editor edits: the defaults and the user's layer over them. An
// edit replaces an action's bindings in one context, and the user's layer
// keeps only what differs from the defaults, so a reset is erasing an
// entry and a later change to the defaults still reaches every action the
// user left alone.
namespace encke::input
{
    class Bindings
    {
    public:
        // The defaults must read; the user's file is optional, an empty
        // layer when it does not exist.
        bool load(std::filesystem::path const& defaults, std::filesystem::path const& user);

        // From layers already read, for tests. Nothing is saved anywhere.
        void set(Layer defaults, Layer user);

        Layer const& defaults() const { return defaults_; }
        Layer const& user() const { return user_; }

        // The user's layer over the defaults.
        Layer const& merged() const { return merged_; }

        optional<InputMap> compile() const;

        // An action's bindings in a context, merged; empty if none.
        span<BindingFile const> bindings(string_view context, string_view action) const;

        // The user's layer states this action's bindings in this context.
        bool edited(string_view context, string_view action) const;

        // Replaces an action's bindings in a context. Each is normalized; a
        // list the same as the defaults' leaves the user's layer without
        // an entry.
        void set_bindings(string const& context, string const& action, vector<BindingFile> bindings);

        // Back to the defaults' bindings.
        void reset(string const& context, string const& action);

        // A new action or context in the user's layer. False, and nothing
        // done, when the name is empty or taken.
        bool add_action(string const& name, ActionFile const& action);
        bool add_context(string const& name, i32 priority, string const& description);

        // Only what the user's layer declares and the defaults do not can
        // go: the defaults hold every action the engine reads. An action
        // goes with its bindings in every context, a context with its
        // bindings.
        bool removable_action(string_view name) const;
        bool removable_context(string_view name) const;
        bool remove_action(string const& name);
        bool remove_context(string const& name);

        // The user's layer to its file.
        bool save_user();

        // Everything to the defaults' file, and the user's layer emptied,
        // its file too if there is one: what the user had becomes what the
        // repo ships.
        bool save_defaults();

        // Changed since loaded or saved.
        bool dirty() const { return dirty_; }

        // Bumped by every change, so a reader knows to compile again.
        u64 revision() const { return revision_; }

        std::filesystem::path const& defaults_path() const { return defaults_path_; }
        std::filesystem::path const& user_path() const { return user_path_; }

    private:
        void changed();

        // Drops contexts the user's layer holds nothing in.
        void prune();

        Layer defaults_;
        Layer user_;
        Layer merged_;

        std::filesystem::path defaults_path_;
        std::filesystem::path user_path_;

        bool dirty_    = false;
        u64  revision_ = 0;
    };
}
