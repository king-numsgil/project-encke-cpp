#include "core/pch.hpp"

#include "render/fly_camera.hpp"

#include "render/camera.hpp"

#include <algorithm>
#include <cmath>

namespace encke
{
    namespace
    {
        constexpr f64 kLookRadiansPerPixel = 0.0025;
        constexpr f64 kRollRadiansPerSecond = 1.5;

        // Each wheel notch scales the speed by this much. The range runs from
        // a walk to crossing a planet in seconds; Alt still slows the walk
        // for fine placement.
        constexpr f64 kWheelStep = 1.25;
        constexpr f64 kMinSpeed  = 2.0;
        constexpr f64 kMaxSpeed  = 1.0e7;

        constexpr f64 kFastFactor = 5.0;
        constexpr f64 kSlowFactor = 0.2;

        // Below this, the old up is too close to the new forward to say
        // which way up the camera should be.
        constexpr f64 kDegenerate = 1.0e-6;
    }

    void FlyCamera::aim(Camera& camera, f64vec3 const& forward, f64vec3 const& up) const
    {
        // Camera space is x right, y up, -z forward.
        f64vec3 const back = -glm::normalize(forward);

        f64vec3 right = glm::cross(up, back);
        if (glm::length(right) < kDegenerate * glm::length(up))
        {
            // Aimed along `up`: the camera's old forward serves as up.
            right = glm::cross(camera.orientation * f64vec3{0.0, 0.0, -1.0}, back);
        }
        right = glm::normalize(right);

        f64vec3 const top = glm::cross(back, right);
        camera.orientation = glm::normalize(glm::quat_cast(f64mat3{right, top, back}));
    }

    void FlyCamera::update(Camera& camera, FlyInput const& input, f64 seconds)
    {
        // Composed on the right, so each turn is about the camera's own axis.
        // Mouse right turns right, which is negative rotation about the
        // camera's up; mouse down looks down.
        f64 const yaw   = -static_cast<f64>(input.look.x) * kLookRadiansPerPixel;
        f64 const pitch = -static_cast<f64>(input.look.y) * kLookRadiansPerPixel;
        f64 const roll  = input.roll * kRollRadiansPerSecond * seconds;

        camera.orientation = glm::normalize(camera.orientation *
                                            glm::angleAxis(yaw, f64vec3{0.0, 1.0, 0.0}) *
                                            glm::angleAxis(pitch, f64vec3{1.0, 0.0, 0.0}) *
                                            glm::angleAxis(roll, f64vec3{0.0, 0.0, 1.0}));

        speed_ = std::clamp(speed_ * std::pow(kWheelStep, static_cast<f64>(input.wheel)),
                            kMinSpeed, kMaxSpeed);

        f64vec3 const right   = camera.orientation * f64vec3{1.0, 0.0, 0.0};
        f64vec3 const up      = camera.orientation * f64vec3{0.0, 1.0, 0.0};
        f64vec3 const forward = camera.orientation * f64vec3{0.0, 0.0, -1.0};

        f64vec3 direction = right * input.move.x + up * input.move.y + forward * input.move.z;
        f64 const length  = glm::length(direction);
        if (length <= 0.0)
        {
            return;
        }

        // Diagonals are no faster than straight lines.
        direction /= std::max(length, 1.0);

        f64 speed = speed_;
        if (input.fast)
        {
            speed *= kFastFactor;
        }
        if (input.slow)
        {
            speed *= kSlowFactor;
        }

        camera.position += direction * (speed * seconds);
    }
}
