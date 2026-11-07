#include "core/pch.hpp"

#include "input/control.hpp"

#include <catch2/catch_test_macros.hpp>

namespace encke::input
{
    TEST_CASE("controls read from text and write back the same", "[input]")
    {
        for (string_view const text : {"key:W", "key:Left Shift", "key:F1", "key:Escape", "mouse:x", "mouse:y",
                                       "mouse:wheel", "mouse:left", "mouse:x2", "joy:044f:0422:button:36",
                                       "joy:044f:0447:axis:2", "joy:044f:0422#1:hat:0:down"})
        {
            optional<Control> const control = parse_control(text);
            INFO(text);
            REQUIRE(control.has_value());
            CHECK(format_control(*control) == text);
        }
    }

    TEST_CASE("a control's text names its device and index", "[input]")
    {
        optional<Control> const button = parse_control("joy:044f:0422#2:button:19");
        REQUIRE(button.has_value());
        CHECK(button->kind == ControlKind::JoyButton);
        CHECK(button->index == 19);
        CHECK(button->device == DeviceId{.vendor = 0x044f, .product = 0x0422, .instance = 2});
        CHECK(button->joystick());
        CHECK_FALSE(button->relative());

        optional<Control> const hat = parse_control("joy:044f:0447:hat:0:left");
        REQUIRE(hat.has_value());
        CHECK(hat->direction == HatDirection::Left);

        CHECK(parse_control("mouse:wheel")->relative());
        CHECK(parse_control("key:Space")->index == SDL_SCANCODE_SPACE);
    }

    TEST_CASE("text that names no control is refused", "[input]")
    {
        for (string_view const text : {"", "key:", "key:NotAKey", "mouse:z", "pad:a", "joy:044f:button:1",
                                       "joy:xyz:0422:button:1", "joy:044f:0422:button:", "joy:044f:0422:axis:1:up",
                                       "joy:044f:0422:hat:0:sideways", "joy:044f:0422:slider:0"})
        {
            INFO(text);
            CHECK_FALSE(parse_control(text).has_value());
        }
    }
}
