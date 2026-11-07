#include "core/pch.hpp"

#include "input/input_map.hpp"

#include <catch2/catch_test_macros.hpp>

namespace encke::input
{
    TEST_CASE("the default bindings read and compile", "[input]")
    {
        optional<Layer> const layer = load_layer(std::filesystem::path{ENCKE_ASSET_DIR} / "input" / "default.json");
        REQUIRE(layer.has_value());

        optional<InputMap> const map = compile(*layer, "default.json");
        REQUIRE(map.has_value());
        CHECK(map->find_action("walk_move").has_value());
        CHECK(map->actions[map->find_action("fly_move")->index].type == ActionType::Axis3D);

        // Highest priority first.
        REQUIRE(map->find_context("engine").has_value());
        CHECK(map->find_context("engine")->index == 0);
        for (size_t index = 1; index < map->contexts.size(); ++index)
        {
            CHECK(map->contexts[index - 1].priority >= map->contexts[index].priority);
        }
    }

    TEST_CASE("a layer read and written reads back the same", "[input]")
    {
        optional<Layer> const layer = parse_layer(R"({
            "actions": { "pitch": { "type": "1d", "modifiers": [{ "type": "dead_zone", "lower": 0.05 }] } },
            "contexts": { "flying": { "priority": 5, "bindings": {
                "pitch": [{ "control": "joy:044f:0422:axis:1",
                            "modifiers": [{ "type": "curve", "exponent": 1.5 }, { "type": "negate" }] }]
            } } }
        })", "test");
        REQUIRE(layer.has_value());

        optional<Layer> const again = parse_layer(to_json(*layer), "written");
        REQUIRE(again.has_value());
        BindingFile const& binding = again->contexts.at("flying").bindings.at("pitch").at(0);
        CHECK(binding.control == "joy:044f:0422:axis:1");
        REQUIRE(binding.modifiers.has_value());
        REQUIRE(binding.modifiers->size() == 2);
        CHECK(std::get<Curve>((*binding.modifiers)[0]).exponent == 1.5f);
        CHECK(std::holds_alternative<Negate>((*binding.modifiers)[1]));
        CHECK(std::holds_alternative<Down>(trigger_of(binding)));
        CHECK(consumes(binding));
        CHECK(std::get<DeadZone>(again->actions.at("pitch").modifiers->at(0)).lower == 0.05f);
        CHECK(again->contexts.at("flying").priority == 5);
        CHECK(*again == *layer);
    }

    TEST_CASE("a layer leaves defaults out when written", "[input]")
    {
        Layer layer;
        layer.actions["jump"] = ActionFile{};
        layer.contexts["on_foot"].bindings["jump"] = {normalized(BindingFile{
            .control   = "key:Space",
            .modifiers = vector<Modifier>{},
            .trigger   = Down{},
            .consume   = true,
        })};

        string const json = to_json(layer);
        CHECK(json.find("modifiers") == string::npos);
        CHECK(json.find("trigger") == string::npos);
        CHECK(json.find("consume") == string::npos);
        CHECK(json.find("priority") == string::npos);

        BindingFile const kept = normalized(BindingFile{.control   = "key:G",
                                                        .modifiers = vector<Modifier>{Negate{}},
                                                        .trigger   = Pressed{},
                                                        .consume   = false});
        CHECK(kept.modifiers.has_value());
        CHECK(kept.trigger.has_value());
        CHECK(kept.consume == false);
    }

    TEST_CASE("modifiers and triggers are made and named by index", "[input]")
    {
        for (size_t index = 0; index < kModifierCount; ++index)
        {
            CHECK(make_modifier(index).index() == index);
        }
        for (size_t index = 0; index < kTriggerCount; ++index)
        {
            CHECK(make_trigger(index).index() == index);
        }
        CHECK(std::get<Swizzle>(make_modifier(4)).order == "yxz");
        CHECK(modifier_name(4) == "swizzle");
        CHECK(trigger_name(1) == "pressed");
    }

    TEST_CASE("a layer saved reads back the same", "[input]")
    {
        optional<Layer> const layer = load_layer(std::filesystem::path{ENCKE_ASSET_DIR} / "input" / "default.json");
        REQUIRE(layer.has_value());

        std::filesystem::path const path = std::filesystem::temp_directory_path() / "encke_layer_test.json";
        REQUIRE(save_layer(*layer, path));
        optional<Layer> const again = load_layer(path);
        std::filesystem::remove(path);
        REQUIRE(again.has_value());
        CHECK(*again == *layer);
    }

    TEST_CASE("a layer over another replaces what it names and keeps the rest", "[input]")
    {
        optional<Layer> base = parse_layer(R"({
            "actions": { "jump": {}, "fire": {} },
            "contexts": { "on_foot": { "priority": 10, "bindings": {
                "jump": [{ "control": "key:Space" }],
                "fire": [{ "control": "mouse:left" }, { "control": "key:F" }]
            } } }
        })", "base");
        optional<Layer> const user = parse_layer(R"({
            "actions": { "wave": {} },
            "contexts": {
                "on_foot": { "bindings": { "fire": [{ "control": "mouse:right" }], "wave": [{ "control": "key:G" }] } },
                "menu": { "priority": 50 }
            }
        })", "user");
        REQUIRE(base.has_value());
        REQUIRE(user.has_value());

        merge(*base, *user);
        ContextFile const& on_foot = base->contexts.at("on_foot");
        CHECK(on_foot.priority == 10);   // the user's layer left it
        CHECK(on_foot.bindings.at("jump").at(0).control == "key:Space");
        REQUIRE(on_foot.bindings.at("fire").size() == 1);   // replaced whole
        CHECK(on_foot.bindings.at("fire")[0].control == "mouse:right");
        CHECK(base->actions.contains("wave"));
        CHECK(base->contexts.at("menu").priority == 50);
        CHECK(compile(*base, "merged").has_value());
    }

    TEST_CASE("a layer with a mistake in it is refused", "[input]")
    {
        // Unknown keys, an unknown modifier, a misspelt type.
        CHECK_FALSE(parse_layer(R"({ "actoins": {} })", "test"));
        CHECK_FALSE(parse_layer(R"({ "actions": { "a": { "type": "4d" } } })", "test"));
        CHECK_FALSE(parse_layer(
            R"({ "contexts": { "c": { "bindings": { "a": [{ "control": "key:A", "modifiers": [{ "type": "wobble" }] }] } } } })",
            "test"));

        // Reads, but does not compile: an undeclared action, a bad control,
        // a modifier out of range.
        auto const compiles = [](string_view text) {
            optional<Layer> const layer = parse_layer(text, "test");
            REQUIRE(layer.has_value());
            return compile(*layer, "test").has_value();
        };
        CHECK_FALSE(compiles(R"({ "contexts": { "c": { "bindings": { "a": [{ "control": "key:A" }] } } } })"));
        CHECK_FALSE(compiles(
            R"({ "actions": { "a": {} }, "contexts": { "c": { "bindings": { "a": [{ "control": "key:Nope" }] } } } })"));
        CHECK_FALSE(compiles(
            R"({ "actions": { "a": { "modifiers": [{ "type": "dead_zone", "lower": 1.5 }] } } })"));
        CHECK(compiles(R"({ "actions": { "a": {} }, "contexts": { "c": { "bindings": { "a": [] } } } })"));
    }
}
