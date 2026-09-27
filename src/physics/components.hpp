#pragma once

namespace encke::physics
{
    // On an entity simulated by the PhysicsWorld: a root entity, whose
    // Transform the world writes after every step. The body is Jolt's, named
    // by its index and sequence number so this header stays free of Jolt.
    //
    // The world steps at a fixed rate, which is not the frame rate, so a
    // frame falls between two steps: the Transform is the pose `alpha` of the
    // way from the previous step's to the latest's.
    struct RigidBody
    {
        u32 body = 0;

        f64vec3 previous_position{0.0};
        f64vec3 position{0.0};
        f64quat previous_rotation{1.0, 0.0, 0.0, 0.0};
        f64quat rotation{1.0, 0.0, 0.0, 0.0};

        // The radius of a sphere about the body's centre that holds it: the
        // terrain it may touch is built this far out and a margin more.
        f64 bound = 0.0;

        // Held out of the simulation until the terrain around it is built,
        // so nothing falls through ground that is not there yet.
        bool added = false;
    };
}
