#include "core/pch.hpp"

#include "input/capture.hpp"

#include <catch2/catch_test_macros.hpp>

namespace encke::input
{
    namespace
    {
        ControlEvent event(string_view control, f32 value)
        {
            optional<Control> const parsed = parse_control(control);
            REQUIRE(parsed.has_value());
            return ControlEvent{.control = *parsed, .value = value, .ns = 0};
        }
    }

    TEST_CASE("a capture takes the first press", "[input]")
    {
        Capture capture;
        CHECK_FALSE(capture.feed(array<ControlEvent, 1>{{event("key:W", 1.0f)}}).has_value());

        capture.start();
        array<ControlEvent, 4> const events{{
            event("mouse:x", 30.0f),       // the mouse never binds
            event("mouse:left", 1.0f),
            event("key:W", 0.0f),          // a release is not a press
            event("joy:044f:0422:button:36", 1.0f),
        }};
        optional<Control> const taken = capture.feed(events);
        REQUIRE(taken.has_value());
        CHECK(format_control(*taken) == "joy:044f:0422:button:36");
        CHECK_FALSE(capture.active());
    }

    TEST_CASE("a capture takes an axis once it has moved far enough", "[input]")
    {
        Capture capture;
        capture.start();

        // A throttle at rest, noisy: never far from where it began.
        array<ControlEvent, 3> const noise{{
            event("joy:044f:0447:axis:2", -1.0f),
            event("joy:044f:0447:axis:2", -0.98f),
            event("joy:044f:0447:axis:2", -0.99f),
        }};
        CHECK_FALSE(capture.feed(noise).has_value());
        CHECK(capture.active());

        array<ControlEvent, 1> const pushed{{event("joy:044f:0447:axis:2", -0.4f)}};
        optional<Control> const taken = capture.feed(pushed);
        REQUIRE(taken.has_value());
        CHECK(format_control(*taken) == "joy:044f:0447:axis:2");
    }

    TEST_CASE("escape cancels a capture", "[input]")
    {
        Capture capture;
        capture.start();
        CHECK_FALSE(capture.feed(array<ControlEvent, 1>{{event("key:Escape", 1.0f)}}).has_value());
        CHECK_FALSE(capture.active());
    }
}
