#include "core/pch.hpp"

#include <glaze/glaze.hpp>

#include <catch2/catch_test_macros.hpp>

// glaze reads and writes aggregates by compile-time reflection, with no
// per-field glue. These tests prove it builds here and pin the behaviour a
// settings file relies on: nested structs round-trip, hand-written files may
// carry comments, and a misspelt key is an error rather than silently ignored.
namespace encke
{
    // Named, not anonymous: glaze reads field names off a declaration of an
    // extern variable of the type, which a type without linkage cannot have.
    namespace json_test
    {
        struct CaptureSettings
        {
            string        path;
            int           frame = 10;
            optional<f64> ev100;
        };

        struct TestSettings
        {
            bool            ui  = true;
            bool            taa = true;
            vector<f64>     camera;
            CaptureSettings capture;
        };

        constexpr glz::opts kHandWritten{.comments = true};
    }

    using namespace json_test;

    TEST_CASE("glaze round-trips a nested settings struct", "[core][json]")
    {
        TestSettings const written{
            .ui      = false,
            .taa     = true,
            .camera  = {-2.55, 1.33, -1.62, -3.0, 1.17, -2.0},
            .capture = {.path = "helmet.png", .frame = 12, .ev100 = 13.5},
        };

        auto const json = glz::write<glz::opts{.prettify = true}>(written);
        REQUIRE(json.has_value());

        TestSettings read;
        REQUIRE_FALSE(glz::read_json(read, *json));

        CHECK(read.ui == written.ui);
        CHECK(read.taa == written.taa);
        CHECK(read.camera == written.camera);
        CHECK(read.capture.path == written.capture.path);
        CHECK(read.capture.frame == written.capture.frame);
        CHECK(read.capture.ev100 == written.capture.ev100);
    }

    TEST_CASE("glaze reads a hand-written file with comments and missing keys", "[core][json]")
    {
        string const text = R"({
            // The helmet, from the table.
            "camera": [-2.55, 1.33, -1.62, -3, 1.17, -2],
            "capture": { "path": "helmet.png" } /* frame stays 10 */
        })";

        TestSettings settings;
        REQUIRE_FALSE(glz::read<kHandWritten>(settings, text));

        CHECK(settings.ui);
        CHECK(settings.camera.size() == 6);
        CHECK(settings.capture.path == "helmet.png");
        CHECK(settings.capture.frame == 10);
        CHECK_FALSE(settings.capture.ev100.has_value());
    }

    TEST_CASE("glaze rejects an unknown key", "[core][json]")
    {
        TestSettings settings;
        CHECK(glz::read<kHandWritten>(settings, string{R"({ "tea": false })"}));
        CHECK(glz::read<kHandWritten>(settings, string{R"({ "capture": { "frames": 3 } })"}));
    }
}
