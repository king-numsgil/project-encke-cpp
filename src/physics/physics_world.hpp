#pragma once

#include <memory>

namespace encke
{
    class WorkerPool;
}

namespace encke::physics
{
    // Every rigid body in the scene, simulated by Jolt at a fixed 60 steps a
    // second, in f64 world positions (the library is built double-precision).
    // Gravity is each body's own: toward the centre of the nearest Body with
    // a surface gravity, falling off with the square of the distance.
    //
    // The ground is the terrain's, as collision meshes of chunks at one LOD,
    // config::kCollisionLod by default, built on the worker pool around each moving body
    // and kept. A body is held out of the simulation until the chunks around
    // it are built, and while any chunk a moving body needs is still being
    // built the world does not step at all: nothing falls through ground
    // that is on its way, and the simulation does not depend on how long the
    // pool took.
    //
    // Jolt's jobs run on its own thread pool, one thread per logical
    // processor the physical cores leave over (the hyperthreads beside the
    // terrain workers), plus the main thread, which works while it waits for
    // a step.
    //
    // Main thread only; chunk completions run in the pool's drain().
    class PhysicsWorld
    {
    public:
        PhysicsWorld();
        ~PhysicsWorld();

        PhysicsWorld(PhysicsWorld const&)            = delete;
        PhysicsWorld& operator=(PhysicsWorld const&) = delete;

        // Sets up Jolt with `threads` workers of its own; 0 steps on the
        // calling thread alone. Bodies collide with the terrain at
        // `collision_lod`.
        void init(u32 threads, u32 collision_lod);

        // A dynamic box on `entity`, which must be a root with a Transform:
        // its size is the Transform's scale, its pose the Transform's. Held
        // until the ground around it is built.
        void add_box(entt::registry& registry, entt::entity entity);

        // A dynamic sphere, its diameter the Transform's scale on x.
        void add_sphere(entt::registry& registry, entt::entity entity);

        // Builds the ground bodies need and steps. With `fixed_frame`, one
        // step exactly, whatever `seconds` says, so a scenario steps the same
        // however fast frames come; otherwise as many steps as `seconds` of
        // wall clock hold, at most a few, and the Transforms are interpolated
        // between the last two. Before Scene::update, which composes the
        // Transforms it writes.
        void update(entt::registry& registry, WorkerPool& pool, f64 seconds, bool fixed_frame);

        // Nothing held, no ground being built, every body asleep.
        bool idle() const;

        u32 bodies() const;
        u32 awake() const;

        // Collision chunks built or building, surface or not.
        u32 collision_chunks() const;

        // What a collision shape belongs to, for the debug view.
        enum class ShapeKind
        {
            Ground,   // a terrain collision chunk
            Held,     // a body waiting for its ground
            Awake,
            Asleep,
        };

        // Every body's collision shape as world-space triangles, three
        // points each, one call per body. Shapes come from Jolt's own
        // triangulation, so a sphere is a faceted one, and a box shows its
        // faces' diagonals.
        void collision_triangles(function<void(ShapeKind kind, span<f64vec3 const> triangles)> const& visit) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
