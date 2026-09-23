#pragma once

namespace encke
{
    struct Transform;

    // One frame of fly-camera input, already gathered from the devices.
    struct FlyInput
    {
        f32vec2 look{0.0f};    // mouse pixels, +x right, +y down
        f64vec3 move{0.0};     // x right, y up, z forward; each -1, 0 or 1
        f64     roll  = 0.0;   // +1 rolls left (counter-clockwise), -1 right
        f32     wheel = 0.0f;  // notches: scales the speed
        bool    fast  = false;
        bool    slow  = false;
    };

    // Six-degrees-of-freedom flight. Every axis is the camera's own: the
    // mouse yaws about the camera's up and pitches about its right, roll
    // turns about the view axis, and strafing up moves along the camera's
    // up. Nothing is levelled against a planet or the world, so it flies
    // the same between bodies as on one.
    //
    // It flies a camera entity's local Transform, so a camera parented to a
    // ship flies relative to the ship: its axes are still the camera's own,
    // and the ship carries it.
    class FlyCamera
    {
    public:
        // Points the camera along `forward` with its up as close to `up` as
        // that direction allows, both in the transform's parent frame.
        // Neither needs to be unit length.
        void aim(Transform& camera, f64vec3 const& forward, f64vec3 const& up) const;

        // Turns, then moves `seconds` worth of travel. Position is f64, so
        // speed and distance can reach system scale without drift.
        void update(Transform& camera, FlyInput const& input, f64 seconds);

        f64 speed() const { return speed_; }

    private:
        f64 speed_ = 6.0;   // m/s, before the fast and slow modifiers
    };
}
