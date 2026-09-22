#include "core/pch.hpp"

#include "render/fly_camera.hpp"

#include "render/camera.hpp"

#include <algorithm>
#include <cmath>

namespace encke
{
    namespace
    {
        constexpr f64 kPi = 3.14159265358979323846;

        constexpr f64 kLookRadiansPerPixel = 0.0025;
        constexpr f64 kPitchLimit          = 0.5 * kPi - 0.01;

        // Each wheel notch scales the speed by this much. The range runs from
        // a walk to crossing a planet in seconds; Ctrl still slows the walk
        // for fine placement.
        constexpr f64 kWheelStep = 1.25;
        constexpr f64 kMinSpeed  = 2.0;
        constexpr f64 kMaxSpeed  = 1.0e7;

        constexpr f64 kFastFactor = 5.0;
        constexpr f64 kSlowFactor = 0.2;

        f64quat orientation_of(f64 yaw, f64 pitch)
        {
            return glm::angleAxis(yaw, f64vec3{0.0, 1.0, 0.0}) *
                   glm::angleAxis(pitch, f64vec3{1.0, 0.0, 0.0});
        }
    }

    void FlyCamera::reset(Camera const& camera)
    {
        // Inverse of orientation_of: its forward is
        // (-sin yaw cos pitch, sin pitch, -cos yaw cos pitch).
        f64vec3 const forward = camera.orientation * f64vec3{0.0, 0.0, -1.0};
        yaw_   = std::atan2(-forward.x, -forward.z);
        pitch_ = std::asin(std::clamp(forward.y, -1.0, 1.0));
    }

    void FlyCamera::update(Camera& camera, FlyInput const& input, f64 seconds)
    {
        // Mouse right turns right, which is negative yaw about +Y; mouse down
        // looks down.
        yaw_   -= static_cast<f64>(input.look.x) * kLookRadiansPerPixel;
        pitch_ -= static_cast<f64>(input.look.y) * kLookRadiansPerPixel;
        pitch_  = std::clamp(pitch_, -kPitchLimit, kPitchLimit);
        yaw_    = std::remainder(yaw_, 2.0 * kPi);

        speed_ = std::clamp(speed_ * std::pow(kWheelStep, static_cast<f64>(input.wheel)),
                            kMinSpeed, kMaxSpeed);

        camera.orientation = orientation_of(yaw_, pitch_);

        f64vec3 const right   = camera.orientation * f64vec3{1.0, 0.0, 0.0};
        f64vec3 const forward = camera.orientation * f64vec3{0.0, 0.0, -1.0};
        f64vec3 const up{0.0, 1.0, 0.0};

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
