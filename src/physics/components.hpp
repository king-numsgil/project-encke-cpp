#pragma once

#include "assets/model.hpp"

namespace encke::physics
{
    // On an entity simulated by the PhysicsWorld, whose Transform the world
    // writes after every step. The body is Jolt's, named by its index and
    // sequence number so this header stays free of Jolt.
    //
    // A body is in the world's space, a root with its pose in the world, or
    // in a ship's (ShipSpace), a child of the ship's entity with its pose in
    // the ship's frame. It moves between them as it crosses a ship's volume.
    //
    // The world steps at a fixed rate, which is not the frame rate, so a
    // frame falls between two steps: the Transform is the pose `alpha` of the
    // way from the previous step's to the latest's.
    struct RigidBody
    {
        u32 body = 0;

        // In the frame of the space the body is in.
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

        // The ship whose space it is in; null for the world's.
        entt::entity space = entt::null;
    };

    // On a ship's root entity: its inside is a physics space of its own,
    // fixed to the root's frame, so what is aboard simulates in small
    // numbers and in the ship's gravity, the world's at the ship less the
    // ship's own acceleration, whatever the ship does. A body or the
    // character crossing its volume moves between the world's space and
    // this one, keeping its pose and its velocity in the world.
    //
    // Its shapes are ShipShape entities: the volume is the union of its
    // Volume shapes, and its Hull shapes, once any exist, make the root a
    // dynamic body in the world of `mass` kilograms. Without them it holds
    // still where its Transform puts it.
    struct ShipSpace
    {
        f64 mass = 0.0;
    };

    // One convex piece of a ship, at its entity's world pose relative to
    // the ship's: a hull of `mesh`'s vertices, or a box of `half_extents`
    // about the entity's origin. A Hull piece is part of the ship's body in
    // the world; a Volume piece is part of the space it holds aboard.
    struct ShipShape
    {
        enum class Role : u8
        {
            Hull,
            Volume,
        };

        Role                                 role = Role::Hull;
        entt::entity                         ship = entt::null;
        NodeShape                            shape;
        std::shared_ptr<CollisionMesh const> mesh;   // for a convex hull
    };

    // On a ShipShape entity once the physics world has taken it in.
    struct ShipShapeTaken
    {
    };

    // On an entity the physics world holds still as a collider, from
    // PhysicsWorld::add_static_colliders, at its world pose and stretched by
    // its world scale, as its mesh is.
    struct StaticCollider
    {
        enum class Shape : u8
        {
            Box,      // the unit cube
            Sphere,   // the unit sphere; uniform scale, the diameter is the scale's x
            Mesh,     // `mesh`'s triangles, exactly
            Hull,     // the convex hull of `mesh`'s vertices: a simplified stand-in
        };

        Shape                                shape = Shape::Box;
        std::shared_ptr<CollisionMesh const> mesh;   // for Mesh and Hull

        // A ship with a ShipSpace whose space it is fixed in, and whether
        // it collides in the world's too: a hull is both, a deck only the
        // ship's.
        entt::entity space = entt::null;
        bool         world = true;
    };

    // On an entity with a StaticCollider once its bodies have been made, so
    // they are made once.
    struct StaticBody
    {
    };
}
