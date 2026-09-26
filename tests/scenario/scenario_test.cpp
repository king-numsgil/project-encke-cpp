#include "core/pch.hpp"

#include "scenario/scenario.hpp"

#include <catch2/catch_test_macros.hpp>

namespace encke::scenario
{
    TEST_CASE("a scenario reads settings and every kind of step", "[scenario]")
    {
        string const text = R"({
            "name": "tour",
            "settings": { "ui": false, "tonemap": "agx", "ev100": 13.5, "windows": ["normals"] },
            "steps": [
                // Comments are allowed: the files are written by hand.
                { "op": "settle" },
                { "op": "settle", "min_frames": 3 },
                { "op": "capture", "file": "pole.png" },
                { "op": "freeze_terrain" },
                { "op": "camera", "position": [1, 2, 3], "target": [4, 5, 6] },
                { "op": "fly", "velocity": [0, 0, -200], "frames": 120 },
                { "op": "wait", "frames": 4 },
                { "op": "set", "taa": false, "shading": "brute_force" },
                { "op": "thaw_terrain" },
                { "op": "interactive" }
            ]
        })";

        optional<Scenario> const scenario = parse(text, "test");
        REQUIRE(scenario.has_value());

        CHECK(scenario->name == "tour");
        CHECK(scenario->settings.ui == false);
        CHECK(scenario->settings.tonemap == Tonemap::AgX);
        CHECK(scenario->settings.ev100 == 13.5f);
        CHECK(scenario->settings.windows == vector<DebugWindow>{DebugWindow::Normals});
        CHECK_FALSE(scenario->settings.taa.has_value());

        REQUIRE(scenario->steps.size() == 10);
        CHECK(std::get<Settle>(scenario->steps[0]).min_frames == Settle{}.min_frames);
        CHECK(std::get<Settle>(scenario->steps[1]).min_frames == 3);
        CHECK(std::get<Capture>(scenario->steps[2]).file == "pole.png");
        CHECK(std::holds_alternative<FreezeTerrain>(scenario->steps[3]));

        Camera const& camera = std::get<Camera>(scenario->steps[4]);
        CHECK(camera.position == array<f64, 3>{{1.0, 2.0, 3.0}});
        CHECK(camera.up == array<f64, 3>{{0.0, 1.0, 0.0}});

        CHECK(std::get<Fly>(scenario->steps[5]).frames == 120);
        CHECK(std::get<Wait>(scenario->steps[6]).frames == 4);

        Settings const& set = std::get<Settings>(scenario->steps[7]);
        CHECK(set.taa == false);
        CHECK(set.shading == DebugView::BruteForce);

        CHECK(op_name(scenario->steps[8]) == "thaw_terrain");
        CHECK(std::holds_alternative<Interactive>(scenario->steps[9]));
    }

    TEST_CASE("a scenario with a mistake in it is refused", "[scenario]")
    {
        // An unknown op, a misspelt field, a bad enum name, a path out of the
        // output folder, and a startup setting in a set step.
        CHECK_FALSE(parse(R"({ "steps": [{ "op": "teleport" }] })", "test"));
        CHECK_FALSE(parse(R"({ "steps": [{ "op": "wait", "frame": 3 }] })", "test"));
        CHECK_FALSE(parse(R"({ "settings": { "tonemap": "filmic" } })", "test"));
        CHECK_FALSE(parse(R"({ "steps": [{ "op": "capture", "file": "/tmp/x.png" }] })", "test"));
        CHECK_FALSE(parse(R"({ "steps": [{ "op": "set", "srgb_ui": true }] })", "test"));
    }

    TEST_CASE("a step written out reads back as the same step", "[scenario]")
    {
        Step const step = Camera{.position = {{-2.55, 1.33, -1.62}}, .target = {{-3.0, 1.17, -2.0}}};
        string const json = to_json(step);
        CHECK(json.starts_with(R"({"op":"camera")"));

        optional<Scenario> const scenario = parse(R"({ "steps": [)" + json + "] }", "test");
        REQUIRE(scenario.has_value());
        REQUIRE(scenario->steps.size() == 1);
        CHECK(std::get<Camera>(scenario->steps[0]).position == std::get<Camera>(step).position);
    }
}
