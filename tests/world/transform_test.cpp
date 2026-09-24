#include "core/pch.hpp"

#include "world/transform.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/gtc/quaternion.hpp>

#include <numbers>

using Catch::Matchers::WithinAbs;

namespace encke
{
    namespace
    {
        constexpr f64 kTolerance = 1e-12;

        void check_near(f64vec3 const& actual, f64vec3 const& expected)
        {
            CHECK_THAT(actual.x, WithinAbs(expected.x, kTolerance));
            CHECK_THAT(actual.y, WithinAbs(expected.y, kTolerance));
            CHECK_THAT(actual.z, WithinAbs(expected.z, kTolerance));
        }

        f64quat about_y(f64 radians)
        {
            return glm::angleAxis(radians, f64vec3{0.0, 1.0, 0.0});
        }
    }

    TEST_CASE("a child is offset in its parent's rotated frame", "[transform]")
    {
        entt::registry registry;

        entt::entity const parent = registry.create();
        entt::entity const child  = registry.create();

        registry.emplace<Transform>(parent, Transform{
            .position = {10.0, 0.0, 0.0},
            .rotation = about_y(std::numbers::pi / 2.0),
            .scale    = f32vec3{2.0f},
        });
        registry.emplace<Transform>(child, Transform{
            .position = {0.0, 0.0, -1.0},
            .parent   = parent,
        });

        propagate_transforms(registry);

        // A quarter turn about +Y takes -Z to -X.
        WorldTransform const& world = registry.get<WorldTransform>(child);
        check_near(world.position, {9.0, 0.0, 0.0});
        check_near(forward(world.rotation), {-1.0, 0.0, 0.0});

        // Scale is not inherited, and does not stretch the offset.
        CHECK(world.scale == f32vec3{1.0f});
    }

    TEST_CASE("children created before their parents compose correctly", "[transform]")
    {
        entt::registry registry;

        // Storage order is creation order, so the leaf is met first.
        entt::entity const leaf   = registry.create();
        entt::entity const middle = registry.create();
        entt::entity const root   = registry.create();

        registry.emplace<Transform>(leaf, Transform{.position = {0.0, 0.0, 1.0}, .parent = middle});
        registry.emplace<Transform>(middle, Transform{.position = {0.0, 1.0, 0.0}, .parent = root});
        registry.emplace<Transform>(root, Transform{.position = {1.0, 0.0, 0.0}});

        propagate_transforms(registry);
        check_near(registry.get<WorldTransform>(leaf).position, {1.0, 1.0, 1.0});

        // A second pass re-reads the locals rather than keeping last pass's.
        registry.get<Transform>(root).position = {5.0, 0.0, 0.0};
        propagate_transforms(registry);
        check_near(registry.get<WorldTransform>(leaf).position, {5.0, 1.0, 1.0});
    }

    TEST_CASE("a child whose parent is gone is placed as a root", "[transform]")
    {
        entt::registry registry;

        entt::entity const parent = registry.create();
        entt::entity const child  = registry.create();

        registry.emplace<Transform>(parent, Transform{.position = {100.0, 0.0, 0.0}});
        registry.emplace<Transform>(child, Transform{.position = {0.0, 2.0, 0.0}, .parent = parent});

        registry.destroy(parent);
        propagate_transforms(registry);

        check_near(registry.get<WorldTransform>(child).position, {0.0, 2.0, 0.0});
    }

    TEST_CASE("look_rotation faces -Z toward its target", "[transform]")
    {
        f64vec3 const up{0.0, 1.0, 0.0};

        SECTION("with an ordinary up")
        {
            f64vec3 const toward = glm::normalize(f64vec3{3.0, -1.0, 2.0});
            f64quat const q      = look_rotation(toward, up);

            check_near(forward(q), toward);

            // Its +Y stays in the plane of forward and up: no roll.
            f64vec3 const top = q * f64vec3{0.0, 1.0, 0.0};
            CHECK_THAT(glm::dot(top, glm::cross(toward, up)), WithinAbs(0.0, kTolerance));
            CHECK(glm::dot(top, up) > 0.0);
        }

        SECTION("straight along up, where up says nothing")
        {
            check_near(forward(look_rotation(up, up)), up);
            check_near(forward(look_rotation(-up, up)), -up);
        }
    }
}
