#include "core/pch.hpp"

#include "render/walk_camera.hpp"

#include "render/config.hpp"
#include "world/transform.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtx/quaternion.hpp>

namespace encke
{
    namespace
    {
        // As the fly camera's, so switching does not change the feel.
        constexpr f64 kLookRadiansPerPixel = 0.0025;

        // Pitch stops this short of straight up or down, where the heading
        // would lose its meaning.
        constexpr f64 kPitchLimit = 1.55;

        // Below this, the camera faces too nearly along `up` to say which
        // way along the ground it faces.
        constexpr f64 kDegenerate = 1.0e-6;
    }

    void WalkCamera::begin(Transform const& camera, f64vec3 const& up)
    {
        f64vec3 const forward = camera.rotation * f64vec3{0.0, 0.0, -1.0};
        f64vec3       along   = forward - up * glm::dot(forward, up);
        if (glm::length(along) < kDegenerate)
        {
            // Looking straight up or down: the camera's up points the way
            // its top leans, which is the way it would face levelled.
            f64vec3 const top = camera.rotation * f64vec3{0.0, 1.0, 0.0};
            along             = (glm::dot(forward, up) > 0.0 ? -top : top) - up * glm::dot(top, up);
        }

        heading_ = look_rotation(along, up);
        pitch_   = std::clamp(std::asin(std::clamp(glm::dot(forward, up), -1.0, 1.0)), -kPitchLimit, kPitchLimit);
    }

    f64vec3 WalkCamera::steer(WalkInput const& input, f64vec3 const& up)
    {
        level(up);

        // Mouse right turns right, a negative turn about up; mouse down
        // looks down.
        f64 const yaw = -static_cast<f64>(input.look.x) * kLookRadiansPerPixel;
        pitch_        = std::clamp(pitch_ - static_cast<f64>(input.look.y) * kLookRadiansPerPixel, -kPitchLimit,
                                   kPitchLimit);
        heading_      = glm::normalize(heading_ * glm::angleAxis(yaw, f64vec3{0.0, 1.0, 0.0}));

        f64vec3 direction = heading_ * f64vec3{input.move.x, 0.0, -input.move.y};
        f64 const length  = glm::length(direction);
        if (length <= 0.0)
        {
            return f64vec3{0.0};
        }

        // Diagonals are no faster than straight lines.
        direction /= std::max(length, 1.0);
        return direction * (input.sprint ? config::kSprintSpeed : config::kWalkSpeed);
    }

    void WalkCamera::place(Transform& camera, f64vec3 const& feet, f64vec3 const& up)
    {
        level(up);
        camera.position = feet + up * config::kEyeHeight;
        camera.rotation = glm::normalize(heading_ * glm::angleAxis(pitch_, f64vec3{1.0, 0.0, 0.0}));
    }

    void WalkCamera::level(f64vec3 const& up)
    {
        f64vec3 const current = heading_ * f64vec3{0.0, 1.0, 0.0};
        heading_              = glm::normalize(glm::rotation(current, up) * heading_);
    }
}
