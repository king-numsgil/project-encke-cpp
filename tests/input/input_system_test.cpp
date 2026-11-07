#include "core/pch.hpp"

#include "input/input_system.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace encke::input
{
    namespace
    {
        // A system running `text`, every context enabled.
        InputSystem running(string_view text)
        {
            optional<Layer> const layer = parse_layer(text, "test");
            REQUIRE(layer.has_value());
            optional<InputMap> map = compile(*layer, "test");
            REQUIRE(map.has_value());

            InputSystem input;
            input.set_map(std::move(*map));
            for (u32 index = 0; index < input.map().contexts.size(); ++index)
            {
                input.set_enabled(ContextId{index}, true);
            }
            return input;
        }

        ActionId id(InputSystem const& input, string_view name)
        {
            optional<ActionId> const action = input.map().find_action(name);
            REQUIRE(action.has_value());
            return *action;
        }

        void push(InputSystem& input, string_view control, f32 value, u64 ns = 0)
        {
            optional<Control> const parsed = parse_control(control);
            REQUIRE(parsed.has_value());
            input.push(ControlEvent{.control = *parsed, .value = value, .ns = ns});
        }

        constexpr string_view kWalk = R"({
            "actions": { "move": { "type": "2d" }, "jump": {}, "gear": {}, "look": { "type": "2d" } },
            "contexts": { "on_foot": { "bindings": {
                "move": [
                    { "control": "key:D" },
                    { "control": "key:A", "modifiers": [{ "type": "negate" }] },
                    { "control": "key:W", "modifiers": [{ "type": "swizzle", "order": "yxz" }] }
                ],
                "jump": [{ "control": "key:Space" }],
                "gear": [{ "control": "joy:044f:0422:button:36", "trigger": { "type": "pressed" } }],
                "look": [
                    { "control": "mouse:x" },
                    { "control": "mouse:y", "modifiers": [{ "type": "swizzle", "order": "yxz" }] }
                ]
            } } }
        })";
    }

    TEST_CASE("bindings sum into an action, and hold until released", "[input]")
    {
        InputSystem    input = running(kWalk);
        ActionId const move  = id(input, "move");

        push(input, "key:W", 1.0f);
        push(input, "key:D", 1.0f);
        input.update();
        CHECK(input.axis2(move) == f32vec2{1.0f, 1.0f});
        CHECK(input.active(move));

        // Nothing new: still held.
        input.update();
        CHECK(input.axis2(move) == f32vec2{1.0f, 1.0f});

        push(input, "key:A", 1.0f);
        push(input, "key:W", 0.0f);
        input.update();
        CHECK(input.axis2(move) == f32vec2{0.0f, 0.0f});
        CHECK_FALSE(input.active(move));
    }

    TEST_CASE("a press and release between two frames still fires", "[input]")
    {
        InputSystem    input = running(kWalk);
        ActionId const jump  = id(input, "jump");
        ActionId const gear  = id(input, "gear");

        // An encoder's pulse, and a quick tap: both inside one frame.
        push(input, "joy:044f:0422:button:36", 1.0f, 100);
        push(input, "joy:044f:0422:button:36", 0.0f, 140);
        push(input, "key:Space", 1.0f, 150);
        push(input, "key:Space", 0.0f, 200);
        input.update();
        CHECK(input.fired(gear) == 1);
        CHECK(input.state(gear).fired_ns == 100);
        CHECK(input.fired(jump) == 1);
        CHECK(input.active(jump));

        input.update();
        CHECK(input.fired(gear) == 0);
        CHECK_FALSE(input.active(jump));

        // Three clicks of a wheel in one frame are three.
        for (u64 click = 0; click < 3; ++click)
        {
            push(input, "joy:044f:0422:button:36", 1.0f, click * 10);
            push(input, "joy:044f:0422:button:36", 0.0f, click * 10 + 5);
        }
        input.update();
        CHECK(input.fired(gear) == 3);
    }

    TEST_CASE("mouse motion is summed over the frame", "[input]")
    {
        InputSystem    input = running(kWalk);
        ActionId const look  = id(input, "look");

        push(input, "mouse:x", 3.0f);
        push(input, "mouse:y", -1.0f);
        push(input, "mouse:x", 2.0f);
        input.update();
        CHECK(input.axis2(look) == f32vec2{5.0f, -1.0f});

        input.update();
        CHECK(input.axis2(look) == f32vec2{0.0f, 0.0f});
    }

    TEST_CASE("a higher context consumes its controls", "[input]")
    {
        InputSystem input = running(R"({
            "actions": { "menu_back": {}, "jump": {}, "crouch": {} },
            "contexts": {
                "menu":    { "priority": 50, "bindings": {
                    "menu_back": [{ "control": "key:Space" }],
                    "crouch":    [{ "control": "key:C", "consume": false }]
                } },
                "on_foot": { "priority": 10, "bindings": {
                    "jump":   [{ "control": "key:Space" }],
                    "crouch": [{ "control": "key:C" }]
                } }
            }
        })");
        ActionId const back = id(input, "menu_back");
        ActionId const jump = id(input, "jump");

        push(input, "key:Space", 1.0f);
        input.update();
        CHECK(input.active(back));
        CHECK_FALSE(input.active(jump));

        // With the menu gone, on_foot sees Space, already held: no press.
        input.set_enabled(*input.map().find_context("menu"), false);
        input.update();
        CHECK_FALSE(input.active(back));
        CHECK(input.active(jump));
        CHECK(input.fired(jump) == 0);
    }

    TEST_CASE("a disabled context drives nothing", "[input]")
    {
        InputSystem    input = running(kWalk);
        ActionId const move  = id(input, "move");
        input.set_enabled(*input.map().find_context("on_foot"), false);

        push(input, "key:D", 1.0f);
        input.update();
        CHECK_FALSE(input.active(move));

        // Enabled while D is held, it reads D without an edge.
        input.set_enabled(*input.map().find_context("on_foot"), true);
        input.update();
        CHECK(input.axis2(move) == f32vec2{1.0f, 0.0f});
    }

    TEST_CASE("an axis becomes a button through a threshold", "[input]")
    {
        InputSystem input = running(R"({
            "actions": { "afterburner": {}, "throttle": { "type": "1d" } },
            "contexts": { "flying": { "bindings": {
                "afterburner": [{ "control": "joy:044f:0447:axis:2",
                                  "modifiers": [{ "type": "threshold", "press": 0.9, "release": 0.8 }] }],
                "throttle": [{ "control": "joy:044f:0447:axis:2", "modifiers": [{ "type": "dead_zone", "lower": 0.1 }] }]
            } } }
        })");
        ActionId const burner   = id(input, "afterburner");
        ActionId const throttle = id(input, "throttle");

        push(input, "joy:044f:0447:axis:2", 0.85f);
        input.update();
        CHECK_FALSE(input.active(burner));
        CHECK(input.axis1(throttle) == Catch::Approx((0.85f - 0.1f) / 0.9f));

        push(input, "joy:044f:0447:axis:2", 0.95f);
        push(input, "joy:044f:0447:axis:2", 0.85f);   // inside the gap: still on
        input.update();
        CHECK(input.fired(burner) == 1);
        CHECK(input.active(burner));

        push(input, "joy:044f:0447:axis:2", 0.7f);
        input.update();
        CHECK_FALSE(input.active(burner));
    }

    TEST_CASE("a released trigger fires on the release", "[input]")
    {
        InputSystem input = running(R"({
            "actions": { "drop": {} },
            "contexts": { "c": { "bindings": { "drop": [{ "control": "key:G", "trigger": { "type": "released" } }] } } }
        })");
        ActionId const drop = id(input, "drop");

        push(input, "key:G", 1.0f);
        input.update();
        CHECK_FALSE(input.active(drop));

        push(input, "key:G", 0.0f, 500);
        input.update();
        CHECK(input.fired(drop) == 1);
        CHECK(input.state(drop).fired_ns == 500);
    }

    TEST_CASE("losing focus lets go of buttons and keeps axes", "[input]")
    {
        InputSystem input = running(R"({
            "actions": { "fire": {}, "pitch": { "type": "1d" } },
            "contexts": { "c": { "bindings": {
                "fire":  [{ "control": "joy:044f:0422:button:0" }],
                "pitch": [{ "control": "joy:044f:0422:axis:1" }]
            } } }
        })");
        ActionId const fire  = id(input, "fire");
        ActionId const pitch = id(input, "pitch");

        push(input, "joy:044f:0422:button:0", 1.0f);
        push(input, "joy:044f:0422:axis:1", 0.5f);
        input.update();
        CHECK(input.active(fire));

        input.release_all(1000);
        input.update();
        CHECK_FALSE(input.active(fire));
        CHECK(input.axis1(pitch) == 0.5f);

        input.release_device(DeviceId{.vendor = 0x044f, .product = 0x0422, .instance = 0}, 2000);
        input.update();
        CHECK(input.axis1(pitch) == 0.0f);
    }

    TEST_CASE("an injected action overrides the devices and fires once", "[input]")
    {
        InputSystem    input = running(kWalk);
        ActionId const jump  = id(input, "jump");
        ActionId const move  = id(input, "move");

        input.update();
        input.inject(jump, f32vec3{1.0f, 0.0f, 0.0f});
        input.inject(move, f32vec3{0.0f, 1.0f, 7.0f});
        CHECK(input.fired(jump) == 1);
        CHECK(input.axis3(move) == f32vec3{0.0f, 1.0f, 0.0f});   // a 2D action

        // Held into the next frame: no second press.
        input.update();
        input.inject(jump, f32vec3{1.0f, 0.0f, 0.0f});
        CHECK(input.fired(jump) == 0);
        CHECK(input.active(jump));
    }

    TEST_CASE("the engine's ids are refused by name and by type", "[input]")
    {
        InputSystem const input = running(kWalk);
        CHECK(input.require("move", ActionType::Axis2D).has_value());
        CHECK_FALSE(input.require("move", ActionType::Axis3D).has_value());
        CHECK_FALSE(input.require("teleport", ActionType::Bool).has_value());
        CHECK(input.require_context("on_foot").has_value());
        CHECK_FALSE(input.require_context("flying").has_value());
    }
}
