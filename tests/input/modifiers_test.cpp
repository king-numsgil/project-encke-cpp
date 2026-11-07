#include "core/pch.hpp"

#include "input/modifiers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace encke::input
{
    namespace
    {
        f32vec3 run(Modifier const& modifier, f32vec3 value)
        {
            ModifierState state;
            return apply(modifier, value, state);
        }
    }

    TEST_CASE("a dead zone zeroes the centre and stretches the rest", "[input]")
    {
        DeadZone const zone{.lower = 0.2f, .upper = 1.0f, .radial = false};
        CHECK(run(zone, f32vec3{0.1f, -0.15f, 0.0f}) == f32vec3{0.0f});
        CHECK(run(zone, f32vec3{0.6f, 0.0f, 0.0f}).x == Catch::Approx(0.5f));
        CHECK(run(zone, f32vec3{-1.0f, 0.0f, 0.0f}).x == Catch::Approx(-1.0f));

        // Radial keeps the direction and measures the length.
        DeadZone const radial{.lower = 0.2f, .upper = 1.0f, .radial = true};
        f32vec3 const  out = run(radial, f32vec3{0.36f, 0.48f, 0.0f});   // length 0.6
        CHECK(glm::length(out) == Catch::Approx(0.5f));
        CHECK(out.x / out.y == Catch::Approx(0.75f));
    }

    TEST_CASE("curve, negate, scale, saturation and split shape a value", "[input]")
    {
        CHECK(run(Curve{.exponent = 2.0f}, f32vec3{-0.5f, 0.0f, 0.0f}).x == Catch::Approx(-0.25f));
        CHECK(run(Negate{}, f32vec3{1.0f, -2.0f, 0.0f}) == f32vec3{-1.0f, 2.0f, -0.0f});
        CHECK(run(Scale{.factor = {{2.0f, 1.0f, 0.5f}}}, f32vec3{1.0f}) == f32vec3{2.0f, 1.0f, 0.5f});
        CHECK(run(Saturation{.input = 0.5f, .output = 0.8f}, f32vec3{0.25f, 0.9f, 0.0f}).y == Catch::Approx(0.8f));
        CHECK(run(Saturation{.input = 0.5f, .output = 0.8f}, f32vec3{0.25f, 0.0f, 0.0f}).x == Catch::Approx(0.4f));
        CHECK(run(Split{.half = Half::Positive}, f32vec3{-0.5f, 0.5f, 0.0f}) == f32vec3{0.0f, 0.5f, 0.0f});
        CHECK(run(Split{.half = Half::Negative}, f32vec3{-0.5f, 0.5f, 0.0f}) == f32vec3{0.5f, 0.0f, 0.0f});
    }

    TEST_CASE("a swizzle moves x to the component it names", "[input]")
    {
        CHECK(run(Swizzle{.order = "yxz"}, f32vec3{1.0f, 0.0f, 0.0f}) == f32vec3{0.0f, 1.0f, 0.0f});
        CHECK(run(Swizzle{.order = "zyx"}, f32vec3{1.0f, 0.0f, 0.0f}) == f32vec3{0.0f, 0.0f, 1.0f});
        CHECK_FALSE(check(Swizzle{.order = "yxz"}).has_value());
        CHECK(check(Swizzle{.order = "xxz"}).has_value());
        CHECK(check(Swizzle{.order = "xy"}).has_value());
    }

    TEST_CASE("a threshold holds between press and release", "[input]")
    {
        Threshold const threshold{.press = 0.6f, .release = 0.4f};
        ModifierState   state;
        auto const      at = [&](f32 x) { return apply(threshold, f32vec3{x, 0.0f, 0.0f}, state).x; };

        CHECK(at(0.5f) == 0.0f);
        CHECK(at(0.6f) == 1.0f);
        CHECK(at(0.5f) == 1.0f);   // above release, still on
        CHECK(at(0.39f) == 0.0f);
        CHECK(at(-0.7f) == 1.0f);  // either way along the axis

        CHECK(check(Threshold{.press = 0.4f, .release = 0.6f}).has_value());
    }

    TEST_CASE("a chain applies in order", "[input]")
    {
        array<Modifier, 2> const      chain{{Negate{}, Swizzle{.order = "yxz"}}};
        array<ModifierState, 2>       states{};
        CHECK(apply(chain, f32vec3{1.0f, 0.0f, 0.0f}, states) == f32vec3{-0.0f, -1.0f, -0.0f});
    }
}
