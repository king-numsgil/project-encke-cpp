#pragma once

namespace encke
{
    // World positions are f64; the GPU only ever sees VIEW space.
    //
    // A space sim spans cockpit instruments centimetres from the eye out to
    // bodies millions of metres away, and an f32 world position is only good to
    // ~6 cm at 1,000 km -- enough to make a ship interior visibly jitter. So
    // nothing large reaches the GPU: model and view are composed in f64, which
    // cancels the large translations against each other, and only the small
    // view-space result is narrowed to f32.
    //
    // The rule, stated once: **every world -> view transform happens in f64 on
    // the CPU.** A shader that sees a world-space position is a bug.
    struct Camera
    {
        f64vec3 position{0.0};

        // View -> world rotation. Constructor order is (w, x, y, z).
        f64quat orientation{1.0, 0.0, 0.0, 0.0};

        f64 vertical_fov = 1.2217304763960306;   // 70 degrees

        // 5 cm: close enough for instruments in a cockpit. With reversed-Z and
        // an infinite far plane, pulling this in costs far less precision than
        // it would with a conventional depth mapping.
        f64 near_plane = 0.05;

        // World -> view, full f64 including the translation.
        f64mat4 view() const;

        // Infinite reversed-Z: near maps to 1.0 and infinity to 0.0. There is
        // no far plane, so any distance in a star system fits one depth buffer.
        f64mat4 projection(f64 aspect) const;
    };
}
