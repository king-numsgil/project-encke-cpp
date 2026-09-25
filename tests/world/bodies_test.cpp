#include "core/pch.hpp"

#include "world/bodies.hpp"
#include "world/transform.hpp"

#include <catch2/catch_test_macros.hpp>

namespace encke
{
    namespace
    {
        entt::entity add_body(entt::registry& registry, f64vec3 const& position, f64 radius, optional<f64> air)
        {
            entt::entity const body = registry.create();
            registry.emplace<Transform>(body, Transform{.position = position});
            registry.emplace<Body>(body, Body{.radius = radius});
            if (air.has_value())
            {
                registry.emplace<Atmosphere>(body, Atmosphere{.height = *air});
            }
            return body;
        }
    }

    TEST_CASE("the atmosphere a point sees is the one whose top is nearest", "[bodies]")
    {
        entt::registry registry;
        CHECK(!atmosphere_at(registry, f64vec3{0.0}).has_value());

        // A large planet with deep air, a small one with thin air a little
        // way off, and an airless moon between them.
        add_body(registry, f64vec3{0.0}, 6'000'000.0, 100'000.0);
        add_body(registry, f64vec3{20'000'000.0, 0.0, 0.0}, 1'000'000.0, 10'000.0);
        add_body(registry, f64vec3{12'000'000.0, 0.0, 0.0}, 500'000.0, nullopt);
        propagate_transforms(registry);

        // Inside the large planet's air.
        optional<AtmosphereView> const low = atmosphere_at(registry, f64vec3{0.0, 6'050'000.0, 0.0});
        REQUIRE(low.has_value());
        CHECK(low->radius == 6'000'000.0);

        // Just above the small planet's air: its top is nearer than the
        // large one's, though the large planet is bigger.
        optional<AtmosphereView> const near_small = atmosphere_at(registry, f64vec3{18'900'000.0, 0.0, 0.0});
        REQUIRE(near_small.has_value());
        CHECK(near_small->radius == 1'000'000.0);
        CHECK(near_small->centre == f64vec3{20'000'000.0, 0.0, 0.0});

        // Standing on the airless moon: the nearest air is still drawn.
        optional<AtmosphereView> const moon = atmosphere_at(registry, f64vec3{12'000'000.0, 500'000.0, 0.0});
        REQUIRE(moon.has_value());
        CHECK(moon->radius == 6'000'000.0);
    }
}
