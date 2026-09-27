#include "core/pch.hpp"

#include "render/config.hpp"
#include "render/walk_camera.hpp"
#include "world/transform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace encke
{
    namespace
    {
        constexpr f64 kClose = 1e-9;

        f64vec3 forward_of(Transform const& camera)
        {
            return camera.rotation * f64vec3{0.0, 0.0, -1.0};
        }
    }

    TEST_CASE("the walk camera takes over the camera's facing, levelled, at the eye", "[camera]")
    {
        f64vec3 const up = glm::normalize(f64vec3{0.12, 0.99, 0.08});

        // Facing along the ground, a little down, and rolled.
        Transform camera{};
        f64vec3 const along = glm::normalize(glm::cross(up, f64vec3{1.0, 0.0, 0.0}));
        camera.rotation = look_rotation(along - up * 0.2, glm::normalize(up + along * 0.3));

        WalkCamera walk;
        walk.begin(camera, up);

        Transform       placed{};
        f64vec3 const   feet{1.0, 2.0, 3.0};
        walk.place(placed, feet, up);

        CHECK(glm::length(placed.position - (feet + up * config::kEyeHeight)) < kClose);
        CHECK(glm::length(forward_of(placed) - forward_of(camera)) < 1e-9);

        // No roll: the camera's right lies along the ground.
        f64vec3 const right = placed.rotation * f64vec3{1.0, 0.0, 0.0};
        CHECK(std::abs(glm::dot(right, up)) < kClose);
    }

    TEST_CASE("the walk camera asks for velocity along the ground, and keeps level as up turns", "[camera]")
    {
        f64vec3 const up{0.0, 1.0, 0.0};
        Transform     camera{};
        camera.rotation = look_rotation(f64vec3{0.0, -0.5, -1.0}, up);

        WalkCamera walk;
        walk.begin(camera, up);

        // Forward and right together: along the ground, at walking speed,
        // not faster on the diagonal.
        f64vec3 const velocity = walk.steer(WalkInput{.move = f64vec2{1.0, 1.0}}, up);
        CHECK(std::abs(glm::dot(velocity, up)) < kClose);
        CHECK(std::abs(glm::length(velocity) - config::kWalkSpeed) < 1e-9);

        f64vec3 const sprint = walk.steer(WalkInput{.move = f64vec2{0.0, 1.0}, .sprint = true}, up);
        CHECK(std::abs(glm::length(sprint) - config::kSprintSpeed) < 1e-9);
        CHECK(glm::dot(sprint, f64vec3{0.0, 0.0, -1.0}) > 0.0);

        // Walked a way round the planet: up has turned, and forward still
        // runs along the new ground.
        f64vec3 const turned = glm::normalize(f64vec3{0.0, 1.0, 0.3});
        f64vec3 const later  = walk.steer(WalkInput{.move = f64vec2{0.0, 1.0}}, turned);
        CHECK(std::abs(glm::dot(later, turned)) < 1e-9);
        CHECK(std::abs(glm::length(later) - config::kWalkSpeed) < 1e-9);

        // The mouse turns the heading about up and tilts the view, which
        // stops short of straight down.
        walk.steer(WalkInput{.look = f32vec2{0.0f, 1.0e5f}}, turned);
        Transform placed{};
        walk.place(placed, f64vec3{0.0}, turned);
        CHECK(glm::dot(forward_of(placed), turned) < -0.99);
        CHECK(glm::dot(forward_of(placed), turned) > -1.0 + 1e-6);
    }
}
