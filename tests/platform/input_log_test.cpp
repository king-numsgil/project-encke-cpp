#include "core/pch.hpp"

#include "platform/input_log.hpp"

#include <catch2/catch_test_macros.hpp>

namespace encke::input_log
{
    // The log is read by people with jq and, later, replayed by tests: its
    // spelling is a format, pinned here.
    TEST_CASE("input log records are one tagged JSON object each", "[input]")
    {
        CHECK(to_json(Button{.ns = 1500, .device = 1, .button = 12, .down = true}) ==
              R"({"type":"button","ns":1500,"device":1,"button":12,"down":true})");
        CHECK(to_json(Axis{.ns = 7, .device = 2, .axis = 3, .value = -32768}) ==
              R"({"type":"axis","ns":7,"device":2,"axis":3,"value":-32768})");
        CHECK(to_json(Hat{.ns = 9, .device = 1, .hat = 0, .value = "up-right"}) ==
              R"({"type":"hat","ns":9,"device":1,"hat":0,"value":"up-right"})");
        CHECK(to_json(Removed{.ns = 11, .device = 2}) == R"({"type":"removed","ns":11,"device":2})");
    }

    TEST_CASE("a device record escapes its Windows path", "[input]")
    {
        Device device;
        device.path = R"(\\?\HID#VID_044F&PID_0422)";

        string const json = to_json(device);
        CHECK(json.starts_with(R"({"type":"device",)"));
        CHECK(json.find(R"("path":"\\\\?\\HID#VID_044F&PID_0422")") != string::npos);
    }
}
