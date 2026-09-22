#pragma once

namespace encke
{
    struct Camera;

    // One frame of fly-camera input, already gathered from the devices.
    struct FlyInput
    {
        f32vec2 look{0.0f};    // mouse pixels, +x right, +y down
        f64vec3 move{0.0};     // x right, y up, z forward; each -1, 0 or 1
        f32     wheel = 0.0f;  // notches: scales the speed
        bool    fast  = false;
        bool    slow  = false;
    };

    // First-person free flight: yaw about world +Y, pitch clamped short of
    // straight up or down, no roll. Moves along where it looks, with world
    // up and down on their own axis.
    //
    // World +Y as "up" suits the test planet, where +Y is the pole's normal.
    // Flying a ship, or standing anywhere else on a planet, wants the local
    // up instead; that is the parameter to add when it matters.
    class FlyCamera
    {
    public:
        // Takes yaw and pitch from the camera's current orientation, so
        // control starts from wherever the camera was put.
        void reset(Camera const& camera);

        // Turns, then moves `seconds` worth of travel. Position is f64, so
        // speed and distance can reach system scale without drift.
        void update(Camera& camera, FlyInput const& input, f64 seconds);

        f64 speed() const { return speed_; }

    private:
        f64 yaw_   = 0.0;
        f64 pitch_ = 0.0;
        f64 speed_ = 6.0;   // m/s, before the fast and slow modifiers
    };
}
