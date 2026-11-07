#include "core/pch.hpp"

#include "input/bindings.hpp"

#include <catch2/catch_test_macros.hpp>

namespace encke::input
{
    namespace
    {
        Layer layer(string_view text)
        {
            optional<Layer> read = parse_layer(text, "test");
            REQUIRE(read.has_value());
            return std::move(*read);
        }

        constexpr string_view kDefaults = R"({
            "actions": { "jump": {}, "fire": {} },
            "contexts": { "on_foot": { "priority": 10, "bindings": {
                "jump": [{ "control": "key:Space" }],
                "fire": [{ "control": "mouse:left" }]
            } } }
        })";

        BindingFile bound(string control)
        {
            return BindingFile{.control = std::move(control), .modifiers = {}, .trigger = {}, .consume = {}};
        }
    }

    TEST_CASE("an edit goes to the user's layer and shows merged", "[input]")
    {
        Bindings bindings;
        bindings.set(layer(kDefaults), Layer{});
        u64 const before = bindings.revision();

        bindings.set_bindings("on_foot", "jump", {bound("key:Space"), bound("joy:044f:0422:button:0")});
        CHECK(bindings.revision() != before);
        CHECK(bindings.dirty());
        CHECK(bindings.edited("on_foot", "jump"));
        CHECK_FALSE(bindings.edited("on_foot", "fire"));
        REQUIRE(bindings.bindings("on_foot", "jump").size() == 2);
        CHECK(bindings.user().contexts.at("on_foot").bindings.size() == 1);
        CHECK(bindings.compile().has_value());
    }

    TEST_CASE("an edit back to the defaults leaves no entry", "[input]")
    {
        Bindings bindings;
        bindings.set(layer(kDefaults), Layer{});

        bindings.set_bindings("on_foot", "jump", {bound("key:J")});
        REQUIRE(bindings.edited("on_foot", "jump"));

        // Spelled with every default stated: normalized, it is the same.
        bindings.set_bindings("on_foot", "jump",
                              {BindingFile{.control   = "key:Space",
                                           .modifiers = vector<Modifier>{},
                                           .trigger   = Down{},
                                           .consume   = true}});
        CHECK_FALSE(bindings.edited("on_foot", "jump"));
        CHECK(bindings.user().contexts.empty());

        // Unbinding is an edit; a reset undoes it.
        bindings.set_bindings("on_foot", "fire", {});
        CHECK(bindings.edited("on_foot", "fire"));
        CHECK(bindings.bindings("on_foot", "fire").empty());
        bindings.reset("on_foot", "fire");
        CHECK_FALSE(bindings.edited("on_foot", "fire"));
        CHECK(bindings.bindings("on_foot", "fire").size() == 1);
    }

    TEST_CASE("new actions and contexts go to the user's layer", "[input]")
    {
        Bindings bindings;
        bindings.set(layer(kDefaults), Layer{});

        CHECK(bindings.add_action("throttle", ActionFile{.type = ActionType::Axis1D, .description = {}, .modifiers = {}}));
        CHECK_FALSE(bindings.add_action("jump", ActionFile{}));
        CHECK_FALSE(bindings.add_action("", ActionFile{}));
        CHECK(bindings.add_context("flying", 20, "In the pilot's seat"));
        CHECK_FALSE(bindings.add_context("on_foot", 0, ""));

        bindings.set_bindings("flying", "throttle", {bound("joy:044f:0447:axis:2")});
        optional<InputMap> const map = bindings.compile();
        REQUIRE(map.has_value());
        CHECK(map->contexts.front().name == "flying");   // highest priority first
        CHECK(map->require("throttle", ActionType::Axis1D).has_value());
        CHECK(bindings.user().contexts.at("flying").description == "In the pilot's seat");
    }

    TEST_CASE("only what the user declared can be removed", "[input]")
    {
        Bindings bindings;
        bindings.set(layer(kDefaults), Layer{});
        REQUIRE(bindings.add_action("attitude", ActionFile{.type = ActionType::Axis3D, .description = {}, .modifiers = {}}));
        REQUIRE(bindings.add_context("ship", 20, ""));
        bindings.set_bindings("ship", "attitude", {bound("joy:044f:0422:axis:0")});
        bindings.set_bindings("on_foot", "attitude", {bound("key:Q")});

        CHECK_FALSE(bindings.removable_action("jump"));
        CHECK_FALSE(bindings.remove_action("jump"));
        CHECK_FALSE(bindings.removable_context("on_foot"));

        // Its bindings go with it, in every context.
        CHECK(bindings.remove_action("attitude"));
        CHECK_FALSE(bindings.merged().actions.contains("attitude"));
        CHECK(bindings.bindings("on_foot", "attitude").empty());
        CHECK(bindings.bindings("ship", "attitude").empty());
        CHECK(bindings.compile().has_value());

        CHECK(bindings.remove_context("ship"));
        CHECK_FALSE(bindings.merged().contexts.contains("ship"));
        CHECK(bindings.user().contexts.empty());
    }

    TEST_CASE("a binding's component is the swizzle last in its chain", "[input]")
    {
        BindingFile binding = bound("joy:044f:0422:axis:1");
        CHECK(component_of(binding) == 0);

        set_component(binding, 2);
        CHECK(component_of(binding) == 2);
        REQUIRE(binding.modifiers.has_value());
        CHECK(binding.modifiers->size() == 1);

        // Other modifiers stay, before it; a new component replaces it.
        binding.modifiers->insert(binding.modifiers->begin(), Negate{});
        set_component(binding, 1);
        CHECK(component_of(binding) == 1);
        REQUIRE(binding.modifiers->size() == 2);
        CHECK(std::holds_alternative<Negate>((*binding.modifiers)[0]));

        // Back to x: no swizzle at all.
        set_component(binding, 0);
        CHECK(component_of(binding) == 0);
        REQUIRE(binding.modifiers.has_value());
        CHECK(binding.modifiers->size() == 1);

        BindingFile plain = bound("key:W");
        set_component(plain, 0);
        CHECK_FALSE(plain.modifiers.has_value());
    }

    TEST_CASE("saving defaults takes the user's edits into them", "[input]")
    {
        std::filesystem::path const dir      = std::filesystem::temp_directory_path() / "encke_bindings_test";
        std::filesystem::path const defaults = dir / "default.json";
        std::filesystem::path const user     = dir / "input.json";
        std::filesystem::create_directories(dir);
        REQUIRE(save_layer(layer(kDefaults), defaults));
        std::filesystem::remove(user);

        Bindings bindings;
        REQUIRE(bindings.load(defaults, user));
        CHECK_FALSE(bindings.dirty());

        bindings.set_bindings("on_foot", "jump", {bound("key:J")});
        REQUIRE(bindings.save_user());
        CHECK_FALSE(bindings.dirty());

        Bindings again;
        REQUIRE(again.load(defaults, user));
        CHECK(again.edited("on_foot", "jump"));
        CHECK(again.bindings("on_foot", "jump")[0].control == "key:J");

        REQUIRE(again.save_defaults());
        CHECK(again.user().contexts.empty());
        CHECK_FALSE(again.edited("on_foot", "jump"));

        Bindings third;
        REQUIRE(third.load(defaults, user));
        CHECK(third.bindings("on_foot", "jump")[0].control == "key:J");
        CHECK_FALSE(third.edited("on_foot", "jump"));

        std::filesystem::remove_all(dir);
    }
}
