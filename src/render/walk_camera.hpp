#pragma once

namespace encke
{
    struct Transform;

    // How the camera gets about: on foot as a character on the ground, or
    // flying free (FlyCamera), which X toggles.
    enum class Movement : u32
    {
        Walk    = 0,
        Jetpack = 1,
    };

    // One frame of walking input, already gathered from the devices.
    struct WalkInput
    {
        f32vec2 look{0.0f};   // mouse pixels, +x right, +y down
        f64vec2 move{0.0};    // x right, y forward; each -1, 0 or 1
        bool    sprint = false;
        bool    jump   = false;
    };

    // First-person look for a walking character. The character's up is
    // the ground's, away from its body's centre, so it turns as the
    // character walks around a planet; the heading is carried along by the
    // least turn that keeps it level. Pitch is about the heading's right
    // and stops short of straight up and down. There is no roll.
    //
    // It asks the character for a velocity along the ground and puts the
    // camera at the eye above wherever the character's feet end up;
    // physics::PhysicsWorld does the walking in between.
    class WalkCamera
    {
    public:
        // Takes over from the camera's pose: facing where it faced, levelled
        // against `up`, its pitch kept.
        void begin(Transform const& camera, f64vec3 const& up);

        // Turns by the mouse and returns the velocity asked of the character
        // along the ground, world space, m/s.
        f64vec3 steer(WalkInput const& input, f64vec3 const& up);

        // The camera at config::kEyeHeight above `feet`, looking where the
        // character looks. The camera must be a root.
        void place(Transform& camera, f64vec3 const& feet, f64vec3 const& up);

    private:
        // Carries the heading onto `up` by the least turn.
        void level(f64vec3 const& up);

        // +Y the character's up, -Z its facing along the ground.
        f64quat heading_{1.0, 0.0, 0.0, 0.0};
        f64     pitch_ = 0.0;
    };
}
